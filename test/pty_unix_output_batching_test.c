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
 * Output batching and ack wake-ups on Unix: small writes arrive in few
 * messages, acks wake the reactor only when its credit was exhausted, and no
 * output is lost or reordered under backpressure, including after the
 * program exits.
 */

typedef struct OutputEvents {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    uint8_t *output;
    size_t output_length;
    size_t output_capacity;
    size_t *message_lengths;
    size_t message_count;
    size_t message_capacity;
    size_t largest_message;
    size_t acked_bytes;
    size_t window;
    int window_exceeded;
    int output_after_close;
    int output_closed;
    int process_exit;
    int session_closed;
    int hold_first_output;
    int holding;
} OutputEvents;

static OutputEvents events = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER,
};

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
    events.output = NULL;
    events.output_length = 0;
    events.output_capacity = 0;
    events.message_lengths = NULL;
    events.message_count = 0;
    events.message_capacity = 0;
    events.largest_message = 0;
    events.acked_bytes = 0;
    events.window = window;
    events.window_exceeded = 0;
    events.output_after_close = 0;
    events.output_closed = 0;
    events.process_exit = 0;
    events.session_closed = 0;
    events.hold_first_output = 0;
    events.holding = 0;
    pthread_mutex_unlock(&events.mutex);
}

