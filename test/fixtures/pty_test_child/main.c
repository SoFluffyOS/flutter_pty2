#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#define unlink _unlink
#else
#include <sys/select.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

static void sleep_milliseconds(unsigned int milliseconds)
{
#if defined(_WIN32)
    Sleep(milliseconds);
#else
    struct timeval delay = {
        .tv_sec = (long)(milliseconds / 1000),
        .tv_usec = (long)((milliseconds % 1000) * 1000),
    };
    select(0, NULL, NULL, NULL, &delay);
#endif
}

static unsigned long long parse_count(const char *value)
{
    char *end = NULL;
    unsigned long long count = strtoull(value, &end, 10);
    if (value[0] == '\0' || end == NULL || *end != '\0') {
        fprintf(stderr, "invalid count: %s\n", value);
        exit(2);
    }
    return count;
}

static unsigned int parse_delay(const char *value)
{
    unsigned long long delay = parse_count(value);
    if (delay > 60000) {
        fprintf(stderr, "delay is too large: %s\n", value);
        exit(2);
    }
    return (unsigned int)delay;
}

static void write_pattern(unsigned long long count, unsigned int delay)
{
    unsigned char buffer[4096];
    unsigned long long offset = 0;

    while (offset < count) {
        size_t length = sizeof(buffer);
        unsigned long long remaining = count - offset;
        if (remaining < length) length = (size_t)remaining;
        for (size_t index = 0; index < length; index++) {
            buffer[index] = (unsigned char)((offset + index) % 251);
        }
        if (fwrite(buffer, 1, length, stdout) != length) exit(3);
        if (fflush(stdout) != 0) exit(3);
        offset += length;
        if (delay != 0) sleep_milliseconds(delay);
    }
}

static void print_arguments(int argc, char **argv)
{
    for (int index = 2; index < argc; index++) {
        printf("%d:%s\n", index - 2, argv[index]);
    }
}

static void print_environment(char **environment)
{
    if (environment == NULL) return;
    for (char **entry = environment; *entry != NULL; entry++) {
        puts(*entry);
    }
}

static int run_slow_input(unsigned int delay)
{
    unsigned char buffer[1024];
    size_t length;
    while ((length = fread(buffer, 1, sizeof(buffer), stdin)) != 0) {
        if (fwrite(buffer, 1, length, stdout) != length) return 3;
        if (fflush(stdout) != 0) return 3;
        sleep_milliseconds(delay);
    }
    return ferror(stdin) ? 3 : 0;
}

static int run_copy_input(unsigned long long count, unsigned int delay)
{
    unsigned char buffer[4096];
    unsigned long long remaining = count;
    while (remaining != 0) {
        size_t requested = sizeof(buffer);
        if (remaining < requested) requested = (size_t)remaining;
        const size_t length = fread(buffer, 1, requested, stdin);
        if (length != requested) return 3;
        if (fwrite(buffer, 1, length, stdout) != length) return 3;
        if (fflush(stdout) != 0) return 3;
        if (delay != 0) sleep_milliseconds(delay);
        remaining -= length;
    }
    return 0;
}

static int run_print_size(unsigned int delay)
{
    sleep_milliseconds(delay);
#if defined(_WIN32)
    puts("0 0 0 0");
#else
    struct winsize window;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) != 0) return 3;
    printf("%u %u %u %u\n",
           (unsigned int)window.ws_row,
           (unsigned int)window.ws_col,
           (unsigned int)window.ws_xpixel,
           (unsigned int)window.ws_ypixel);
#endif
    return fflush(stdout) == 0 ? 0 : 3;
}

static int run_echo(int argc, char **argv)
{
    for (int index = 2; index < argc; index++) {
        if (index != 2) putchar(' ');
        fputs(argv[index], stdout);
    }
    putchar('\n');
    return fflush(stdout) == 0 ? 0 : 3;
}

static int run_hold(void)
{
    while (1) sleep_milliseconds(1000);
    return 0;
}

