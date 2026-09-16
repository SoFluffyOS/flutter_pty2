#include <errno.h>
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

static int run_echo(int argc, char **argv)
{
    for (int index = 2; index < argc; index++) {
        if (index != 2) putchar(' ');
        fputs(argv[index], stdout);
    }
    putchar('\n');
    return fflush(stdout) == 0 ? 0 : 3;
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
    if (strcmp(argv[1], "exit") == 0 && argc == 3) {
        return (int)parse_count(argv[2]);
    }
    if (strcmp(argv[1], "exit-after-output") == 0 && argc >= 4) {
        fputs(argv[3], stdout);
        if (fflush(stdout) != 0) return 3;
        return (int)parse_count(argv[2]);
    }

    fprintf(stderr, "invalid command\n");
    return 2;
}
