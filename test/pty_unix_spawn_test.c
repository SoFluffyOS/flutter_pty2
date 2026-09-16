#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/unix/pty_unix_spawn.h"

static PtySpawnOptions base_options(const char *executable,
                                    const char *const *arguments,
                                    int32_t argument_count,
                                    const char *working_directory)
{
    PtySpawnOptions options = {
        .executable = executable,
        .arguments = arguments,
        .argument_count = argument_count,
        .environment = NULL,
        .environment_count = 0,
        .working_directory = working_directory,
        .size = {
            .rows = 24,
            .columns = 80,
            .pixel_width = 0,
            .pixel_height = 0,
        },
    };
    return options;
}

static void assert_spawn_failure(const PtySpawnOptions *options,
                                 PtyErrorKind expected_kind)
{
    int master_fd = -1;
    int slave_fd = -1;
    pid_t process_id = -1;
    PtyError error;
    assert(pty_unix_spawn(options,
                          &master_fd,
                          &slave_fd,
                          &process_id,
                          &error) == 0);
    assert(master_fd == -1);
    assert(slave_fd == -1);
    assert(process_id == -1);
    assert(error.kind == (int32_t)expected_kind);
}

int main(void)
{
    const char *arguments[] = {"-c", "printf '%s' \"$PTY_TEST_VALUE\"", NULL};
    const char *environment[] = {"PATH=/usr/bin:/bin", "PTY_TEST_VALUE=spawn-ok"};
    PtySpawnOptions options = base_options("sh", arguments, 2, NULL);
    options.environment = environment;
    options.environment_count = 2;

    int master_fd = -1;
    int slave_fd = -1;
    pid_t process_id = -1;
    PtyError error;
    assert(pty_unix_spawn(&options,
                          &master_fd,
                          &slave_fd,
                          &process_id,
                          &error) == 1);
    assert(master_fd >= 0);
    assert(slave_fd >= 0);
    assert(process_id > 0);

    char output[64] = {0};
    size_t output_length = 0;
    int process_status = 0;
    for (int attempt = 0; attempt < 100; attempt++) {
        struct pollfd descriptor = {.fd = master_fd, .events = POLLIN};
        const int poll_result = poll(&descriptor, 1, 20);
        if (poll_result > 0 && (descriptor.revents & POLLIN) != 0) {
            const ssize_t length = read(master_fd,
                                        output + output_length,
                                        sizeof(output) - output_length - 1);
            if (length > 0) output_length += (size_t)length;
        }
        const pid_t waited = waitpid(process_id, &process_status, WNOHANG);
        if (waited == process_id && output_length >= strlen("spawn-ok")) break;
    }
    close(master_fd);
    close(slave_fd);
    assert(WIFEXITED(process_status));
    assert(WEXITSTATUS(process_status) == 0);
    assert(output_length == strlen("spawn-ok"));
    assert(memcmp(output, "spawn-ok", output_length) == 0);

    const char *missing_arguments[] = {NULL};
    const PtySpawnOptions missing = base_options(
        "pty_test_child_missing_executable",
        missing_arguments,
        0,
        NULL);
    assert_spawn_failure(&missing, PTY_ERROR_NOT_FOUND);

    const PtySpawnOptions bad_directory = base_options(
        "/bin/sh",
        missing_arguments,
        0,
        "/path/that/does/not/exist");
    assert_spawn_failure(&bad_directory, PTY_ERROR_WORKING_DIRECTORY);
    return 0;
}
