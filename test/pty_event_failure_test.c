#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
static volatile LONG post_count;
static volatile LONG spawned_event;
static volatile LONG reject_only_session_closed;
#else
#include <stdatomic.h>
#include <unistd.h>
static _Atomic int post_count;
static _Atomic int spawned_event;
static _Atomic int reject_only_session_closed;
#endif

#include "../src/flutter_pty.h"
#include "../src/include/dart_api_dl.h"

static int should_reject_only_session_closed(void)
{
#if defined(_WIN32)
    return InterlockedCompareExchange(&reject_only_session_closed, 0, 0) != 0;
#else
    return atomic_load_explicit(&reject_only_session_closed,
                                memory_order_acquire) != 0;
#endif
}

static void set_reject_only_session_closed(int value)
{
#if defined(_WIN32)
    InterlockedExchange(&reject_only_session_closed, value);
#else
    atomic_store_explicit(&reject_only_session_closed,
                          value,
                          memory_order_release);
#endif
}

static bool reject_post(Dart_Port_DL port, Dart_CObject *message)
{
    (void)port;
    const int32_t event_type =
        message->value.as_array.values[0]->value.as_int32;
    if (should_reject_only_session_closed() &&
        event_type != PTY_EVENT_SESSION_CLOSED) {
#if defined(_WIN32)
        if (event_type == PTY_EVENT_SPAWNED) {
            InterlockedExchange(&spawned_event, 1);
        }
#else
        if (event_type == PTY_EVENT_SPAWNED) {
            atomic_store_explicit(&spawned_event, 1, memory_order_release);
        }
#endif
        return true;
    }
#if defined(_WIN32)
    InterlockedIncrement(&post_count);
#else
    atomic_fetch_add_explicit(&post_count, 1, memory_order_relaxed);
#endif
    return false;
}

static int spawned_event_received(void)
{
#if defined(_WIN32)
    return InterlockedCompareExchange(&spawned_event, 0, 0) != 0;
#else
    return atomic_load_explicit(&spawned_event, memory_order_acquire) != 0;
#endif
}

static int posted_event_count(void)
{
#if defined(_WIN32)
    return (int)InterlockedCompareExchange(&post_count, 0, 0);
#else
    return atomic_load_explicit(&post_count, memory_order_acquire);
#endif
}

static void wait_milliseconds(unsigned int milliseconds)
{
#if defined(_WIN32)
    Sleep(milliseconds);
#else
    struct timespec delay = {
        .tv_sec = (time_t)(milliseconds / 1000),
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L,
    };
    nanosleep(&delay, NULL);
#endif
}

static int stats_are_zero(void)
{
    PtyDebugStats stats;
    pty_debug_get_stats(&stats);
    return stats.live_sessions == 0 && stats.live_read_workers == 0 &&
           stats.live_write_workers == 0 && stats.live_wait_workers == 0 &&
           stats.live_close_workers == 0 && stats.pending_write_chunks == 0 &&
           stats.pending_write_bytes == 0 && stats.inflight_write_bytes == 0;
}

int main(void)
{
    Dart_PostCObject_Type saved_post_object = Dart_PostCObject_DL;
    Dart_PostCObject_DL = NULL;
#if defined(_WIN32)
    const char *unavailable_executable = getenv("PTY_TEST_CHILD_WINDOWS");
    if (unavailable_executable == NULL) {
        unavailable_executable = "C:\\pty_test_child.exe";
    }
    const char *unavailable_arguments[] = {"exit", "0"};
    const char *unavailable_environment[] = {"PATH=C:\\Windows\\System32"};
#else
    const char *unavailable_executable = "/bin/sh";
    const char *unavailable_arguments[] = {"-c", "exit 0"};
    const char *unavailable_environment[] = {"PATH=/usr/bin:/bin"};
#endif
    const PtySpawnOptions unavailable_options = {
        .executable = unavailable_executable,
        .arguments = unavailable_arguments,
        .argument_count = 2,
        .environment = unavailable_environment,
        .environment_count = 1,
        .size = {.rows = 24, .columns = 80},
        .input_buffer_bytes = 64 * 1024,
        .output_window_bytes = 16 * 1024,
        .event_port = 1,
    };
    PtySession *unavailable_session = NULL;
    PtyError unavailable_error;
    assert(pty_session_start(&unavailable_options,
                              &unavailable_session,
                              &unavailable_error) == 0);
    assert(unavailable_session == NULL);
    assert(unavailable_error.domain == PTY_ERROR_DOMAIN_INTERNAL);
    assert(unavailable_error.kind == PTY_ERROR_INTERNAL);
    Dart_PostCObject_DL = saved_post_object;

    Dart_PostCObject_DL = reject_post;
#if defined(_WIN32)
    const char *executable = getenv("PTY_TEST_CHILD_WINDOWS");
    if (executable == NULL) executable = "C:\\pty_test_child.exe";
    const char *arguments[] = {"exit", "0"};
#else
    const char *executable = "/bin/sh";
    const char *arguments[] = {"-c", "sleep 30"};
#endif
    const char *environment[] = {
#if defined(_WIN32)
        "PATH=C:\\Windows\\System32",
#else
        "PATH=/usr/bin:/bin",
#endif
    };
    const PtySpawnOptions options = {
        .executable = executable,
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

    // A failed event post transfers the Dart owner reference to abandonment.
    // The test must not release session after native cleanup reaches zero.
    for (int attempt = 0; attempt < 500 && !stats_are_zero(); attempt++) {
        wait_milliseconds(10);
    }
    assert(stats_are_zero());
    assert(posted_event_count() == 1);

    set_reject_only_session_closed(1);
#if defined(_WIN32)
    InterlockedExchange(&spawned_event, 0);
#else
    atomic_store_explicit(&spawned_event, 0, memory_order_release);
#endif
    const char *close_arguments[] = {"-c", "sleep 30"};
    const PtySpawnOptions close_options = {
        .executable = executable,
        .arguments = close_arguments,
        .argument_count = 2,
        .environment = environment,
        .environment_count = 1,
        .size = {.rows = 24, .columns = 80},
        .input_buffer_bytes = 64 * 1024,
        .output_window_bytes = 16 * 1024,
        .event_port = 2,
    };
    session = NULL;
    assert(pty_session_start(&close_options, &session, &error) == 1);
    assert(session != NULL);
    for (int attempt = 0; attempt < 500 && !spawned_event_received();
         attempt++) {
        wait_milliseconds(10);
    }
    assert(spawned_event_received());
    pty_session_begin_close(session);
    for (int attempt = 0; attempt < 500 && !stats_are_zero(); attempt++) {
        wait_milliseconds(10);
    }
    // The rejected SESSION_CLOSED event abandons the native owner. Do not
    // release session here; the close-worker path already did so.
    assert(stats_are_zero());
    return 0;
}
