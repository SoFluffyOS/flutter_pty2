#include <assert.h>
#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../src/flutter_pty.h"
#include "../src/include/dart_api_dl.h"
#include "../src/pty_internal.h"

#define CONCURRENT_SESSION_COUNT 100

static int configured_session_count(void)
{
    const char *value = getenv("PTY_CONCURRENT_SESSION_COUNT");
    if (value == NULL || value[0] == '\0') return CONCURRENT_SESSION_COUNT;
    char *end = NULL;
    const long parsed = strtol(value, &end, 10);
    if (*end != '\0' || parsed < 1 || parsed > CONCURRENT_SESSION_COUNT) {
        return CONCURRENT_SESSION_COUNT;
    }
    return (int)parsed;
}

typedef struct ConcurrentEvents {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int spawned;
    int spawn_failed;
    int session_closed;
} ConcurrentEvents;

static ConcurrentEvents events = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER,
};

static bool post_object(Dart_Port_DL port, Dart_CObject *message)
{
    (void)port;
    assert(message != NULL);
    assert(message->type == Dart_CObject_kArray);
    assert(message->value.as_array.length >= 1);
    const int32_t event_type = message->value.as_array.values[0]->value.as_int32;

    pthread_mutex_lock(&events.mutex);
    if (event_type == PTY_EVENT_SPAWNED) {
        events.spawned++;
    } else if (event_type == PTY_EVENT_SPAWN_FAILED) {
        events.spawn_failed++;
    } else if (event_type == PTY_EVENT_SESSION_CLOSED) {
        events.session_closed++;
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

static int wait_for_spawned(int expected)
{
    const struct timespec deadline = deadline_after_seconds(30);
    pthread_mutex_lock(&events.mutex);
    while (events.spawned < expected && events.spawn_failed == 0) {
        if (pthread_cond_timedwait(&events.condition,
                                   &events.mutex,
                                   &deadline) != 0) {
            pthread_mutex_unlock(&events.mutex);
            return 0;
        }
    }
    const int success = events.spawned == expected && events.spawn_failed == 0;
    pthread_mutex_unlock(&events.mutex);
    return success;
}

static int wait_for_closed(int expected)
{
    const struct timespec deadline = deadline_after_seconds(30);
    pthread_mutex_lock(&events.mutex);
    while (events.session_closed < expected) {
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

static int count_directory_entries(const char *path)
{
    DIR *directory = opendir(path);
    assert(directory != NULL);
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        count++;
    }
    assert(closedir(directory) == 0);
    return count;
}

static int count_open_file_descriptors(void)
{
    return count_directory_entries("/dev/fd");
}

#if defined(__linux__)
static int count_live_threads(void)
{
    return count_directory_entries("/proc/self/task");
}
#endif

static int wait_for_zero_resources(void)
{
    for (int attempt = 0; attempt < 3000; attempt++) {
        PtyDebugStats stats;
        pty_debug_get_stats(&stats);
        if (stats.live_sessions == 0 && stats.live_read_workers == 0 &&
            stats.live_write_workers == 0 && stats.live_wait_workers == 0 &&
            stats.live_close_workers == 0 && stats.pending_write_chunks == 0 &&
            stats.pending_write_bytes == 0 &&
            stats.inflight_write_bytes == 0) {
            return 1;
        }
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000};
        nanosleep(&delay, NULL);
    }
    return 0;
}

int main(void)
{
    Dart_PostCObject_DL = post_object;
    const int baseline_file_descriptors = count_open_file_descriptors();
#if defined(__linux__)
    const int baseline_threads = count_live_threads();
#endif
    const char *arguments[] = {"-c", "exit 0"};
    const char *environment[] = {"PATH=/usr/bin:/bin"};
    const int session_target = configured_session_count();
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
    PtySession *sessions[CONCURRENT_SESSION_COUNT] = {0};
    int session_count = 0;
    int success = 1;

    for (int index = 0; index < session_target; index++) {
        PtyError error;
        if (!pty_session_start(&options, &sessions[session_count], &error)) {
            fprintf(stderr,
                    "concurrent spawn failed: %s\n",
                    error.message);
            success = 0;
            break;
        }
        session_count++;
    }
    if (success && !wait_for_spawned(session_count)) {
        fprintf(stderr, "not all concurrent PTY sessions spawned\n");
        success = 0;
    }
    for (int index = 0; index < session_count; index++) {
        pty_session_begin_close(sessions[index]);
    }
    if (!wait_for_closed(session_count)) {
        fprintf(stderr, "not all concurrent PTY sessions closed\n");
        success = 0;
    }
    for (int index = 0; index < session_count; index++) {
        pty_session_release(sessions[index]);
    }
    if (!wait_for_zero_resources()) {
        fprintf(stderr, "concurrent PTY resources did not return to zero\n");
        success = 0;
    }
    if (count_open_file_descriptors() != baseline_file_descriptors) {
        fprintf(stderr, "concurrent PTY descriptor count changed\n");
        success = 0;
    }
#if defined(__linux__)
    if (count_live_threads() != baseline_threads) {
        fprintf(stderr, "concurrent PTY thread count changed\n");
        success = 0;
    }
#endif
    return success ? 0 : 1;
}
