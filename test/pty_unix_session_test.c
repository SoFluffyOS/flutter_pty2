#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "../src/flutter_pty.h"
#include "../src/include/dart_api_dl.h"

typedef struct SessionEvents {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int spawned;
    int write_complete;
    int output_closed;
    int process_exit;
    int session_closed;
    int exit_code;
    char output[128];
    size_t output_length;
} SessionEvents;

static SessionEvents events = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER,
};
static PtySession *active_session;

static void reset_events(void)
{
    pthread_mutex_lock(&events.mutex);
    events.spawned = 0;
    events.write_complete = 0;
    events.output_closed = 0;
    events.process_exit = 0;
    events.session_closed = 0;
    events.exit_code = 0;
    events.output_length = 0;
    pthread_mutex_unlock(&events.mutex);
}

static int contains_bytes(const char *bytes,
                          size_t length,
                          const char *needle,
                          size_t needle_length)
{
    if (needle_length > length) return 0;
    for (size_t index = 0; index <= length - needle_length; index++) {
        if (memcmp(bytes + index, needle, needle_length) == 0) return 1;
    }
    return 0;
}

static bool post_object(Dart_Port_DL port, Dart_CObject *message)
{
    (void)port;
    assert(message != NULL);
    assert(message->type == Dart_CObject_kArray);
    assert(message->value.as_array.length >= 1);
    Dart_CObject **values = message->value.as_array.values;
    const int32_t event_type = values[0]->value.as_int32;
    uint64_t output_length_to_ack = 0;

    pthread_mutex_lock(&events.mutex);
    switch (event_type) {
    case PTY_EVENT_SPAWNED:
        events.spawned = 1;
        break;
    case PTY_EVENT_OUTPUT:
        assert(values[1]->type == Dart_CObject_kTypedData);
        const size_t length = (size_t)values[1]->value.as_typed_data.length;
        assert(events.output_length + length <= sizeof(events.output));
        memcpy(events.output + events.output_length,
               values[1]->value.as_typed_data.values,
               length);
        events.output_length += length;
        output_length_to_ack = length;
        break;
    case PTY_EVENT_WRITE_COMPLETE:
        events.write_complete = 1;
        break;
    case PTY_EVENT_OUTPUT_CLOSED:
        events.output_closed = 1;
        break;
    case PTY_EVENT_PROCESS_EXIT:
        events.process_exit = 1;
        assert(values[1]->value.as_int32 == 0);
        events.exit_code = (int)values[2]->value.as_int64;
        break;
    case PTY_EVENT_SESSION_CLOSED:
        events.session_closed = 1;
        break;
    default:
        break;
    }
    pthread_cond_broadcast(&events.condition);
    pthread_mutex_unlock(&events.mutex);
    if (output_length_to_ack != 0 && active_session != NULL) {
        pty_session_ack_output(active_session, output_length_to_ack);
    }
    return true;
}

static struct timespec deadline_after_seconds(long seconds)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += seconds;
    return deadline;
}

static int wait_for_process_and_output(void)
{
    const struct timespec deadline = deadline_after_seconds(5);
    pthread_mutex_lock(&events.mutex);
    while (!events.process_exit || !events.output_closed) {
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

static int wait_for_spawn(void)
{
    const struct timespec deadline = deadline_after_seconds(5);
    pthread_mutex_lock(&events.mutex);
    while (!events.spawned) {
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

static int wait_for_write_complete(void)
{
    const struct timespec deadline = deadline_after_seconds(5);
    pthread_mutex_lock(&events.mutex);
    while (!events.write_complete) {
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

int main(void)
{
    Dart_PostCObject_DL = post_object;
    const char *arguments[] = {
        "-c",
        "IFS= read line; printf 'got:%s' \"$line\"; exit 7",
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
        .output_window_bytes = 8,
        .event_port = 1,
    };
    PtySession *session = NULL;
    PtyError error;
    assert(pty_session_start(&options, &session, &error) == 1);
    assert(session != NULL);
    active_session = session;
    assert(wait_for_spawn());
    const uint8_t byte = 1;
    assert(pty_session_try_write(NULL, 42, &byte, 1, &error) ==
           PTY_WRITE_ERROR);
    assert(error.kind == PTY_ERROR_INVALID_ARGUMENT);
    assert(pty_session_try_write(session, 42, NULL, 1, &error) ==
           PTY_WRITE_ERROR);
    assert(error.kind == PTY_ERROR_INVALID_ARGUMENT);
    assert(pty_session_try_write(session, 42, &byte, 0, &error) ==
           PTY_WRITE_ERROR);
    assert(error.kind == PTY_ERROR_INVALID_ARGUMENT);
    PtySize invalid_size = {.rows = 0, .columns = 80};
    assert(pty_session_resize(session, invalid_size, &error) == 0);
    assert(error.kind == PTY_ERROR_INVALID_ARGUMENT);
    const uint8_t input[] = {'h', 'e', 'l', 'l', 'o', '\n'};
    assert(pty_session_try_write(session,
                                 42,
                                 input,
                                 sizeof(input),
                                 &error) == PTY_WRITE_ACCEPTED);
    assert(wait_for_write_complete());
    assert(wait_for_process_and_output());

    pthread_mutex_lock(&events.mutex);
    assert(events.spawned == 1);
    assert(events.exit_code == 7);
    assert(events.output_length >= strlen("got:hello"));
    assert(contains_bytes(events.output,
                          events.output_length,
                          "got:hello",
                          strlen("got:hello")));
    pthread_mutex_unlock(&events.mutex);

    pty_session_begin_close(session);
    const struct timespec deadline = deadline_after_seconds(5);
    pthread_mutex_lock(&events.mutex);
    while (!events.session_closed) {
        if (pthread_cond_timedwait(&events.condition,
                                   &events.mutex,
                                   &deadline) != 0) {
            pthread_mutex_unlock(&events.mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&events.mutex);
    pty_session_release(session);
    active_session = NULL;

    reset_events();
    const char *discard_arguments[] = {
        "-c",
        "sleep 1; printf discard-output; exit 0",
    };
    PtySpawnOptions discard_options = options;
    discard_options.arguments = discard_arguments;
    assert(pty_session_start(&discard_options, &session, &error) == 1);
    assert(session != NULL);
    active_session = session;
    assert(wait_for_spawn());
    pty_session_discard_output(session);
    assert(wait_for_process_and_output());
    pthread_mutex_lock(&events.mutex);
    assert(events.output_length == 0);
    pthread_mutex_unlock(&events.mutex);
    pty_session_begin_close(session);
    const struct timespec discard_deadline = deadline_after_seconds(5);
    pthread_mutex_lock(&events.mutex);
    while (!events.session_closed) {
        if (pthread_cond_timedwait(&events.condition,
                                   &events.mutex,
                                   &discard_deadline) != 0) {
            pthread_mutex_unlock(&events.mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&events.mutex);
    pty_session_release(session);
    active_session = NULL;
    return 0;
}
