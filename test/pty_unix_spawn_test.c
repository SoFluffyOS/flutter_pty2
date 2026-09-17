#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
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
                                 PtyErrorKind expected_kind,
                                 int64_t expected_code)
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
    assert(error.os_code == expected_code);
}

static void assert_fd_three_is_not_inherited(void)
{
    const int status_fd = STDERR_FILENO + 1;
    const int original_fd = fcntl(status_fd, F_DUPFD_CLOEXEC, 100);
    const int descriptor = open("/dev/null", O_RDONLY);
    assert(descriptor >= 0);
    if (descriptor != status_fd) {
        assert(dup2(descriptor, status_fd) == status_fd);
        assert(close(descriptor) == 0);
    }

    const char *arguments[] = {
        "-c",
        "if test -e /dev/fd/3; then exit 42; else exit 0; fi",
    };
    const char *environment[] = {"PATH=/usr/bin:/bin"};
    const PtySpawnOptions options = base_options("/bin/sh", arguments, 2, NULL);
    PtySpawnOptions configured = options;
    configured.environment = environment;
    configured.environment_count = 1;

    int master_fd = -1;
    int slave_fd = -1;
    pid_t process_id = -1;
    PtyError error;
    assert(pty_unix_spawn(&configured,
                          &master_fd,
                          &slave_fd,
                          &process_id,
                          &error) == 1);
    int status = 0;
    assert(waitpid(process_id, &status, 0) == process_id);
    assert(close(master_fd) == 0);
    assert(close(slave_fd) == 0);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);

    if (original_fd >= 0) {
        assert(dup2(original_fd, status_fd) == status_fd);
        assert(close(original_fd) == 0);
        return;
    }
    assert(close(status_fd) == 0);
}

static void assert_path_permission_failure_is_preserved(void)
{
    char directory_template[] = "/tmp/flutter-pty-path-XXXXXX";
    char *directory = mkdtemp(directory_template);
    assert(directory != NULL);

    char executable_path[PATH_MAX];
    const int path_length = snprintf(executable_path,
                                     sizeof(executable_path),
                                     "%s/not-executable",
                                     directory);
    assert(path_length > 0 && (size_t)path_length < sizeof(executable_path));
    const int descriptor = open(executable_path,
                                O_CREAT | O_WRONLY | O_TRUNC,
                                S_IRUSR | S_IWUSR);
    assert(descriptor >= 0);
    assert(close(descriptor) == 0);
    assert(chmod(executable_path, S_IRUSR | S_IWUSR) == 0);

    const char *arguments[] = {NULL};
    const char *environment[1];
    char path_environment[PATH_MAX + 32];
    const int environment_length = snprintf(path_environment,
                                            sizeof(path_environment),
                                            "PATH=%s:/missing",
                                            directory);
    assert(environment_length > 0 &&
           (size_t)environment_length < sizeof(path_environment));
    environment[0] = path_environment;
    PtySpawnOptions options = base_options("not-executable",
                                           arguments,
                                           0,
                                           NULL);
    options.environment = environment;
    options.environment_count = 1;

    assert_spawn_failure(&options, PTY_ERROR_PERMISSION_DENIED, EACCES);
    assert(unlink(executable_path) == 0);
    assert(rmdir(directory) == 0);
}

static void assert_exec_not_found_failure_is_preserved(void)
{
    char executable_template[] = "/tmp/flutter-pty-exec-XXXXXX";
    const int descriptor = mkstemp(executable_template);
    assert(descriptor >= 0);
    const char script[] = "#!/path/that/does/not/exist\n";
    assert(write(descriptor, script, sizeof(script) - 1) ==
           (ssize_t)(sizeof(script) - 1));
    assert(close(descriptor) == 0);
    assert(chmod(executable_template, S_IRWXU) == 0);

    const char *arguments[] = {NULL};
    const PtySpawnOptions options = base_options(executable_template,
                                                 arguments,
                                                 0,
                                                 NULL);
    assert_spawn_failure(&options, PTY_ERROR_NOT_FOUND, ENOENT);
    assert(unlink(executable_template) == 0);
}

int main(void)
{
    const char *invalid_arguments[] = {NULL};
    PtySpawnOptions invalid_size = base_options(
        "/bin/sh",
        invalid_arguments,
        0,
        NULL);
    invalid_size.size.rows = 0;
    assert_spawn_failure(&invalid_size, PTY_ERROR_INVALID_ARGUMENT, EINVAL);
    invalid_size = base_options("/bin/sh", invalid_arguments, 0, NULL);
    invalid_size.size.columns = 0x8000;
    assert_spawn_failure(&invalid_size, PTY_ERROR_INVALID_ARGUMENT, EINVAL);
    invalid_size = base_options("/bin/sh", invalid_arguments, 0, NULL);
    invalid_size.size.pixel_width = -1;
    assert_spawn_failure(&invalid_size, PTY_ERROR_INVALID_ARGUMENT, EINVAL);
    invalid_size = base_options("/bin/sh", invalid_arguments, 0, NULL);
    invalid_size.size.pixel_height = 0x10000;
    assert_spawn_failure(&invalid_size, PTY_ERROR_INVALID_ARGUMENT, EINVAL);

    assert_fd_three_is_not_inherited();
    assert_path_permission_failure_is_preserved();
    assert_exec_not_found_failure_is_preserved();

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
    errno = EACCES;
    assert_spawn_failure(&missing, PTY_ERROR_NOT_FOUND, ENOENT);

    const PtySpawnOptions bad_directory = base_options(
        "/bin/sh",
        missing_arguments,
        0,
        "/path/that/does/not/exist");
    assert_spawn_failure(&bad_directory,
                         PTY_ERROR_WORKING_DIRECTORY,
                         ENOENT);

    const char *relative_environment[] = {"PATH=."};
    PtySpawnOptions bad_relative_directory = base_options(
        "sh",
        missing_arguments,
        0,
        "/path/that/does/not/exist");
    bad_relative_directory.environment = relative_environment;
    bad_relative_directory.environment_count = 1;
    assert_spawn_failure(&bad_relative_directory,
                         PTY_ERROR_WORKING_DIRECTORY,
                         ENOENT);
    return 0;
}