static bool post_object(Dart_Port_DL port, Dart_CObject *message)
{
    (void)port;
    assert(message != NULL);
    assert(message->type == Dart_CObject_kArray);
    Dart_CObject **values = message->value.as_array.values;
    const int32_t event_type = values[0]->value.as_int32;

    pthread_mutex_lock(&events.mutex);
    if (event_type == PTY_EVENT_OUTPUT) {
        assert(values[1]->type == Dart_CObject_kTypedData);
        const size_t length = (size_t)values[1]->value.as_typed_data.length;
        assert(length > 0);
        if (events.output_closed) events.output_after_close = 1;
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
        events.message_lengths[events.message_count++] = length;
        if (length > events.largest_message) events.largest_message = length;
        if (events.output_length - events.acked_bytes > events.window) {
            events.window_exceeded = 1;
        }
        if (events.hold_first_output) {
            // Keep the reactor inside this post, as a busy Dart isolate
            // would, until the test releases it.
            events.hold_first_output = 0;
            events.holding = 1;
            pthread_cond_broadcast(&events.condition);
            while (events.holding) {
                pthread_cond_wait(&events.condition, &events.mutex);
            }
        }
    } else if (event_type == PTY_EVENT_OUTPUT_CLOSED) {
        events.output_closed = 1;
    } else if (event_type == PTY_EVENT_PROCESS_EXIT) {
        events.process_exit = 1;
    } else if (event_type == PTY_EVENT_SESSION_CLOSED) {
        events.session_closed = 1;
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

/*
 * Acks every output message on this thread, one ack per message as the Dart
 * side does, optionally after a delay, until the output closes.
 */
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
        // Count the bytes as consumed before the ack can grant new credit.
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

static PtySession *start_shell(const char *command, uint64_t window)
{
    const char *arguments[] = {"-c", command};
    const char *environment[] = {"PATH=/usr/bin:/bin"};
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
    };
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

/* The bytes `seq 1 count` writes to a terminal, which maps \n to \r\n. */
static char *expected_seq_output(unsigned count, size_t *out_length)
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
    *out_length = length;
    return expected;
}

static void assert_seq_output(unsigned count)
{
    size_t expected_length = 0;
    char *expected = expected_seq_output(count, &expected_length);
    pthread_mutex_lock(&events.mutex);
    assert(events.output_length == expected_length);
    assert(memcmp(events.output, expected, expected_length) == 0);
    pthread_mutex_unlock(&events.mutex);
    free(expected);
}

static void test_ack_wake_decision(void)
{
    assert(pty_output_ack_wakes_reader(0, 1));
    assert(pty_output_ack_wakes_reader(0, 256 * 1024));
    assert(!pty_output_ack_wakes_reader(0, 0));
    assert(!pty_output_ack_wakes_reader(1, 2));
    assert(!pty_output_ack_wakes_reader(4096, 256 * 1024));
    assert(!pty_output_ack_wakes_reader(256 * 1024, 256 * 1024));
}

/* Acks that leave credit available never write to the wake pipe. */
static void test_acks_with_credit_do_not_wake(void)
{
    const uint64_t window = 8 * 1024 * 1024;
    reset_events(window);
    const uint64_t wakes_before = pty_debug_output_ack_wakes();
    PtySession *session = start_shell(
        "dd if=/dev/zero bs=4096 count=256 2>/dev/null", window);
    ack_until_output_closed(session, 0);
    finish_session(session);

    pthread_mutex_lock(&events.mutex);
    assert(events.output_length == 256 * 4096);
    assert(events.message_count > 0);
    pthread_mutex_unlock(&events.mutex);
    assert(pty_debug_output_ack_wakes() == wakes_before);
}

/*
 * The shell's echo builtin writes each line with its own write(). While the
 * reactor is busy posting the first line, the program writes the rest and
 * exits. The reactor then reads everything the terminal buffered and posts it
 * as one message instead of one message per read. Linux buffers all of it;
 * other systems may block the program before it ends, so there the test only
 * checks the bytes.
 */
static void test_buffered_small_writes_arrive_in_one_message(void)
{
    const unsigned lines = 2000;
    reset_events(8 * 1024 * 1024);
    pthread_mutex_lock(&events.mutex);
    events.hold_first_output = 1;
    pthread_mutex_unlock(&events.mutex);
    PtySession *session = start_shell(
        "i=1; while [ $i -le 2000 ]; do echo $i; i=$((i + 1)); done",
        8 * 1024 * 1024);

    const struct timespec deadline = deadline_after_seconds(2);
    pthread_mutex_lock(&events.mutex);
    while (!events.holding || !events.process_exit) {
        if (pthread_cond_timedwait(&events.condition,
                                   &events.mutex,
                                   &deadline) != 0) {
            break;
        }
    }
    const int program_finished_while_held =
        events.holding && events.process_exit;
    assert(events.holding);
    events.holding = 0;
    pthread_cond_broadcast(&events.condition);
    pthread_mutex_unlock(&events.mutex);

    ack_until_output_closed(session, 0);
    finish_session(session);
    assert_seq_output(lines);

    pthread_mutex_lock(&events.mutex);
    // The first message, then one batch with every other line (about 11 KiB);
    // a slow reactor may even read everything in the first. Posting each read
    // separately took four messages here, as a Linux pty read returns at most
    // about 4 KiB.
#if defined(__linux__)
    assert(program_finished_while_held);
    assert(events.message_count <= 2);
#else
    (void)program_finished_while_held;
#endif
    pthread_mutex_unlock(&events.mutex);
}

/*
 * A small window with slow acks parks the reactor on exhausted credit many
 * times. Every byte still arrives once and in order, the window is never
 * exceeded, the acks that refill the credit wake the reactor, and all output
 * is delivered before the output closes, although the program exits while
 * most of its output is still unread.
 */
static void test_backpressure_keeps_every_byte_in_order(void)
{
    const unsigned lines = 50000;
    const uint64_t window = 16 * 1024;
    reset_events(window);
    const uint64_t wakes_before = pty_debug_output_ack_wakes();
    PtySession *session = start_shell("seq 1 50000", window);
    ack_until_output_closed(session, 200);
    finish_session(session);
    assert_seq_output(lines);

    pthread_mutex_lock(&events.mutex);
    assert(!events.window_exceeded);
    assert(!events.output_after_close);
    assert(events.process_exit);
    assert(events.largest_message <= window);
    pthread_mutex_unlock(&events.mutex);
    assert(pty_debug_output_ack_wakes() > wakes_before);
}

int main(void)
{
    Dart_PostCObject_DL = post_object;
    test_ack_wake_decision();
    test_acks_with_credit_do_not_wake();
    test_buffered_small_writes_arrive_in_one_message();
    test_backpressure_keeps_every_byte_in_order();
    reset_events(0);
    return 0;
}
