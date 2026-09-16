#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#endif

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
            fprintf(stderr,
                    "native stress timeout: process_exit=%d output_closed=%d "
                    "session_closed=%d\n",
                    events.process_exit,
                    events.output_closed,
                    events.session_closed);
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
#elif defined(__APPLE__)
static int count_live_threads(void)
{
    struct proc_taskinfo task_info;
    const int result = proc_pidinfo(getpid(),
                                    PROC_PIDTASKINFO,
                                    0,
                                    &task_info,
                                    sizeof(task_info));
    assert(result == (int)sizeof(task_info));
    return task_info.pti_threadnum;
}
#endif

#if defined(__linux__)
static int process_id_from_name(const char *name, pid_t *process_id)
{
    char *end = NULL;
    errno = 0;
    const long value = strtol(name, &end, 10);
    if (errno == ERANGE || end == name || *end != '\0' || value <= 0 ||
        value > INT_MAX) {
        return 0;
    }
    *process_id = (pid_t)value;
    return 1;
}

static pid_t process_parent_id(pid_t process_id)
{
    char path[64];
    const int length = snprintf(path,
                                sizeof(path),
                                "/proc/%ld/status",
                                (long)process_id);
    assert(length > 0 && (size_t)length < sizeof(path));
    FILE *status = fopen(path, "r");
    if (status == NULL) {
        if (errno == ENOENT) return -1;
        assert(status != NULL);
    }

    char line[128];
    int parent_id = -1;
    while (fgets(line, sizeof(line), status) != NULL) {
        if (sscanf(line, "PPid: %d", &parent_id) == 1) break;
    }
    assert(fclose(status) == 0);
    return (pid_t)parent_id;
}

static int count_child_processes(void)
{
    DIR *directory = opendir("/proc");
    assert(directory != NULL);
    const pid_t current_process_id = getpid();
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        pid_t process_id;
        if (!process_id_from_name(entry->d_name, &process_id)) continue;
        if (process_parent_id(process_id) == current_process_id) count++;
    }
    assert(closedir(directory) == 0);
    return count;
}
#elif defined(__APPLE__)
static int count_child_processes(void)
{
    const int buffer_size = proc_listchildpids(getpid(), NULL, 0);
    assert(buffer_size >= 0);
    if (buffer_size == 0) return 0;

    pid_t *children = malloc((size_t)buffer_size);
    assert(children != NULL);
    const int result = proc_listchildpids(getpid(), children, buffer_size);
    assert(result >= 0 && result <= buffer_size);
    free(children);
    return result / (int)sizeof(*children);
}
#endif

int main(void)
{
    Dart_PostCObject_DL = post_object;
    int cycle_count = 1000;
    const char *configured_cycle_count = getenv("PTY_NATIVE_STRESS_CYCLES");
    if (configured_cycle_count != NULL) {
        cycle_count = (int)strtol(configured_cycle_count, NULL, 10);
    }
    assert(cycle_count > 0);
    const int baseline_file_descriptors = count_open_file_descriptors();
    const int baseline_child_processes = count_child_processes();
#if defined(__linux__) || defined(__APPLE__)
    const int baseline_threads = count_live_threads();
#endif
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

    for (int cycle = 0; cycle < cycle_count; cycle++) {
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
    PtyDebugStats stats;
    pty_debug_get_stats(&stats);
    assert(stats.live_sessions == 0);
    assert(stats.live_read_workers == 0);
    assert(stats.live_write_workers == 0);
    assert(stats.live_wait_workers == 0);
    assert(stats.live_close_workers == 0);
    assert(stats.pending_write_chunks == 0);
    assert(stats.pending_write_bytes == 0);
    assert(count_open_file_descriptors() == baseline_file_descriptors);
    assert(count_child_processes() == baseline_child_processes);
#if defined(__linux__) || defined(__APPLE__)
    assert(count_live_threads() == baseline_threads);
#endif
    return 0;
}
