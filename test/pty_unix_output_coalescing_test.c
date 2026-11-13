#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/flutter_pty.h"
#include "../src/include/dart_api_dl.h"
#include "../src/pty_internal.h"

/*
 * Output coalescing on Unix (PtySpawnOptions.output_coalescing_us): output
 * read soon after the previous event is held until the delay after that
 * event, an isolated write is posted at once, nothing waits for the delay
 * once the program exits or the session closes, and no byte is lost or
 * reordered under backpressure.
 *
 * Most cases use a long delay (100 ms) so that the timing they check stands
 * well clear of scheduling noise.
 */

#define LONG_DELAY_US 100000u
#define MS 1000000u

typedef struct OutputEvents {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    uint8_t *output;
    size_t output_length;
    size_t output_capacity;
    size_t *message_lengths;
    uint64_t *message_times;
    size_t message_count;
    size_t message_capacity;
    size_t message_times_capacity;
    size_t largest_message;
    size_t acked_bytes;
    size_t window;
    int window_exceeded;
    int output_after_close;
    int output_closed;
    uint64_t output_closed_time;
    int process_exit;
    int output_after_exit;
    int session_closed;
    uint64_t session_closed_time;
} OutputEvents;

static OutputEvents events = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER,
};

static uint64_t now_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static void *grow(void *buffer, size_t *capacity, size_t needed, size_t size)
{
    if (needed <= *capacity) return buffer;
    size_t next = *capacity == 0 ? 1024 : *capacity;
    while (next < needed) next *= 2;
    void *grown = realloc(buffer, next * size);
    assert(grown != NULL);
    *capacity = next;
    return grown;
}

static void reset_events(size_t window)
{
    pthread_mutex_lock(&events.mutex);
    free(events.output);
    free(events.message_lengths);
    free(events.message_times);
    events.output = NULL;
    events.output_length = 0;
    events.output_capacity = 0;
    events.message_lengths = NULL;
    events.message_times = NULL;
    events.message_count = 0;
    events.message_capacity = 0;
    events.message_times_capacity = 0;
    events.largest_message = 0;
    events.acked_bytes = 0;
    events.window = window;
    events.window_exceeded = 0;
    events.output_after_close = 0;
    events.output_closed = 0;
    events.output_closed_time = 0;
    events.process_exit = 0;
    events.output_after_exit = 0;
    events.session_closed = 0;
    events.session_closed_time = 0;
    pthread_mutex_unlock(&events.mutex);
}

static bool post_object(Dart_Port_DL port, Dart_CObject *message)
{
    (void)port;
    assert(message != NULL);
    assert(message->type == Dart_CObject_kArray);
    Dart_CObject **values = message->value.as_array.values;
    const int32_t event_type = values[0]->value.as_int32;
    const uint64_t time = now_ns();

    pthread_mutex_lock(&events.mutex);
    if (event_type == PTY_EVENT_OUTPUT) {
        assert(values[1]->type == Dart_CObject_kTypedData);
        const size_t length = (size_t)values[1]->value.as_typed_data.length;
        assert(length > 0);
        if (events.output_closed) events.output_after_close = 1;
        if (events.process_exit) events.output_after_exit = 1;
        events.output = grow(events.output,
                             &events.output_capacity,
                             events.output_length + length,
                             1);
        memcpy(events.output + events.output_length,
               values[1]->value.as_typed_data.values,
               length);
        events.output_length += length;
        events.message_lengths = grow(events.message_lengths,
                                      &events.message_capacity,
                                      events.message_count + 1,
                                      sizeof(size_t));
        events.message_times = grow(events.message_times,
                                    &events.message_times_capacity,
                                    events.message_count + 1,
                                    sizeof(uint64_t));
        events.message_lengths[events.message_count] = length;
        events.message_times[events.message_count] = time;
        events.message_count++;
        if (length > events.largest_message) events.largest_message = length;
        if (events.output_length - events.acked_bytes > events.window) {
            events.window_exceeded = 1;
        }
    } else if (event_type == PTY_EVENT_OUTPUT_CLOSED) {
        events.output_closed = 1;
        events.output_closed_time = time;
    } else if (event_type == PTY_EVENT_PROCESS_EXIT) {
        events.process_exit = 1;
    } else if (event_type == PTY_EVENT_SESSION_CLOSED) {
        events.session_closed = 1;
        events.session_closed_time = time;
    }
    pthread_cond_broadcast(&events.condition);
    pthread_mutex_unlock(&events.mutex);
    return true;
}

