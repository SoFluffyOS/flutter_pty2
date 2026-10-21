#ifndef FLUTTER_PTY_UNIX_SPAWN_H_
#define FLUTTER_PTY_UNIX_SPAWN_H_

#include <stdint.h>
#include <sys/types.h>

#include "../flutter_pty.h"

typedef enum PtyChildStage {
    PTY_CHILD_STAGE_NONE = 0,
    PTY_CHILD_STAGE_SIGNAL_MASK = 1,
    PTY_CHILD_STAGE_SIGNAL_DISPOSITION = 2,
    PTY_CHILD_STAGE_SETSID = 3,
    PTY_CHILD_STAGE_CONTROLLING_TTY = 4,
    PTY_CHILD_STAGE_DUP2 = 5,
    PTY_CHILD_STAGE_CHDIR = 6,
    PTY_CHILD_STAGE_EXEC = 7,
    PTY_CHILD_STAGE_SUPERVISOR = 8
} PtyChildStage;

typedef struct PtyChildError {
    int32_t stage;
    int32_t error_number;
} PtyChildError;

typedef struct PtyUnixOwnedOptions {
    PtySpawnOptions options;
    char *executable;
    char **arguments;
    char **environment;
    char *working_directory;
} PtyUnixOwnedOptions;

int pty_unix_clone_options(const PtySpawnOptions *source,
                           PtyUnixOwnedOptions *destination,
                           PtyError *error);
void pty_unix_free_options(PtyUnixOwnedOptions *options);

/*
 * A spawned program and the supervisor that waits for it.
 *
 * The program is not a child of the calling process. A short-lived launcher
 * forks the supervisor, which forks the program. When the program exits, the
 * supervisor reports how through exit_fd but leaves it unreaped, so its pid
 * stays reserved and signals cannot reach a recycled pid. Once the caller has
 * recorded the exit it calls pty_unix_release_process, and the supervisor
 * reaps the program. The calling process never waits for the program itself,
 * so a runtime that reaps every child (such as the Dart VM while it has its
 * own child processes running) cannot take the program's exit status.
 */
typedef struct PtyUnixProcess {
    /* The program. Signals and process-group operations target it. */
    pid_t process_id;
    /* Read end of the pipe that receives the program's exit report. */
    int exit_fd;
    /* Write end whose closing lets the supervisor reap the program. */
    int release_fd;
} PtyUnixProcess;

typedef struct PtyUnixExit {
    /* Nonzero when a signal ended the program. */
    int32_t signaled;
    /* The exit code, or the signal number when signaled. */
    int32_t value;
} PtyUnixExit;

int pty_unix_spawn(const PtySpawnOptions *options,
                   int *master_fd,
                   int *slave_fd,
                   PtyUnixProcess *process,
                   PtyError *error);

/*
 * Blocks until the program exits and reads how. Returns 1 with *exit set, 0
 * when the supervisor ended without a report, or -1 with errno set.
 */
int pty_unix_read_exit(int exit_fd, PtyUnixExit *exit);

/*
 * Closes both descriptors, after which the supervisor reaps the program and
 * exits. Call it once the exit has been read, or to give up on the program.
 */
void pty_unix_release_process(PtyUnixProcess *process);

#endif
