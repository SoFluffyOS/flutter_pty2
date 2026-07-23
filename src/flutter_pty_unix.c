
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "forkpty.h"
#include "flutter_pty.h"

#include "include/dart_api.h"
#include "include/dart_api_dl.h"
#include "include/dart_native_api.h"

#define MAX_PENDING_WRITE_BYTES (8 * 1024 * 1024)

typedef struct WriteChunk
{
    uint8_t *data;

    size_t length;

    size_t written;

    struct WriteChunk *next;

} WriteChunk;

typedef struct PtyHandle
{
    int ptm;

    int pid;

    pthread_mutex_t mutex;

    pthread_t read_thread;

    int wake_pipe[2];

    bool stopping;

    bool awaiting_read_ack;

    bool ackRead;

    WriteChunk *write_head;

    WriteChunk *write_tail;

    size_t pending_write_bytes;

} PtyHandle;

typedef struct ReadLoopOptions
{
    PtyHandle *handle;

    Dart_Port port;

    Dart_Port done_port;

} ReadLoopOptions;

static _Thread_local char error_buffer[256];

extern char **environ;

static void set_error(const char *operation, int error_number)
{
    snprintf(error_buffer,
             sizeof(error_buffer),
             "%s: %s",
             operation,
             strerror(error_number));
}

static void wake_event_loop(PtyHandle *handle)
{
    ssize_t result;
    do
    {
        result = write(handle->wake_pipe[1], "w", 1);
    } while (result < 0 && errno == EINTR);
}

static void drain_wake_pipe(PtyHandle *handle)
{
    char buffer[256];
    while (1)
    {
        ssize_t result = read(handle->wake_pipe[0], buffer, sizeof(buffer));
        if (result > 0) continue;
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        return;
    }
}

static bool flush_write_queue(PtyHandle *handle)
{
    pthread_mutex_lock(&handle->mutex);

    while (handle->write_head != NULL)
    {
        WriteChunk *chunk = handle->write_head;
        ssize_t result = write(handle->ptm,
                               chunk->data + chunk->written,
                               chunk->length - chunk->written);

        if (result > 0)
        {
            chunk->written += result;
            if (chunk->written < chunk->length) continue;

            handle->write_head = chunk->next;
            if (handle->write_head == NULL) handle->write_tail = NULL;
            handle->pending_write_bytes -= chunk->length;
            free(chunk->data);
            free(chunk);
            continue;
        }

        if (result < 0 && errno == EINTR) continue;
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            pthread_mutex_unlock(&handle->mutex);
            return true;
        }

        pthread_mutex_unlock(&handle->mutex);
        return false;
    }

    pthread_mutex_unlock(&handle->mutex);
    return true;
}

static void *read_loop(void *arg)
{
    ReadLoopOptions *options = (ReadLoopOptions *)arg;

    PtyHandle *handle = options->handle;
    char buffer[16384];

    while (1)
    {
        pthread_mutex_lock(&handle->mutex);
        bool stopping = handle->stopping;
        bool awaiting_read_ack = handle->awaiting_read_ack;
        bool has_pending_writes = handle->write_head != NULL;
        pthread_mutex_unlock(&handle->mutex);

        if (stopping) break;

        struct pollfd descriptors[2] = {
            { .fd = handle->ptm,
              .events = (awaiting_read_ack ? 0 : POLLIN) |
                        (has_pending_writes ? POLLOUT : 0) },
            { .fd = handle->wake_pipe[0], .events = POLLIN },
        };

        int poll_result;
        do
        {
            poll_result = poll(descriptors, 2, -1);
        } while (poll_result < 0 && errno == EINTR);

        if (poll_result < 0) break;

        if (descriptors[1].revents & POLLIN)
        {
            drain_wake_pipe(handle);

            pthread_mutex_lock(&handle->mutex);
            stopping = handle->stopping;
            has_pending_writes = handle->write_head != NULL;
            pthread_mutex_unlock(&handle->mutex);

            if (stopping) break;
            if (has_pending_writes && !flush_write_queue(handle)) break;
        }

        if (descriptors[0].revents & POLLOUT)
        {
            if (!flush_write_queue(handle)) break;
        }

        if (!(descriptors[0].revents & POLLIN))
        {
            if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
            continue;
        }

        ssize_t n;
        do
        {
            n = read(handle->ptm, buffer, sizeof(buffer));
        } while (n < 0 && errno == EINTR);

        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }

        if (n == 0)
        {
            break;
        }

        Dart_CObject result;
        result.type = Dart_CObject_kTypedData;
        result.value.as_typed_data.type = Dart_TypedData_kUint8;
        result.value.as_typed_data.length = n;
        result.value.as_typed_data.values = (uint8_t *)buffer;

        if (handle->ackRead)
        {
            pthread_mutex_lock(&handle->mutex);
            handle->awaiting_read_ack = true;
            pthread_mutex_unlock(&handle->mutex);
        }

        if (!Dart_PostCObject_DL(options->port, &result)) break;
    }

    pthread_mutex_lock(&handle->mutex);
    handle->stopping = true;
    pthread_mutex_unlock(&handle->mutex);

    Dart_PostInteger_DL(options->done_port, 0);
    free(options);
    return NULL;
}