static void sleep_microseconds(long microseconds)
{
    const struct timespec delay = {
        .tv_sec = microseconds / 1000000L,
        .tv_nsec = (microseconds % 1000000L) * 1000L,
    };
    nanosleep(&delay, NULL);
}

static struct timespec deadline_after_seconds(long seconds)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += seconds;
    return deadline;
}

/* Waits for at least `count` output messages; the mutex is held on return. */
static void lock_and_wait_for_messages(size_t count)
{
    const struct timespec deadline = deadline_after_seconds(10);
    pthread_mutex_lock(&events.mutex);
    while (events.message_count < count) {
        const int wait_result = pthread_cond_timedwait(&events.condition,
                                                       &events.mutex,
                                                       &deadline);
        assert(wait_result == 0);
    }
}

/* Acks every output message, optionally after a delay, until output closes. */
static void ack_until_output_closed(PtySession *session, long ack_delay_us)
{
    const struct timespec deadline = deadline_after_seconds(60);
    size_t acked_messages = 0;
    pthread_mutex_lock(&events.mutex);
    while (true) {
        while (acked_messages == events.message_count &&
               !events.output_closed) {
            const int wait_result = pthread_cond_timedwait(&events.condition,
                                                           &events.mutex,
                                                           &deadline);
            assert(wait_result == 0);
        }
        if (acked_messages == events.message_count) break;
        const size_t length = events.message_lengths[acked_messages++];
        pthread_mutex_unlock(&events.mutex);
        if (ack_delay_us > 0) sleep_microseconds(ack_delay_us);
        pthread_mutex_lock(&events.mutex);
        events.acked_bytes += length;
        pthread_mutex_unlock(&events.mutex);
        pty_session_ack_output(session, length);
        pthread_mutex_lock(&events.mutex);
    }
    pthread_mutex_unlock(&events.mutex);
}

static void wait_for_session_closed(void)
{
    const struct timespec deadline = deadline_after_seconds(10);
    pthread_mutex_lock(&events.mutex);
    while (!events.session_closed) {
        const int wait_result = pthread_cond_timedwait(&events.condition,
                                                       &events.mutex,
                                                       &deadline);
        assert(wait_result == 0);
    }
    pthread_mutex_unlock(&events.mutex);
}

static PtySpawnOptions shell_options(const char *const *arguments,
                                     uint64_t window,
                                     uint32_t coalescing_us)
{
    static const char *environment[] = {"PATH=/usr/bin:/bin"};
    const PtySpawnOptions options = {
        .executable = "/bin/sh",
        .arguments = arguments,
        .argument_count = 2,
        .environment = environment,
        .environment_count = 1,
        .size = {.rows = 24, .columns = 80},
        .input_buffer_bytes = 64 * 1024,
        .output_window_bytes = window,
        .event_port = 1,
        .output_coalescing_us = coalescing_us,
    };
    return options;
}

static PtySession *start_shell(const char *command,
                               uint64_t window,
                               uint32_t coalescing_us)
{
    const char *arguments[] = {"-c", command};
    const PtySpawnOptions options =
        shell_options(arguments, window, coalescing_us);
    PtySession *session = NULL;
    PtyError error;
    assert(pty_session_start(&options, &session, &error) == 1);
    assert(session != NULL);
    return session;
}

static void finish_session(PtySession *session)
{
    pty_session_begin_close(session);
    wait_for_session_closed();
    pty_session_release(session);
}

static void assert_output_equals(const char *expected, size_t expected_length)
{
    pthread_mutex_lock(&events.mutex);
    assert(events.output_length == expected_length);
    assert(memcmp(events.output, expected, expected_length) == 0);
    pthread_mutex_unlock(&events.mutex);
}

/* The bytes `seq 1 count` writes to a terminal, which maps \n to \r\n. */
static void assert_seq_output(unsigned count)
{
    size_t capacity = (size_t)count * 12 + 1;
    char *expected = malloc(capacity);
    assert(expected != NULL);
    size_t length = 0;
    for (unsigned value = 1; value <= count; value++) {
        length += (size_t)snprintf(expected + length,
                                   capacity - length,
                                   "%u\r\n",
                                   value);
    }
    assert_output_equals(expected, length);
    free(expected);
}

