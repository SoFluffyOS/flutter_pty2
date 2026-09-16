#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>

#include "../src/flutter_pty.h"
#include "../src/include/dart_api_dl.h"

typedef struct SessionEvents {
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE condition;
    int process_exit;
    int output_closed;
    int session_closed;
} SessionEvents;

static SessionEvents events;
static PtySession *active_session;

static bool post_object(Dart_Port_DL port, Dart_CObject *message)
{
    (void)port;
    assert(message != NULL);
    assert(message->type == Dart_CObject_kArray);
    assert(message->value.as_array.length >= 1);
    Dart_CObject **values = message->value.as_array.values;
    const int32_t event_type = values[0]->value.as_int32;
    uint64_t output_length_to_ack = 0;

    EnterCriticalSection(&events.mutex);
    switch (event_type) {
    case PTY_EVENT_OUTPUT:
        output_length_to_ack = (uint64_t)values[1]->value.as_typed_data.length;
        break;
    case PTY_EVENT_OUTPUT_CLOSED:
        events.output_closed = 1;
        break;
    case PTY_EVENT_PROCESS_EXIT:
        events.process_exit = 1;
        break;
    case PTY_EVENT_SESSION_CLOSED:
        events.session_closed = 1;
        break;
    default:
        break;
    }
    WakeAllConditionVariable(&events.condition);
    LeaveCriticalSection(&events.mutex);

    if (output_length_to_ack != 0 && active_session != NULL) {
        pty_session_ack_output(active_session, output_length_to_ack);
    }
    return true;
}

static void reset_events(void)
{
    EnterCriticalSection(&events.mutex);
    events.process_exit = 0;
    events.output_closed = 0;
    events.session_closed = 0;
    LeaveCriticalSection(&events.mutex);
}

static int wait_for_events(int wait_for_close)
{
    const ULONGLONG deadline = GetTickCount64() + 5000;
    EnterCriticalSection(&events.mutex);
    while (!events.process_exit || !events.output_closed ||
           (wait_for_close && !events.session_closed)) {
        const ULONGLONG now = GetTickCount64();
        const DWORD remaining = deadline > now
                                    ? (DWORD)(deadline - now)
                                    : 0;
        if (!SleepConditionVariableCS(&events.condition,
                                      &events.mutex,
                                      remaining)) {
            LeaveCriticalSection(&events.mutex);
            return 0;
        }
    }
    LeaveCriticalSection(&events.mutex);
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
            stats.pending_write_bytes == 0) {
            return 1;
        }
        Sleep(10);
    }
    return 0;
}

int main(void)
{
    InitializeCriticalSection(&events.mutex);
    InitializeConditionVariable(&events.condition);
    Dart_PostCObject_DL = post_object;

    int cycle_count = 1000;
    const char *configured_cycle_count = getenv("PTY_WINDOWS_STRESS_CYCLES");
    if (configured_cycle_count != NULL) {
        cycle_count = (int)strtol(configured_cycle_count, NULL, 10);
    }
    assert(cycle_count > 0);

    const char *fixture = getenv("PTY_TEST_CHILD_WINDOWS");
    if (fixture == NULL) fixture = "C:\\pty_test_child.exe";
    const char *arguments[] = {"exit", "0"};
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

    for (int cycle = 0; cycle < cycle_count; cycle++) {
        reset_events();
        PtySession *session = NULL;
        PtyError error;
        assert(pty_session_start(&options, &session, &error) == 1);
        assert(session != NULL);
        active_session = session;
        assert(wait_for_events(0));
        pty_session_begin_close(session);
        assert(wait_for_events(1));
        pty_session_release(session);
        active_session = NULL;
    }

    assert(wait_for_zero_resources());
    PtyDebugStats stats;
    pty_debug_get_stats(&stats);
    assert(stats.live_sessions == 0);
    assert(stats.live_read_workers == 0);
    assert(stats.live_write_workers == 0);
    assert(stats.live_wait_workers == 0);
    assert(stats.live_close_workers == 0);
    assert(stats.pending_write_chunks == 0);
    assert(stats.pending_write_bytes == 0);
    DeleteCriticalSection(&events.mutex);
    return 0;
}