static int start_read_thread(PtyHandle *handle, Dart_Port port, Dart_Port done_port)
{
    ReadLoopOptions *options = malloc(sizeof(ReadLoopOptions));
    if (options == NULL) return ENOMEM;

    options->handle = handle;
    options->port = port;
    options->done_port = done_port;

    int result = pthread_create(&handle->read_thread, NULL, &read_loop, options);
    if (result != 0)
    {
        free(options);
        return result;
    }

    return 0;
}

typedef struct WaitExitOptions
{
    int pid;

    Dart_Port port;

} WaitExitOptions;

static void *wait_exit_thread(void *arg)
{
    WaitExitOptions *options = (WaitExitOptions *)arg;

    int status = 0;
    pid_t result;
    do
    {
        result = waitpid(options->pid, &status, 0);
    } while (result < 0 && errno == EINTR);

    if (result < 0)
    {
        Dart_PostInteger_DL(options->port, -1);
    }
    else if (WIFEXITED(status))
    {
        Dart_PostInteger_DL(options->port, WEXITSTATUS(status));
    }
    else if (WIFSIGNALED(status))
    {
        Dart_PostInteger_DL(options->port, -WTERMSIG(status));
    }
    else
    {
        Dart_PostInteger_DL(options->port, -1);
    }

    free(options);
    return NULL;
}

static int start_wait_exit_thread(int pid, Dart_Port port)
{
    WaitExitOptions *options = malloc(sizeof(WaitExitOptions));
    if (options == NULL) return ENOMEM;

    options->pid = pid;

    options->port = port;

    pthread_t thread;

    int result = pthread_create(&thread, NULL, &wait_exit_thread, options);
    if (result != 0)
    {
        free(options);
        return result;
    }

    pthread_detach(thread);
    return 0;
}

static void set_environment(char **environment)
{
    if (environment == NULL) return;
    environ = environment;
}

static void enable_utf8_input_mode(int fd)
{
#ifdef IUTF8
    struct termios attributes;
    if (tcgetattr(fd, &attributes) != 0)
    {
        return;
    }

    attributes.c_iflag |= IUTF8;
    tcsetattr(fd, TCSANOW, &attributes);
#else
    (void)fd;
#endif
}

