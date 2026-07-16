#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "flutter_pty.h"

static pthread_mutex_t event_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event_condition = PTHREAD_COND_INITIALIZER;
static char output[32];
static size_t output_length;
static bool exited;
static bool output_done;
static int64_t exit_value;

static bool post_object(Dart_Port_DL port, Dart_CObject *message)
{
    (void)port;
    assert(message->type == Dart_CObject_kTypedData);

    pthread_mutex_lock(&event_mutex);
    size_t length = message->value.as_typed_data.length;
    assert(output_length + length <= sizeof(output));
    memcpy(output + output_length, message->value.as_typed_data.values, length);
    output_length += length;
    pthread_cond_broadcast(&event_condition);
    pthread_mutex_unlock(&event_mutex);
    return true;
}

static bool post_integer(Dart_Port_DL port, int64_t value)
{
    (void)port;
    pthread_mutex_lock(&event_mutex);
    if (port == 2)
    {
        exited = true;
        exit_value = value;
    }
    if (port == 3) output_done = true;
    pthread_cond_broadcast(&event_condition);
    pthread_mutex_unlock(&event_mutex);
    return true;
}

static struct timespec deadline_after_milliseconds(long milliseconds)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += (milliseconds % 1000) * 1000000;
    deadline.tv_sec += milliseconds / 1000 + deadline.tv_nsec / 1000000000;
    deadline.tv_nsec %= 1000000000;
    return deadline;
}

static bool wait_for_output(size_t length, long timeout_milliseconds)
{
    struct timespec deadline = deadline_after_milliseconds(timeout_milliseconds);

    pthread_mutex_lock(&event_mutex);
    while (output_length < length)
    {
        if (pthread_cond_timedwait(&event_condition, &event_mutex, &deadline) != 0) break;
    }
    bool received = output_length >= length;
    pthread_mutex_unlock(&event_mutex);
    return received;
}

static bool wait_for_exit(long timeout_milliseconds)
{
    struct timespec deadline = deadline_after_milliseconds(timeout_milliseconds);

    pthread_mutex_lock(&event_mutex);
    while (!exited || !output_done)
    {
        if (pthread_cond_timedwait(&event_condition, &event_mutex, &deadline) != 0) break;
    }
    bool did_exit = exited && output_done;
    pthread_mutex_unlock(&event_mutex);
    return did_exit;
}

static void reset_events(void)
{
    pthread_mutex_lock(&event_mutex);
    memset(output, 0, sizeof(output));
    output_length = 0;
    exited = false;
    output_done = false;
    exit_value = 0;
    pthread_mutex_unlock(&event_mutex);
}

static int count_open_descriptors(void)
{
    int count = 0;
    for (int fd = 0; fd < 256; fd++)
    {
        if (fcntl(fd, F_GETFD) != -1) count++;
    }
    return count;
}

static int run_winsize_child(void)
{
    char input;
    assert(read(STDIN_FILENO, &input, 1) == 1);

    struct winsize size;
    assert(ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0);
    printf("%u %u %u %u\n",
           size.ws_row,
           size.ws_col,
           size.ws_xpixel,
           size.ws_ypixel);
    return 0;
}

