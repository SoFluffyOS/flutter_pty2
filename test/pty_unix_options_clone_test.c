#include <assert.h>
#include <string.h>

#include "../src/unix/pty_unix_spawn.h"

int main(void)
{
    char executable[64] = "/bin/sh";
    char argument_zero[64] = "-c";
    char argument_one[64] = "printf ok";
    const char *arguments[] = {argument_zero, argument_one};
    char environment_zero[64] = "PATH=/bin";
    char environment_one[64] = "LANG=C";
    const char *environment[] = {environment_zero, environment_one};
    char working_directory[64] = "/tmp";
    const PtySpawnOptions source = {
        .executable = executable,
        .arguments = arguments,
        .argument_count = 2,
        .environment = environment,
        .environment_count = 2,
        .working_directory = working_directory,
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

    strcpy(executable, "changed executable");
    strcpy(argument_zero, "changed argument zero");
    strcpy(argument_one, "changed argument one");
    strcpy(environment_zero, "changed environment zero");
    strcpy(environment_one, "changed environment one");
    strcpy(working_directory, "changed working directory");

    assert(strcmp(copy.executable, "/bin/sh") == 0);
    assert(strcmp(copy.arguments[0], "-c") == 0);
    assert(strcmp(copy.arguments[1], "printf ok") == 0);
    assert(strcmp(copy.environment[0], "PATH=/bin") == 0);
    assert(strcmp(copy.environment[1], "LANG=C") == 0);
    assert(strcmp(copy.working_directory, "/tmp") == 0);

    pty_unix_free_options(&copy);
    return 0;
}