FFI_PLUGIN_EXPORT PtyHandle *pty_create(PtyOptions *options)
{
    error_buffer[0] = '\0';
    if (options == NULL || options->executable == NULL ||
        options->arguments == NULL || options->rows <= 0 ||
        options->rows > INT16_MAX || options->cols <= 0 ||
        options->cols > INT16_MAX)
    {
        set_error("invalid PTY options", EINVAL);
        return NULL;
    }
    struct winsize ws = {0};

    ws.ws_row = options->rows;
    ws.ws_col = options->cols;

    int ptm;

    int exec_status_pipe[2];
    if (pipe(exec_status_pipe) != 0)
    {
        set_error("failed to create child status pipe", errno);
        return NULL;
    }

    int status_flags = fcntl(exec_status_pipe[1], F_GETFD);
    if (status_flags < 0 ||
        fcntl(exec_status_pipe[1], F_SETFD, status_flags | FD_CLOEXEC) < 0)
    {
        int error_number = errno;
        close(exec_status_pipe[0]);
        close(exec_status_pipe[1]);
        set_error("failed to configure child status pipe", error_number);
        return NULL;
    }

    int pid = pty_forkpty(&ptm, NULL, NULL, &ws, exec_status_pipe[1]);

    if (pid < 0)
    {
        int error_number = errno;
        close(exec_status_pipe[0]);
        close(exec_status_pipe[1]);
        set_error("pty_forkpty failed", error_number);
        return NULL;
    }

    if (pid == 0)
    {
        close(exec_status_pipe[0]);
        set_environment(options->environment);

        if (options->working_directory != NULL && strlen(options->working_directory) > 0)
        {
            if (chdir(options->working_directory) != 0)
            {
                int error_number = errno;
                write(exec_status_pipe[1], &error_number, sizeof(error_number));
                _exit(126);
            }
        }

        execvp(options->executable, options->arguments);
        int error_number = errno;
        write(exec_status_pipe[1], &error_number, sizeof(error_number));
        _exit(127);
    }

    close(exec_status_pipe[1]);
    int child_error = 0;
    ssize_t status_length;
    do
    {
        status_length = read(exec_status_pipe[0], &child_error, sizeof(child_error));
    } while (status_length < 0 && errno == EINTR);
    close(exec_status_pipe[0]);

    if (status_length != 0)
    {
        if (status_length < 0)
        {
            child_error = errno;
            kill(pid, SIGKILL);
        }
        close(ptm);
        waitpid(pid, NULL, 0);
        set_error("failed to start PTY child", child_error);
        return NULL;
    }

    PtyHandle *handle = (PtyHandle *)calloc(1, sizeof(PtyHandle));
    if (handle == NULL)
    {
        close(ptm);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        set_error("failed to allocate PTY handle", ENOMEM);
        return NULL;
    }

    handle->ptm = ptm;
    handle->pid = pid;
    handle->ackRead = options->ackRead;
    pthread_mutex_init(&handle->mutex, NULL);

    enable_utf8_input_mode(ptm);

    int ptm_flags = fcntl(ptm, F_GETFL);
    if (ptm_flags < 0 || fcntl(ptm, F_SETFL, ptm_flags | O_NONBLOCK) < 0)
    {
        int error_number = errno;
        close(ptm);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        pthread_mutex_destroy(&handle->mutex);
        free(handle);
        set_error("failed to set PTY nonblocking mode", error_number);
        return NULL;
    }

    if (pipe(handle->wake_pipe) != 0)
    {
        int error_number = errno;
        close(ptm);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        pthread_mutex_destroy(&handle->mutex);
        free(handle);
        set_error("failed to create PTY wake pipe", error_number);
        return NULL;
    }

    int wake_read_flags = fcntl(handle->wake_pipe[0], F_GETFL);
    int wake_write_flags = fcntl(handle->wake_pipe[1], F_GETFL);
    int wake_read_fd_flags = fcntl(handle->wake_pipe[0], F_GETFD);
    int wake_write_fd_flags = fcntl(handle->wake_pipe[1], F_GETFD);
    if (wake_read_flags < 0 || wake_write_flags < 0 ||
        wake_read_fd_flags < 0 || wake_write_fd_flags < 0 ||
        fcntl(handle->wake_pipe[0], F_SETFL, wake_read_flags | O_NONBLOCK) < 0 ||
        fcntl(handle->wake_pipe[1], F_SETFL, wake_write_flags | O_NONBLOCK) < 0 ||
        fcntl(handle->wake_pipe[0],
              F_SETFD,
              wake_read_fd_flags | FD_CLOEXEC) < 0 ||
        fcntl(handle->wake_pipe[1],
              F_SETFD,
              wake_write_fd_flags | FD_CLOEXEC) < 0)
    {
        int error_number = errno;
        close(handle->wake_pipe[0]);
        close(handle->wake_pipe[1]);
        close(ptm);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        pthread_mutex_destroy(&handle->mutex);
        free(handle);
        set_error("failed to set PTY wake pipe nonblocking mode", error_number);
        return NULL;
    }

    int read_thread_error = start_read_thread(handle,
                                              options->stdout_port,
                                              options->output_done_port);
    if (read_thread_error != 0)
    {
        close(handle->wake_pipe[0]);
        close(handle->wake_pipe[1]);
        close(ptm);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        pthread_mutex_destroy(&handle->mutex);
        free(handle);
        set_error("failed to start PTY read thread", read_thread_error);
        return NULL;
    }

    int wait_thread_error = start_wait_exit_thread(pid, options->exit_port);
    if (wait_thread_error != 0)
    {
        kill(pid, SIGKILL);
        pty_destroy(handle);
        waitpid(pid, NULL, 0);
        set_error("failed to start PTY wait thread", wait_thread_error);
        return NULL;
    }

    return handle;
}

FFI_PLUGIN_EXPORT int pty_write(PtyHandle *handle, char *buffer, int length)
{
    if (handle == NULL || buffer == NULL || length <= 0) return 0;
    if ((size_t)length > MAX_PENDING_WRITE_BYTES) return 0;

    WriteChunk *chunk = malloc(sizeof(WriteChunk));
    if (chunk == NULL) return 0;

    chunk->data = malloc(length);
    if (chunk->data == NULL)
    {
        free(chunk);
        return 0;
    }

    memcpy(chunk->data, buffer, length);
    chunk->length = length;
    chunk->written = 0;
    chunk->next = NULL;

    pthread_mutex_lock(&handle->mutex);
    if (handle->stopping)
    {
        pthread_mutex_unlock(&handle->mutex);
        free(chunk->data);
        free(chunk);
        return 0;
    }

    if (handle->pending_write_bytes > MAX_PENDING_WRITE_BYTES - (size_t)length)
    {
        pthread_mutex_unlock(&handle->mutex);
        free(chunk->data);
        free(chunk);
        return 0;
    }

    if (handle->write_tail == NULL)
    {
        handle->write_head = chunk;
    }
    else
    {
        handle->write_tail->next = chunk;
    }
    handle->write_tail = chunk;
    handle->pending_write_bytes += chunk->length;
    pthread_mutex_unlock(&handle->mutex);
    wake_event_loop(handle);
    return 1;
}