static void test_hold_deadline(void)
{
    const uint64_t delay = 2 * MS;
    // Disabled, nothing posted yet, or the previous event is old: post now.
    assert(pty_output_hold_deadline(10 * MS, 9 * MS, 0) == 0);
    assert(pty_output_hold_deadline(10 * MS, 0, delay) == 0);
    assert(pty_output_hold_deadline(10 * MS, 8 * MS, delay) == 0);
    assert(pty_output_hold_deadline(10 * MS, 1 * MS, delay) == 0);
    // A clock that went backwards never holds.
    assert(pty_output_hold_deadline(10 * MS, 11 * MS, delay) == 0);
    // Back to back: hold until the delay after the previous event.
    assert(pty_output_hold_deadline(10 * MS, 9 * MS, delay) == 11 * MS);
    assert(pty_output_hold_deadline(10 * MS, 10 * MS, delay) == 12 * MS);
    assert(pty_output_hold_deadline(10 * MS, 8 * MS + 1, delay) ==
           10 * MS + 1);
}

static void test_rejects_too_long_delay(void)
{
    const char *arguments[] = {"-c", "true"};
    const PtySpawnOptions options = shell_options(
        arguments, 16 * 1024, PTY_MAX_OUTPUT_COALESCING_US + 1);
    PtySession *session = NULL;
    PtyError error;
    assert(pty_session_start(&options, &session, &error) == 0);
    assert(session == NULL);
    assert(error.kind == PTY_ERROR_INVALID_ARGUMENT);
}

/* Writes one byte to the terminal; its echo is the program's output. */
static void type_byte(PtySession *session, uint64_t request_id, uint8_t byte)
{
    PtyError error;
    assert(pty_session_try_write(session, request_id, &byte, 1, &error) ==
           PTY_WRITE_ACCEPTED);
}

/*
 * A keystroke after a pause is echoed at once, however long the delay. A
 * second one right after it is held, but not past the delay after the first
 * echo.
 */
static void test_isolated_echo_is_not_delayed(void)
{
    reset_events(1024 * 1024);
    PtySession *session = start_shell("cat >/dev/null", 1024 * 1024,
                                      LONG_DELAY_US);
    sleep_microseconds(200000);

    uint64_t typed = now_ns();
    type_byte(session, 1, 'a');
    lock_and_wait_for_messages(1);
    const uint64_t first_latency = events.message_times[0] - typed;
    pthread_mutex_unlock(&events.mutex);
    pty_session_ack_output(session, 1);

    sleep_microseconds(200000);
    typed = now_ns();
    type_byte(session, 2, 'b');
    lock_and_wait_for_messages(2);
    const uint64_t second_latency = events.message_times[1] - typed;
    const uint64_t second_post = events.message_times[1];
    pthread_mutex_unlock(&events.mutex);
    pty_session_ack_output(session, 1);

    // Back to back: typed 10 ms after the previous echo was posted.
    sleep_microseconds(10000);
    type_byte(session, 3, 'c');
    lock_and_wait_for_messages(3);
    const uint64_t held_gap = events.message_times[2] - second_post;
    pthread_mutex_unlock(&events.mutex);
    pty_session_ack_output(session, 1);

    finish_session(session);
    assert_output_equals("abc", 3);
    printf("echo latency: %.2f ms, %.2f ms; back-to-back gap %.2f ms\n",
           first_latency / 1e6, second_latency / 1e6, held_gap / 1e6);
    assert(first_latency < 40 * MS);
    assert(second_latency < 40 * MS);
    assert(held_gap >= 80 * MS);
    assert(held_gap < (LONG_DELAY_US / 1000 + 60) * MS);
}

/*
 * Lines written a few milliseconds apart arrive in a few events with the
 * delay, and in about one event per line without it.
 */
static size_t burst_messages(uint32_t coalescing_us)
{
    reset_events(1024 * 1024);
    PtySession *session = start_shell(
        "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do "
        "echo $i; sleep 0.005; done; sleep 0.3",
        1024 * 1024,
        coalescing_us);
    ack_until_output_closed(session, 0);
    finish_session(session);
    assert_seq_output(20);
    pthread_mutex_lock(&events.mutex);
    const size_t count = events.message_count;
    pthread_mutex_unlock(&events.mutex);
    return count;
}

static void test_bursts_are_batched(void)
{
    const size_t without = burst_messages(0);
    const size_t with = burst_messages(50000);
    printf("burst of 20 lines: %zu messages without, %zu with 50 ms\n",
           without, with);
    assert(without >= 10);
    // The burst lasts at least 100 ms: the first line at once, then about
    // one event per 50 ms.
    assert(with <= 6);
}

