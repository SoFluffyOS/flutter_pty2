#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>

#include "../src/flutter_pty.h"
#include "../src/include/dart_api_dl.h"

typedef struct CycleEvents {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int process_exit;
    int output_closed;
    int session_closed;
} CycleEvents;

static CycleEvents events = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER,
};

static bool post_object(Dart_Port_DL port, Dart_CObject *message)
{
    (void)port;
    assert(message != NULL);
    assert(message->type == Dart_CObject_kArray);
    assert(message->value.as_array.length >= 1);
    const int32_t event_type =
        message->value.as_array.values[0]->value.as_int32;

    pthread_mutex_lock(&events.mutex);
    if (event_type == PTY_EVENT_PROCESS_EXIT) {
        events.process_exit = 1;
    } else if (event_type == PTY_EVENT_OUTPUT_CLOSED) {
        events.output_closed = 1;
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

static int wait_for_events(int wait_for_close)
{
    const struct timespec deadline = deadline_after_seconds(5);
    pthread_mutex_lock(&events.mutex);
    while (!events.process_exit || !events.output_closed ||
           (wait_for_close && !events.session_closed)) {
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

static void reset_events(void)
{
    pthread_mutex_lock(&events.mutex);
    events.process_exit = 0;
    events.output_closed = 0;
    events.session_closed = 0;
    pthread_mutex_unlock(&events.mutex);
}

int main(void)
{
    Dart_PostCObject_DL = post_object;
    const char *arguments[] = {"-c", "exit 0"};
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

    for (int cycle = 0; cycle < 1000; cycle++) {
        reset_events();
        PtySession *session = NULL;
        PtyError error;
        assert(pty_session_start(&options, &session, &error) == 1);
        assert(session != NULL);
        assert(wait_for_events(0));
        pty_session_begin_close(session);
        assert(wait_for_events(1));
        pty_session_release(session);
    }
    return 0;
}
