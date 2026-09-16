#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../src/flutter_pty.h"
#include "../src/include/dart_api_dl.h"

typedef struct SessionEvents {
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE condition;
    int spawned;
    int output_closed;
    int process_exit;
    int session_closed;
    DWORD exit_code;
    size_t output_length;
} SessionEvents;

static SessionEvents events;
static PtySession *active_session;
static int discard_after_spawn;
static volatile LONG discard_complete;
static volatile LONG output_after_discard;

static bool post_object(Dart_Port_DL port, Dart_CObject *message)
{
    (void)port;
    assert(message != NULL);
    assert(message->type == Dart_CObject_kArray);
    assert(message->value.as_array.length >= 1);
    Dart_CObject **values = message->value.as_array.values;
    const int32_t event_type = values[0]->value.as_int32;
    uint64_t output_length_to_ack = 0;
    int discard_now = 0;

    EnterCriticalSection(&events.mutex);
    switch (event_type) {
    case PTY_EVENT_SPAWNED:
        events.spawned = 1;
        discard_now = discard_after_spawn;
        break;
    case PTY_EVENT_OUTPUT:
        assert(values[1]->type == Dart_CObject_kTypedData);
        output_length_to_ack = (uint64_t)values[1]->value.as_typed_data.length;
        if (InterlockedCompareExchange(&discard_complete, 0, 0) != 0) {
            InterlockedIncrement(&output_after_discard);
        }
        events.output_length += (size_t)output_length_to_ack;
        break;
    case PTY_EVENT_OUTPUT_CLOSED:
        events.output_closed = 1;
        break;
    case PTY_EVENT_PROCESS_EXIT:
        events.process_exit = 1;
        events.exit_code = (DWORD)values[2]->value.as_int64;
        break;
    case PTY_EVENT_SESSION_CLOSED:
        events.session_closed = 1;
        break;
    default:
        break;
    }
    WakeAllConditionVariable(&events.condition);
    LeaveCriticalSection(&events.mutex);
    if (discard_now && active_session != NULL) {
        pty_session_discard_output(active_session);
        InterlockedExchange(&discard_complete, 1);
    }
    if (output_length_to_ack != 0 && active_session != NULL) {
        pty_session_ack_output(active_session, output_length_to_ack);
    }
    return true;
}

static void reset_events(void)
{
    EnterCriticalSection(&events.mutex);
    events.spawned = 0;
    events.output_closed = 0;
    events.process_exit = 0;
    events.session_closed = 0;
    events.exit_code = 0;
    events.output_length = 0;
    LeaveCriticalSection(&events.mutex);
    InterlockedExchange(&discard_complete, 0);
    InterlockedExchange(&output_after_discard, 0);
}

static int wait_for_events(int wait_for_close)
{
    const DWORD deadline = GetTickCount() + 5000;
    EnterCriticalSection(&events.mutex);
    while (!events.process_exit || !events.output_closed ||
           (wait_for_close && !events.session_closed)) {
        const DWORD now = GetTickCount();
        const DWORD remaining = deadline > now ? deadline - now : 0;
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

int main(void)
{
    InitializeCriticalSection(&events.mutex);
    InitializeConditionVariable(&events.condition);
    Dart_PostCObject_DL = post_object;
    const char *fixture = getenv("PTY_TEST_CHILD_WINDOWS");
    if (fixture == NULL) fixture = "C:\\pty_test_child.exe";
    const char *arguments[] = {"echo", "native-session"};
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
    active_session = session;
    assert(wait_for_events(0));
    assert(events.spawned == 1);
    assert(events.exit_code == 0);
    assert(events.output_length >= strlen("native-session"));
    assert(events.output_closed == 1);
    PtyError signal_error;
    assert(pty_session_send_signal(session, 15, 0, &signal_error) == 0);
    assert(signal_error.kind == PTY_ERROR_UNSUPPORTED);
    pty_session_begin_close(session);
    assert(wait_for_events(1));
    pty_session_release(session);

    reset_events();
    discard_after_spawn = 1;
    const char *discard_arguments[] = {"delayed-flood", "250", "1048576"};
    const PtySpawnOptions discard_options = {
        .executable = fixture,
        .arguments = discard_arguments,
        .argument_count = 3,
        .environment = environment,
        .environment_count = 1,
        .size = {.rows = 24, .columns = 80},
        .input_buffer_bytes = 64 * 1024,
        .output_window_bytes = 16 * 1024,
        .event_port = 1,
    };
    session = NULL;
    assert(pty_session_start(&discard_options, &session, &error) == 1);
    assert(session != NULL);
    active_session = session;
    assert(wait_for_events(0));
    assert(events.spawned == 1);
    assert(events.output_length == 0);
    assert(InterlockedCompareExchange(&output_after_discard, 0, 0) == 0);
    pty_session_begin_close(session);
    assert(wait_for_events(1));
    pty_session_release(session);
    active_session = NULL;
    DeleteCriticalSection(&events.mutex);
    return 0;
}
