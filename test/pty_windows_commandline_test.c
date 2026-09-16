#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../src/windows/pty_windows_commandline.h"

static void expect_command(const char *executable,
                           const char *const *arguments,
                           int argument_count,
                           const wchar_t *expected)
{
    wchar_t *command = pty_windows_build_command_line(
        executable,
        arguments,
        argument_count);
    assert(command != NULL);
    assert(wcscmp(command, expected) == 0);
    free(command);
}

int main(void)
{
    const char *simple[] = {"hello world", ""};
    expect_command("program.exe",
                   simple,
                   2,
                   L"\"program.exe\" \"hello world\" \"\"");

    const char *quote[] = {"a\"b", "C:\\path\\"};
    expect_command("program.exe",
                   quote,
                   2,
                   L"\"program.exe\" \"a\\\"b\" \"C:\\path\\\\\"");

    const char *unicode[] = {"caf\xC3\xA9"};
    expect_command("program.exe", unicode, 1, L"\"program.exe\" \"caf\u00E9\"");
    return 0;
}
