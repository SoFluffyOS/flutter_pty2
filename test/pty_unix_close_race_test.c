#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>

#include "../src/flutter_pty.h"
#include "../src/include/dart_api_dl.h"
#include "../src/pty_internal.h"

static pthread_mutex_t events_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t events_condition = PTHREAD_COND_INITIALIZER;
static int spawned;
static int session_closed;

static bool post_object(Dart_Port_DL port, Dart_CObject *message)
{
    (void)port;
    assert(message != NULL);
    assert(message->type == Dart_CObject_kArray);
    assert(message->value.as_array.length >= 1);
    pthread_mutex_lock(&events_mutex);
    const int32_t event_type = message->value.as_array.values[0]->value.as_int32;
    if (event_type == PTY_EVENT_SPAWNED) {
        spawned = 1;
    } else if (event_type == PTY_EVENT_SESSION_CLOSED) {
        session_closed = 1;
    }
    pthread_cond_broadcast(&events_condition);
    pthread_mutex_unlock(&events_mutex);
    return true;
}

static int wait_for_spawn(void)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    pthread_mutex_lock(&events_mutex);
    while (!spawned) {
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

int main(void)
{
    Dart_PostCObject_DL = post_object;
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
    PtySession *session = NULL;
    PtyError error;
    assert(pty_session_start(&options, &session, &error) == 1);
    assert(session != NULL);

    assert(wait_for_spawn());
    pty_session_mark_closing(session);
    pty_session_begin_close(session);
    pty_session_begin_close(session);
    assert(wait_for_close());
    pty_session_release(session);
    return 0;
}
