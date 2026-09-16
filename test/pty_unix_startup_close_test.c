#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "../src/flutter_pty.h"
#include "../src/include/dart_api_dl.h"

static pthread_mutex_t events_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t events_condition = PTHREAD_COND_INITIALIZER;
static int session_closed;
static Dart_Port_DL expected_port;

static bool post_object(Dart_Port_DL port, Dart_CObject *message)
{
    (void)port;
    assert(message != NULL);
    assert(message->type == Dart_CObject_kArray);
    assert(message->value.as_array.length >= 1);

    const int32_t event_type =
        message->value.as_array.values[0]->value.as_int32;
    pthread_mutex_lock(&events_mutex);
    if (event_type == PTY_EVENT_SESSION_CLOSED && port == expected_port) {
        session_closed = 1;
    }
    pthread_cond_broadcast(&events_condition);
    pthread_mutex_unlock(&events_mutex);
    return true;
}

static void reset_events(Dart_Port_DL port)
{
    pthread_mutex_lock(&events_mutex);
    expected_port = port;
    session_closed = 0;
    pthread_mutex_unlock(&events_mutex);
}

static int wait_for_close(void)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    pthread_mutex_lock(&events_mutex);
    while (!session_closed) {
        if (pthread_cond_timedwait(&events_condition,
                                   &events_mutex,
                                   &deadline) != 0) {
            pthread_mutex_unlock(&events_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&events_mutex);
    return 1;
}

static int wait_for_cleanup(void)
{
    for (int attempt = 0; attempt < 500; attempt++) {
        PtyDebugStats stats;
        pty_debug_get_stats(&stats);
        if (stats.live_sessions == 0 && stats.live_read_workers == 0 &&
            stats.live_write_workers == 0 && stats.live_wait_workers == 0 &&
            stats.live_close_workers == 0 && stats.pending_write_chunks == 0 &&
            stats.pending_write_bytes == 0) {
            return 1;
        }
        const struct timespec pause = {.tv_nsec = 10 * 1000 * 1000};
        nanosleep(&pause, NULL);
    }
    return 0;
}

int main(void)
{
    Dart_PostCObject_DL = post_object;
    int cycle_count = 100;
    const char *configured_cycle_count = getenv("PTY_STARTUP_CLOSE_CYCLES");
    if (configured_cycle_count != NULL) {
        cycle_count = (int)strtol(configured_cycle_count, NULL, 10);
    }
    assert(cycle_count > 0);

    const char *arguments[] = {"-c", "sleep 1"};
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

    for (int cycle = 0; cycle < cycle_count; cycle++) {
        const Dart_Port_DL event_port = (Dart_Port_DL)(cycle + 1);
        reset_events(event_port);
        PtySession *session = NULL;
        PtyError error;
        PtySpawnOptions cycle_options = options;
        cycle_options.event_port = event_port;
        assert(pty_session_start(&cycle_options, &session, &error) == 1);
        assert(session != NULL);
        pty_session_begin_close(session);
        pty_session_begin_close(session);
        assert(wait_for_close());
        pty_session_release(session);
    }

    assert(wait_for_cleanup());
    PtyDebugStats stats;
    pty_debug_get_stats(&stats);
    assert(stats.live_sessions == 0);
    assert(stats.live_read_workers == 0);
    assert(stats.live_write_workers == 0);
    assert(stats.live_wait_workers == 0);
    assert(stats.live_close_workers == 0);
    assert(stats.pending_write_chunks == 0);
    assert(stats.pending_write_bytes == 0);
    return 0;
}
