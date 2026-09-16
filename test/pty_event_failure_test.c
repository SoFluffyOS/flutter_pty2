#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
static volatile LONG post_count;
#else
#include <stdatomic.h>
#include <unistd.h>
static _Atomic int post_count;
#endif

#include "../src/flutter_pty.h"
#include "../src/include/dart_api_dl.h"

static bool reject_post(Dart_Port_DL port, Dart_CObject *message)
{
    (void)port;
    (void)message;
#if defined(_WIN32)
    InterlockedIncrement(&post_count);
#else
    atomic_fetch_add_explicit(&post_count, 1, memory_order_relaxed);
#endif
    return false;
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
           stats.pending_write_bytes == 0;
}

int main(void)
{
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
    return 0;
}