FFI_PLUGIN_EXPORT void pty_ack_read(PtyHandle *handle)
{
    if (handle == NULL) return;

    if (handle->ackRead)
    {
        pthread_mutex_lock(&handle->mutex);
        bool should_wake = handle->awaiting_read_ack && !handle->stopping;
        handle->awaiting_read_ack = false;
        pthread_mutex_unlock(&handle->mutex);

        if (should_wake) wake_event_loop(handle);
    }
}

FFI_PLUGIN_EXPORT int pty_resize(PtyHandle *handle,
                                 int rows,
                                 int cols,
                                 int pixel_width,
                                 int pixel_height)
{
    error_buffer[0] = '\0';
    if (handle == NULL || rows <= 0 || rows > INT16_MAX ||
        cols <= 0 || cols > INT16_MAX ||
        pixel_width < 0 || pixel_width > UINT16_MAX ||
        pixel_height < 0 || pixel_height > UINT16_MAX)
    {
        errno = EINVAL;
        set_error("invalid PTY size", EINVAL);
        return -1;
    }

    struct winsize ws = {0};

    ws.ws_row = rows;
    ws.ws_col = cols;
    ws.ws_xpixel = pixel_width;
    ws.ws_ypixel = pixel_height;

    int result;
    do
    {
        result = ioctl(handle->ptm, TIOCSWINSZ, &ws);
    } while (result < 0 && errno == EINTR);

    if (result < 0) set_error("failed to resize PTY", errno);
    return result;
}

FFI_PLUGIN_EXPORT int pty_getpid(PtyHandle *handle)
{
    if (handle == NULL) return -1;
    return handle->pid;
}

FFI_PLUGIN_EXPORT int pty_has_running_foreground_process(PtyHandle *handle)
{
    if (handle == NULL) return 0;

    pid_t foreground_process_group = tcgetpgrp(handle->ptm);
    if (foreground_process_group < 0) return 0;

    pid_t shell_process_group = getpgid(handle->pid);
    if (shell_process_group < 0) return 0;

    return foreground_process_group != shell_process_group;
}

FFI_PLUGIN_EXPORT int pty_kill(PtyHandle *handle, int signal_number)
{
    if (handle == NULL || signal_number <= 0) return 0;

    pid_t foreground_process_group = tcgetpgrp(handle->ptm);
    if (foreground_process_group > 0)
    {
        if (kill(-foreground_process_group, signal_number) == 0) return 1;
        if (errno != ESRCH) return 0;
    }

    return kill(handle->pid, signal_number) == 0;
}

static void hangup_pty_processes(PtyHandle *handle)
{
    pid_t foreground_process_group = tcgetpgrp(handle->ptm);
    if (foreground_process_group > 0)
    {
        kill(-foreground_process_group, SIGHUP);
    }

    pid_t shell_process_group = getpgid(handle->pid);
    if (shell_process_group > 0 &&
        shell_process_group != foreground_process_group)
    {
        kill(-shell_process_group, SIGHUP);
        return;
    }

    if (shell_process_group < 0)
    {
        kill(handle->pid, SIGHUP);
    }
}

FFI_PLUGIN_EXPORT char *pty_error(void)
{
    return error_buffer[0] == '\0' ? NULL : error_buffer;
}

FFI_PLUGIN_EXPORT void pty_destroy(PtyHandle *handle)
{
    if (handle == NULL) return;

    hangup_pty_processes(handle);

    pthread_mutex_lock(&handle->mutex);
    handle->stopping = true;
    pthread_mutex_unlock(&handle->mutex);
    wake_event_loop(handle);
    pthread_join(handle->read_thread, NULL);

    WriteChunk *chunk = handle->write_head;
    while (chunk != NULL)
    {
        WriteChunk *next = chunk->next;
        free(chunk->data);
        free(chunk);
        chunk = next;
    }

    close(handle->ptm);
    close(handle->wake_pipe[0]);
    close(handle->wake_pipe[1]);
    pthread_mutex_destroy(&handle->mutex);
    free(handle);
}