static int run_state_child(void)
{
    struct termios attributes;
    assert(tcgetattr(STDIN_FILENO, &attributes) == 0);
    struct sigaction interrupt_action;
    assert(sigaction(SIGINT, NULL, &interrupt_action) == 0);
    struct sigaction pipe_action;
    assert(sigaction(SIGPIPE, NULL, &pipe_action) == 0);

#ifdef IUTF8
    bool utf8_enabled = (attributes.c_iflag & IUTF8) != 0;
#else
    bool utf8_enabled = true;
#endif
    printf("utf8=%d int=%d pipe=%d\n",
           utf8_enabled,
           interrupt_action.sa_handler == SIG_DFL,
           pipe_action.sa_handler == SIG_DFL);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--winsize-child") == 0)
    {
        return run_winsize_child();
    }
    if (argc > 1 && strcmp(argv[1], "--state-child") == 0)
    {
        return run_state_child();
    }

    Dart_PostCObject_DL = post_object;
    Dart_PostInteger_DL = post_integer;

    assert(pty_create(NULL) == NULL);
    assert(strstr(pty_error(), "Invalid argument") != NULL);
    assert(pty_getpid(NULL) == -1);

    char *arguments[] = {
        "/bin/sh",
        "-c",
        "printf A; sleep 0.2; printf B",
        NULL,
    };
    PtyOptions options = {
        .rows = 24,
        .cols = 80,
        .executable = "/bin/sh",
        .arguments = arguments,
        .stdout_port = 1,
        .exit_port = 2,
        .output_done_port = 3,
        .ackRead = true,
    };

    PtyHandle *handle = pty_create(&options);
    assert(handle != NULL);
    assert(wait_for_output(1, 2000));
    assert(output[0] == 'A');
    assert(!wait_for_output(2, 500));

    pty_ack_read(handle);
    assert(wait_for_output(2, 2000));
    assert(output[1] == 'B');
    pty_ack_read(handle);
    assert(wait_for_exit(2000));

    pty_destroy(handle);

    reset_events();
    arguments[2] = "stty raw -echo; printf R; head -c 131072 | wc -c";
    options.ackRead = false;

    handle = pty_create(&options);
    assert(handle != NULL);
    assert(wait_for_output(1, 2000));
    assert(output[0] == 'R');

    char *large_input = malloc(131072);
    assert(large_input != NULL);
    memset(large_input, 'x', 131072);
    assert(pty_write(NULL, large_input, 131072) == 0);
    assert(pty_write(handle, NULL, 131072) == 0);
    assert(pty_write(handle, large_input, 8 * 1024 * 1024 + 1) == 0);
    assert(pty_write(handle, large_input, 131072) == 1);
    free(large_input);

    assert(wait_for_exit(5000));
    assert(exit_value == 0);
    assert(strstr(output, "131072") != NULL);
    pty_destroy(handle);

    reset_events();
    arguments[0] = argv[0];
    arguments[1] = "--winsize-child";
    arguments[2] = NULL;
    options.executable = argv[0];

    handle = pty_create(&options);
    assert(handle != NULL);
    assert(pty_resize(NULL, 40, 100, 900, 600) == -1);
    assert(strstr(pty_error(), "invalid PTY size") != NULL);
    assert(pty_resize(handle, 0, 100, 900, 600) == -1);
    assert(pty_resize(handle, 40, 0, 900, 600) == -1);
    assert(pty_resize(handle, 40, 100, 900, 600) == 0);
    assert(pty_error() == NULL);
    assert(pty_write(handle, "\n", 1) == 1);

    assert(wait_for_exit(2000));
    assert(exit_value == 0);
    assert(strstr(output, "40 100 900 600") != NULL);
    pty_destroy(handle);

    reset_events();
    arguments[1] = "--state-child";
    signal(SIGINT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    handle = pty_create(&options);
    signal(SIGINT, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    assert(handle != NULL);
    assert(wait_for_exit(2000));
    assert(exit_value == 0);
    assert(strstr(output, "utf8=1 int=1 pipe=1") != NULL);
    pty_destroy(handle);

    reset_events();
    char *sleep_arguments[] = {"/bin/sh", "-c", "sleep 30", NULL};
    options.executable = sleep_arguments[0];
    options.arguments = sleep_arguments;
    options.working_directory = NULL;

    handle = pty_create(&options);
    assert(handle != NULL);
    assert(pty_kill(NULL, SIGTERM) == 0);
    assert(pty_kill(handle, 0) == 0);
    assert(pty_kill(handle, SIGTERM) == 1);
    assert(wait_for_exit(2000));
    assert(exit_value == -SIGTERM);
    pty_destroy(handle);

    reset_events();
    handle = pty_create(&options);
    assert(handle != NULL);
    pty_destroy(handle);
    assert(wait_for_exit(2000));
    assert(exit_value == -SIGHUP || exit_value == -1);

    char *invalid_arguments[] = {"/definitely/missing/lumide-shell", NULL};
    options.executable = invalid_arguments[0];
    options.arguments = invalid_arguments;
    int descriptors_before_failed_spawns = count_open_descriptors();
    for (int attempt = 0; attempt < 32; attempt++)
    {
        handle = pty_create(&options);
        assert(handle == NULL);
    }
    assert(strstr(pty_error(), "No such file or directory") != NULL);
    assert(count_open_descriptors() == descriptors_before_failed_spawns);

    char *shell_arguments[] = {"/bin/sh", NULL};
    options.executable = shell_arguments[0];
    options.arguments = shell_arguments;
    options.working_directory = "/definitely/missing/lumide-directory";
    handle = pty_create(&options);
    assert(handle == NULL);
    assert(strstr(pty_error(), "No such file or directory") != NULL);

    reset_events();
    assert(setenv("GHOSTTY_RESOURCES_DIR", "leaked", 1) == 0);
    char *environment_arguments[] = {
        "/bin/sh",
        "-c",
        "printf '%s|%s' \"$TERM_PROGRAM\" \"${GHOSTTY_RESOURCES_DIR-unset}\"",
        NULL,
    };
    char *clean_environment[] = {
        "PATH=/usr/bin:/bin",
        "TERM_PROGRAM=Lumide",
        NULL,
    };
    options.executable = environment_arguments[0];
    options.arguments = environment_arguments;
    options.environment = clean_environment;
    options.working_directory = NULL;

    handle = pty_create(&options);
    assert(unsetenv("GHOSTTY_RESOURCES_DIR") == 0);
    assert(handle != NULL);
    assert(wait_for_exit(2000));
    assert(exit_value == 0);
    assert(strcmp(output, "Lumide|unset") == 0);
    pty_destroy(handle);

    puts("flutter_pty Unix lifecycle test passed");
    return 0;
}
