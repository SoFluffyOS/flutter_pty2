#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "../src/flutter_pty.h"
#include "../src/include/dart_api_dl.h"

typedef struct OutputEvents {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    size_t output_bytes;
    int output_events;
    int session_closed;
} OutputEvents;

static OutputEvents events = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER,
};

static bool post_object(Dart_Port_DL port, Dart_CObject *message)
{
    (void)port;
    assert(message != NULL);
    assert(message->type == Dart_CObject_kArray);
    assert(message->value.as_array.length >= 1);
    const int32_t event_type = message->value.as_array.values[0]->value.as_int32;

    pthread_mutex_lock(&events.mutex);
    if (event_type == PTY_EVENT_OUTPUT) {
        assert(message->value.as_array.length == 2);
        Dart_CObject *payload = message->value.as_array.values[1];
        assert(payload->type == Dart_CObject_kTypedData);
        assert(payload->value.as_typed_data.length > 0);
        events.output_bytes += (size_t)payload->value.as_typed_data.length;
        events.output_events++;
    } else if (event_type == PTY_EVENT_SESSION_CLOSED) {
        events.session_closed = 1;
    }
    pthread_cond_broadcast(&events.condition);
    pthread_mutex_unlock(&events.mutex);
    return true;
}

static struct timespec deadline_after_seconds(long seconds)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += seconds;
    return deadline;
}

static int wait_for_output_bytes(size_t expected)
{
    const struct timespec deadline = deadline_after_seconds(5);
    pthread_mutex_lock(&events.mutex);
    while (events.output_bytes < expected) {
        if (pthread_cond_timedwait(&events.condition,
                                   &events.mutex,
                                   &deadline) != 0) {
            pthread_mutex_unlock(&events.mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&events.mutex);
    return 1;
}

static int wait_for_session_closed(void)
{
    const struct timespec deadline = deadline_after_seconds(5);
    pthread_mutex_lock(&events.mutex);
    while (!events.session_closed) {
        if (pthread_cond_timedwait(&events.condition,
                                   &events.mutex,
                                   &deadline) != 0) {
            pthread_mutex_unlock(&events.mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&events.mutex);
    return 1;
}

static int wait_for_zero_resources(void)
{
    for (int attempt = 0; attempt < 500; attempt++) {
        PtyDebugStats stats;
        pty_debug_get_stats(&stats);
        if (stats.live_sessions == 0 && stats.live_read_workers == 0 &&
            stats.live_write_workers == 0 && stats.live_wait_workers == 0 &&
            stats.live_close_workers == 0 && stats.pending_write_chunks == 0 &&
            stats.pending_write_bytes == 0 &&
            stats.inflight_write_bytes == 0) {
            return 1;
        }
        const struct timespec delay = {.tv_nsec = 10 * 1000 * 1000};
        nanosleep(&delay, NULL);
    }
    return 0;
}

static void sleep_milliseconds(unsigned int milliseconds)
{
    const struct timespec delay = {
        .tv_sec = (time_t)(milliseconds / 1000),
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L,
    };
    nanosleep(&delay, NULL);
}

int main(void)
{
    Dart_PostCObject_DL = post_object;
    const char *arguments[] = {
        "-c",
        "dd if=/dev/zero bs=4096 count=32 2>/dev/null",
    };
    const char *environment[] = {"PATH=/usr/bin:/bin"};
    const PtySpawnOptions options = {
        .executable = "/bin/sh",
        .arguments = arguments,
        .argument_count = 2,
        .environment = environment,
        .environment_count = 1,
        .size = {.rows = 24, .columns = 80},
        .input_buffer_bytes = 64 * 1024,
        .output_window_bytes = 16 * 1024,
        .event_port = 1,
    };
    PtySession *session = NULL;
    PtyError error;
    assert(pty_session_start(&options, &session, &error) == 1);
    assert(session != NULL);

    assert(wait_for_output_bytes(options.output_window_bytes));
    sleep_milliseconds(100);
    pthread_mutex_lock(&events.mutex);
    assert(events.output_bytes == options.output_window_bytes);
    assert(events.output_events > 0);
    pthread_mutex_unlock(&events.mutex);

    pty_session_begin_close(session);
    assert(wait_for_session_closed());
    pty_session_release(session);
    assert(wait_for_zero_resources());
    return 0;
}
