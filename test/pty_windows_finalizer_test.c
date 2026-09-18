#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>

#include "../src/flutter_pty.h"
#include "../src/include/dart_api_dl.h"

static volatile LONG spawned;

static bool post_object(Dart_Port_DL port, Dart_CObject *message)
{
    (void)port;
    assert(message != NULL);
    assert(message->type == Dart_CObject_kArray);
    const int32_t event_type =
        message->value.as_array.values[0]->value.as_int32;
    if (event_type == PTY_EVENT_SPAWNED) {
        InterlockedExchange(&spawned, 1);
    }
    return true;
}

static int spawned_event_received(void)
{
    return InterlockedCompareExchange(&spawned, 0, 0) != 0;
}

static int wait_for_zero_resources(void)
{
    for (int attempt = 0; attempt < 500; attempt++) {
        PtyDebugStats stats;
        pty_debug_get_stats(&stats);
        if (stats.live_sessions == 0 && stats.live_read_workers == 0 &&
            stats.live_write_workers == 0 && stats.live_wait_workers == 0 &&
            stats.live_close_workers == 0 &&
            stats.live_pseudo_console_workers == 0 &&
            stats.pending_write_chunks == 0 &&
            stats.pending_write_bytes == 0 &&
            stats.inflight_write_bytes == 0) {
            return 1;
        }
        Sleep(10);
    }
    return 0;
}

int main(void)
{
    Dart_PostCObject_DL = post_object;
    _putenv("PTY_TEST_FORCE_CLOSE_THREAD_FAILURE=1");

    const char *fixture = getenv("PTY_TEST_CHILD_WINDOWS");
    if (fixture == NULL) fixture = "C:\\pty_test_child.exe";
    const char *arguments[] = {"sleep", "30"};
    const char *environment[] = {"PATH=C:\\Windows\\System32"};
    const PtySpawnOptions options = {
        .executable = fixture,
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
    for (int attempt = 0; attempt < 500 && !spawned_event_received();
         attempt++) {
        Sleep(10);
    }
    assert(spawned_event_received());

    const ULONGLONG start = GetTickCount64();
    pty_session_abandon(session);
    const ULONGLONG elapsed = GetTickCount64() - start;
    assert(elapsed < 1000);
    assert(wait_for_zero_resources());
    return 0;
}