/*
 * The program's exit ends a hold at once: the last output and the output's
 * close follow the first event well within the delay.
 */
static void test_exit_ends_hold(void)
{
    reset_events(1024 * 1024);
    PtySession *session = start_shell(
        "i=1; while [ $i -le 2000 ]; do echo $i; i=$((i + 1)); done",
        1024 * 1024,
        LONG_DELAY_US);
    ack_until_output_closed(session, 0);
    pthread_mutex_lock(&events.mutex);
    const uint64_t drain_time =
        events.output_closed_time - events.message_times[0];
    const int output_after_close = events.output_after_close;
    pthread_mutex_unlock(&events.mutex);
    finish_session(session);
    assert_seq_output(2000);
    printf("exit drain after the first event: %.2f ms\n", drain_time / 1e6);
    assert(!output_after_close);
    assert(drain_time < 80 * MS);
}

/*
 * Output held when the program exits is posted before the exit: the waiter
 * waits for the hold to end. The program exits 20 ms into a 100 ms hold of
 * "b". (Output the program writes just before it exits can still race with
 * the exit report, with or without coalescing; this case avoids that race.)
 */
static void test_held_output_precedes_exit(void)
{
    for (int cycle = 0; cycle < 20; cycle++) {
        reset_events(1024 * 1024);
        PtySession *session = start_shell(
            "printf a; sleep 0.005; printf b; sleep 0.02", 1024 * 1024,
            LONG_DELAY_US);
        ack_until_output_closed(session, 0);
        finish_session(session);
        assert_output_equals("ab", 2);
        pthread_mutex_lock(&events.mutex);
        assert(events.process_exit);
        assert(!events.output_after_exit);
        pthread_mutex_unlock(&events.mutex);
    }
}

/* Closing the session ends a hold at once. */
static void test_close_ends_hold(void)
{
    reset_events(1024 * 1024);
    PtySession *session = start_shell(
        "echo a; sleep 0.01; echo b; sleep 0.01; echo c; sleep 10",
        1024 * 1024,
        LONG_DELAY_US);
    lock_and_wait_for_messages(1);
    pthread_mutex_unlock(&events.mutex);
    sleep_microseconds(30000);
    const uint64_t closing = now_ns();
    pty_session_begin_close(session);
    wait_for_session_closed();
    pty_session_release(session);
    pthread_mutex_lock(&events.mutex);
    const uint64_t close_time = events.session_closed_time - closing;
    pthread_mutex_unlock(&events.mutex);
    printf("close during a hold: %.2f ms\n", close_time / 1e6);
    assert(close_time < 60 * MS);
}

/*
 * A small window with slow acks: every byte arrives once and in order, the
 * window is never exceeded, and no output follows the output's close.
 */
static void test_backpressure_keeps_every_byte_in_order(void)
{
    const unsigned lines = 50000;
    const uint64_t window = 16 * 1024;
    reset_events(window);
    PtySession *session = start_shell("seq 1 50000", window, 2000);
    ack_until_output_closed(session, 200);
    finish_session(session);
    assert_seq_output(lines);

    pthread_mutex_lock(&events.mutex);
    assert(!events.window_exceeded);
    assert(!events.output_after_close);
    assert(events.process_exit);
    assert(events.largest_message <= window);
    pthread_mutex_unlock(&events.mutex);
}

/* A fast writer with the default-sized window: exact bytes, bounded events. */
static void test_flood_keeps_every_byte_in_order(void)
{
    const unsigned lines = 300000;
    reset_events(256 * 1024);
    PtySession *session = start_shell("seq 1 300000", 256 * 1024, 2000);
    ack_until_output_closed(session, 0);
    finish_session(session);
    assert_seq_output(lines);
    pthread_mutex_lock(&events.mutex);
    assert(!events.window_exceeded);
    assert(!events.output_after_close);
    assert(events.largest_message <= 64 * 1024);
    printf("seq 1 300000 with 2 ms: %zu messages\n", events.message_count);
    pthread_mutex_unlock(&events.mutex);
}

int main(void)
{
    Dart_PostCObject_DL = post_object;
    test_hold_deadline();
    test_rejects_too_long_delay();
    test_isolated_echo_is_not_delayed();
    test_bursts_are_batched();
    test_exit_ends_hold();
    test_held_output_precedes_exit();
    test_close_ends_hold();
    test_backpressure_keeps_every_byte_in_order();
    test_flood_keeps_every_byte_in_order();
    reset_events(0);
    return 0;
}