#if defined(_WIN32)
static int spawn_self_process(const char *self, const char *argument)
{
    char executable[MAX_PATH];
    const DWORD executable_length = GetModuleFileNameA(
        NULL,
        executable,
        (DWORD)sizeof(executable));
    if (executable_length == 0 || executable_length >= sizeof(executable)) {
        return 3;
    }
    char command_line[MAX_PATH + 16];
    const int command_length = snprintf(
        command_line,
        sizeof(command_line),
        "\"%s\" %s",
        executable,
        argument);
    if (command_length < 0 || (size_t)command_length >= sizeof(command_line)) {
        return 3;
    }
    STARTUPINFOA startup = {0};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {0};
    if (!CreateProcessA(executable,
                        command_line,
                        NULL,
                        NULL,
                        TRUE,
                        0,
                        NULL,
                        NULL,
                        &startup,
                        &process)) {
        return 3;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
#else
static int spawn_self_process(const char *self, const char *argument)
{
    const pid_t child = fork();
    if (child < 0) return 3;
    if (child == 0) {
        execl(self, self, argument, (char *)NULL);
        _exit(127);
    }
    return 0;
}
#endif

static int run_spawn_child(const char *self)
{
    if (spawn_self_process(self, "hold") != 0) return 3;
    puts("child-started");
    if (fflush(stdout) != 0) return 3;
    while (1) sleep_milliseconds(1000);
}

static int run_spawn_grandchild(const char *self)
{
    if (spawn_self_process(self, "spawn-child") != 0) return 3;
    puts("child-started");
    if (fflush(stdout) != 0) return 3;
    while (1) sleep_milliseconds(1000);
}

int main(int argc, char **argv)
{
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    if (argc < 2) {
        fprintf(stderr, "missing command\n");
        return 2;
    }

    if (strcmp(argv[1], "echo") == 0) {
        return run_echo(argc, argv);
    }
    if (strcmp(argv[1], "hold") == 0 && argc == 2) {
        return run_hold();
    }
    if (strcmp(argv[1], "spawn-child") == 0 && argc == 2) {
        return run_spawn_child(argv[0]);
    }
    if (strcmp(argv[1], "spawn-grandchild") == 0 && argc == 2) {
        return run_spawn_grandchild(argv[0]);
    }
    if (strcmp(argv[1], "echo-binary") == 0 && argc == 3) {
        write_pattern(parse_count(argv[2]), 0);
        return 0;
    }
    if (strcmp(argv[1], "print-argv") == 0) {
        print_arguments(argc, argv);
        return 0;
    }
    if (strcmp(argv[1], "print-env") == 0) {
#if defined(_WIN32)
        extern char **_environ;
        print_environment(_environ);
#else
        extern char **environ;
        print_environment(environ);
#endif
        return 0;
    }
    if (strcmp(argv[1], "print-cwd") == 0) {
        char cwd[4096];
#if defined(_WIN32)
        if (_getcwd(cwd, sizeof(cwd)) == NULL) return 3;
#else
        if (getcwd(cwd, sizeof(cwd)) == NULL) return 3;
#endif
        puts(cwd);
        return fflush(stdout) == 0 ? 0 : 3;
    }
    if (strcmp(argv[1], "flood-output") == 0 && argc == 3) {
        write_pattern(parse_count(argv[2]), 0);
        return 0;
    }
    if (strcmp(argv[1], "slow-output") == 0 && argc == 4) {
        write_pattern(parse_count(argv[2]), parse_delay(argv[3]));
        return 0;
    }
    if (strcmp(argv[1], "slow-input") == 0 && argc == 3) {
        return run_slow_input(parse_delay(argv[2]));
    }
    if (strcmp(argv[1], "copy-input") == 0 && argc == 3) {
        return run_copy_input(parse_count(argv[2]), 0);
    }
    if (strcmp(argv[1], "slow-copy-input") == 0 && argc == 4) {
        return run_copy_input(parse_count(argv[2]), parse_delay(argv[3]));
    }
    if (strcmp(argv[1], "print-size") == 0 && argc == 2) {
        return run_print_size(0);
    }
    if (strcmp(argv[1], "print-size-after") == 0 && argc == 3) {
        return run_print_size(parse_delay(argv[2]));
    }
    if (strcmp(argv[1], "exit") == 0 && argc == 3) {
        return (int)parse_count(argv[2]);
    }
    if (strcmp(argv[1], "exit-after-output") == 0 && argc >= 4) {
        fputs(argv[3], stdout);
        if (fflush(stdout) != 0) return 3;
        return (int)parse_count(argv[2]);
    }
    if (strcmp(argv[1], "crash") == 0 && argc == 2) {
        raise(SIGSEGV);
        return 3;
    }

    fprintf(stderr, "invalid command\n");
    return 2;
}
