#include <assert.h>
#include <string.h>

#include "../src/unix/pty_unix_spawn.h"

int main(void)
{
    const char *arguments[] = {"-c", "printf ok"};
    const char *environment[] = {"PATH=/bin", "LANG=C"};
    const PtySpawnOptions source = {
        .executable = "/bin/sh",
        .arguments = arguments,
        .argument_count = 2,
        .environment = environment,
        .environment_count = 2,
        .working_directory = "/tmp",
        .size = {.rows = 24, .columns = 80},
        .input_buffer_bytes = 1024,
        .output_window_bytes = 2048,
        .event_port = 17,
    };
    PtyUnixOwnedOptions copy;
    PtyError error;
    assert(pty_unix_clone_options(&source, &copy, &error) == 1);
    assert(copy.options.executable == copy.executable);
    assert(copy.options.arguments == (const char *const *)copy.arguments);
    assert(copy.options.environment == (const char *const *)copy.environment);
    assert(copy.options.working_directory == copy.working_directory);
    assert(strcmp(copy.executable, source.executable) == 0);
    assert(strcmp(copy.arguments[0], arguments[0]) == 0);
    assert(strcmp(copy.arguments[1], arguments[1]) == 0);
    assert(strcmp(copy.environment[0], environment[0]) == 0);
    assert(strcmp(copy.working_directory, source.working_directory) == 0);
    pty_unix_free_options(&copy);
    return 0;
}
