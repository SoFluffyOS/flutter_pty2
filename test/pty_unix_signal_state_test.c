#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/unix/pty_unix_spawn.h"

static int report_signal_state(void)
{
    sigset_t blocked;
    assert(sigprocmask(SIG_SETMASK, NULL, &blocked) == 0);
    struct sigaction tstp_action;
    assert(sigaction(SIGTSTP, NULL, &tstp_action) == 0);
    printf("term_blocked=%d tstp_ignored=%d\n",
           sigismember(&blocked, SIGTERM) == 1,
           tstp_action.sa_handler == SIG_IGN);
    return fflush(stdout) == 0 ? 0 : 3;
}

static PtySpawnOptions spawn_options(const char *executable,
                                     const char *const *arguments)
{
    return (PtySpawnOptions){
        .executable = executable,
        .arguments = arguments,
        .argument_count = 1,
        .environment = NULL,
        .environment_count = 0,
        .size = {.rows = 24, .columns = 80},
        .input_buffer_bytes = 64 * 1024,
        .output_window_bytes = 64 * 1024,
    };
}

/* Reads the program's exit report when it is available without blocking. */
static int try_read_exit(const PtyUnixProcess *process, PtyUnixExit *exit_report)
{
    struct pollfd descriptor = {.fd = process->exit_fd, .events = POLLIN};
    if (poll(&descriptor, 1, 0) <= 0) return 0;
    assert(pty_unix_read_exit(process->exit_fd, exit_report) == 1);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--report-signal-state") == 0) {
        return report_signal_state();
    }
    assert(argc >= 1);

    char executable[PATH_MAX];
    assert(realpath(argv[0], executable) != NULL);
    const char *arguments[] = {"--report-signal-state"};
    const PtySpawnOptions options = spawn_options(executable, arguments);

    sigset_t blocked;
    sigset_t previous_mask;
    assert(sigemptyset(&blocked) == 0);
    assert(sigaddset(&blocked, SIGTERM) == 0);
    assert(sigprocmask(SIG_BLOCK, &blocked, &previous_mask) == 0);
    struct sigaction ignored_action = {
        .sa_handler = SIG_IGN,
    };
    sigemptyset(&ignored_action.sa_mask);
    ignored_action.sa_flags = 0;
    struct sigaction previous_action;
    assert(sigaction(SIGTSTP, &ignored_action, &previous_action) == 0);

    int master_fd = -1;
    int slave_fd = -1;
    PtyUnixProcess process;
    PtyError error;
    const int spawned = pty_unix_spawn(
        &options,
        &master_fd,
        &slave_fd,
        &process,
        &error);

    assert(sigaction(SIGTSTP, &previous_action, NULL) == 0);
    assert(sigprocmask(SIG_SETMASK, &previous_mask, NULL) == 0);
    assert(spawned == 1);
    assert(master_fd >= 0);
    assert(slave_fd >= 0);

    char output[128] = {0};
    size_t output_length = 0;
    PtyUnixExit exit_report = {0};
    int child_reaped = 0;
    for (int attempt = 0; attempt < 100; attempt++) {
        struct pollfd descriptor = {.fd = master_fd, .events = POLLIN};
        const int poll_result = poll(&descriptor, 1, 50);
        if (poll_result > 0 && (descriptor.revents & POLLIN) != 0) {
            const ssize_t length = read(
                master_fd,
                output + output_length,
                sizeof(output) - output_length - 1);
            if (length > 0) output_length += (size_t)length;
        }
        if (!child_reaped) child_reaped = try_read_exit(&process, &exit_report);
        if (child_reaped &&
            strstr(output, "term_blocked=0") != NULL &&
            strstr(output, "tstp_ignored=0") != NULL) {
            break;
        }
    }

    if (!child_reaped) {
        assert(pty_unix_read_exit(process.exit_fd, &exit_report) == 1);
    }
    pty_unix_release_process(&process);
    assert(close(master_fd) == 0);
    assert(close(slave_fd) == 0);
    assert(exit_report.signaled == 0);
    assert(exit_report.value == 0);
    assert(strstr(output, "term_blocked=0") != NULL);
    assert(strstr(output, "tstp_ignored=0") != NULL);
    assert(output_length > 0);
    return 0;
}
