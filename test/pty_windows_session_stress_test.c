#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>
#include <tlhelp32.h>

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

static DWORD process_handle_count(void)
{
    DWORD count = 0;
    assert(GetProcessHandleCount(GetCurrentProcess(), &count));
    return count;
}

static DWORD process_thread_count(DWORD process_id)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    assert(snapshot != INVALID_HANDLE_VALUE);

    THREADENTRY32 entry;
    entry.dwSize = sizeof(entry);
    DWORD count = 0;
    BOOL has_entry = Thread32First(snapshot, &entry);
    while (has_entry) {
        if (entry.th32OwnerProcessID == process_id) count++;
        has_entry = Thread32Next(snapshot, &entry);
    }
    assert(GetLastError() == ERROR_NO_MORE_FILES);
    CloseHandle(snapshot);
    return count;
}

static DWORD child_process_count(DWORD parent_process_id)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    assert(snapshot != INVALID_HANDLE_VALUE);

    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(entry);
    DWORD count = 0;
    BOOL has_entry = Process32First(snapshot, &entry);
    while (has_entry) {
        if (entry.th32ParentProcessID == parent_process_id) count++;
        has_entry = Process32Next(snapshot, &entry);
    }
    assert(GetLastError() == ERROR_NO_MORE_FILES);
    CloseHandle(snapshot);
    return count;
}

int main(void)
{
    InitializeCriticalSection(&events.mutex);
    InitializeConditionVariable(&events.condition);
    Dart_PostCObject_DL = post_object;
    const DWORD baseline_handle_count = process_handle_count();
    const DWORD current_process_id = GetCurrentProcessId();
    const DWORD baseline_thread_count = process_thread_count(current_process_id);
    const DWORD baseline_child_count = child_process_count(current_process_id);

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
    assert(process_handle_count() == baseline_handle_count);
    assert(process_thread_count(current_process_id) == baseline_thread_count);
    assert(child_process_count(current_process_id) == baseline_child_count);
    DeleteCriticalSection(&events.mutex);
    return 0;
}
