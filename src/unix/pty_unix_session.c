#include "pty_unix_spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "../common/pty_error.h"
#include "../common/pty_event.h"
#include "../pty_internal.h"

#define PTY_REACTOR_BUFFER_SIZE (16 * 1024)
#define PTY_WAKE_BUFFER_SIZE 64

typedef struct PtyUnixPlatform {
    pthread_mutex_t mutex;
    int master_fd;
    pid_t process_id;
    int wake_pipe[2];
    pthread_t reactor_thread;
    pthread_t waiter_thread;
    int reactor_started;
    int waiter_started;
    int reactor_done;
    int waiter_done;
    int stopping;
    int write_backpressured;
    int discard_output;
    int output_closed;
    int input_closed;
    int session_closed_posted;
    PtyWriteQueue write_queue;
} PtyUnixPlatform;

typedef struct PtyUnixBootstrap {
    PtySession *session;
    PtyUnixOwnedOptions options;
} PtyUnixBootstrap;

static PtyUnixPlatform *platform_for(PtySession *session)
{
    return session == NULL ? NULL : (PtyUnixPlatform *)session->platform;
}

static void wake_reactor(PtyUnixPlatform *platform)
{
    const uint8_t marker = 1;
    while (true) {
        const ssize_t result = write(platform->wake_pipe[1], &marker, 1);
        if (result >= 0) return;
        if (errno == EINTR) continue;
        return;
    }
}

static void pty_unix_drain_wake_pipe(PtyUnixPlatform *platform)
{
    uint8_t buffer[PTY_WAKE_BUFFER_SIZE];
    while (true) {
        const ssize_t result = read(platform->wake_pipe[0], buffer, sizeof(buffer));
        if (result > 0) continue;
        if (result < 0 && errno == EINTR) continue;
        return;
    }
}

static int set_nonblocking_cloexec(int fd)
{
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    const int descriptor_flags = fcntl(fd, F_GETFD);
    if (descriptor_flags < 0) return -1;
    return fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC);
}

static int create_wake_pipe(PtyUnixPlatform *platform)
{
    platform->wake_pipe[0] = -1;
    platform->wake_pipe[1] = -1;
    if (pipe(platform->wake_pipe) != 0) return -1;
    if (set_nonblocking_cloexec(platform->wake_pipe[0]) == 0 &&
        set_nonblocking_cloexec(platform->wake_pipe[1]) == 0) {
        return 0;
    }
    close(platform->wake_pipe[0]);
    close(platform->wake_pipe[1]);
    platform->wake_pipe[0] = -1;
    platform->wake_pipe[1] = -1;
    return -1;
}

static void post_child_error(PtySession *session,
                             PtyErrorKind kind,
                             int error_number,
                             const char *operation)
{
    PtyError error;
    pty_error_set_errno(&error, kind, error_number, operation);
    if (!pty_post_error(session->event_port, &error)) {
        pty_session_mark_closing(session);
    }
}

static void maybe_post_session_closed(PtySession *session)
{
    PtyUnixPlatform *platform = platform_for(session);
    if (platform == NULL) return;
    int should_post = 0;
    pthread_mutex_lock(&platform->mutex);
    if (platform->stopping && platform->reactor_done && platform->waiter_done &&
        !platform->session_closed_posted) {
        platform->session_closed_posted = 1;
        should_post = 1;
    }
    pthread_mutex_unlock(&platform->mutex);
    if (should_post) pty_post_simple_event(session->event_port,
                                            PTY_EVENT_SESSION_CLOSED);
}

static void mark_output_closed(PtySession *session)
{
    PtyUnixPlatform *platform = platform_for(session);
    if (platform == NULL) return;
    int should_post = 0;
    pthread_mutex_lock(&platform->mutex);
    if (!platform->output_closed) {
        platform->output_closed = 1;
        should_post = 1;
    }
    pthread_mutex_unlock(&platform->mutex);
    if (!should_post) return;
    atomic_store_explicit(&session->output_closed, 1, memory_order_release);
    pty_post_simple_event(session->event_port, PTY_EVENT_OUTPUT_CLOSED);
}

static void mark_input_closed(PtySession *session, const PtyError *error)
{
    PtyUnixPlatform *platform = platform_for(session);
    if (platform == NULL) return;
    int should_post = 0;
    pthread_mutex_lock(&platform->mutex);
    if (!platform->input_closed) {
        platform->input_closed = 1;
        should_post = 1;
    }
    pthread_mutex_unlock(&platform->mutex);
    if (!should_post) return;
    atomic_store_explicit(&session->input_closed, 1, memory_order_release);
    if (error == NULL) {
        PtyError closed_error;
        pty_error_set_errno(&closed_error,
                            PTY_ERROR_CLOSED,
                            EPIPE,
                            "PTY input closed");
        pty_post_input_closed(session->event_port, &closed_error);
        return;
    }
    pty_post_input_closed(session->event_port, error);
}

static int pty_unix_flush_write_queue(PtySession *session)
{
    PtyUnixPlatform *platform = platform_for(session);
    if (platform == NULL) return 0;
    while (true) {
        PtyWriteChunk *chunk = pty_write_queue_dequeue(&platform->write_queue);
        if (chunk == NULL) return 1;
        while (chunk->offset < chunk->length) {
            const ssize_t result = write(platform->master_fd,
                                         chunk->bytes + chunk->offset,
                                         chunk->length - chunk->offset);
            if (result > 0) {
                chunk->offset += (uint64_t)result;
                continue;
            }
            if (result < 0 && errno == EINTR) continue;
            if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (pty_write_queue_requeue_front(&platform->write_queue,
                                                  chunk) != PTY_WRITE_ACCEPTED) {
                    pty_write_chunk_free(chunk);
                    post_child_error(session, PTY_ERROR_IO, ENOBUFS,
                                     "requeuing partial write failed");
                    return 0;
                }
                return 1;
            }
            const int error_number = errno;
            pty_write_chunk_free(chunk);
            PtyError error;
            pty_error_set_errno(&error, PTY_ERROR_IO, error_number,
                                "writing PTY input failed");
            mark_input_closed(session, &error);
            return 0;
        }
        pty_post_write_complete(session->event_port, chunk->request_id);
        int should_post_writable = 0;
        pthread_mutex_lock(&platform->mutex);
        if (platform->write_backpressured &&
            pty_write_queue_pending_bytes(&platform->write_queue) <=
                session->input_buffer_limit / 2) {
            platform->write_backpressured = 0;
            should_post_writable = 1;
        }
        pthread_mutex_unlock(&platform->mutex);
        if (should_post_writable) {
            pty_post_simple_event(session->event_port, PTY_EVENT_WRITABLE);
        }
        pty_write_chunk_free(chunk);
    }
}

static int read_output(PtySession *session)
{
    PtyUnixPlatform *platform = platform_for(session);
    if (platform == NULL) return 0;
    uint8_t buffer[PTY_REACTOR_BUFFER_SIZE];
    pthread_mutex_lock(&platform->mutex);
    const int discard = platform->discard_output;
    uint64_t credit = session->output_credit;
    if (!discard && credit < sizeof(buffer)) {
        /* A zero credit means the reactor should not have been polled. */
        if (credit == 0) {
            pthread_mutex_unlock(&platform->mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&platform->mutex);

    size_t capacity = sizeof(buffer);
    pthread_mutex_lock(&platform->mutex);
    if (!platform->discard_output && session->output_credit < capacity) {
        capacity = (size_t)session->output_credit;
    }
    pthread_mutex_unlock(&platform->mutex);
    if (capacity == 0) return 1;

    const ssize_t result = read(platform->master_fd, buffer, capacity);
    if (result > 0) {
        if (!discard) {
            pthread_mutex_lock(&platform->mutex);
            if (session->output_credit >= (uint64_t)result) {
                session->output_credit -= (uint64_t)result;
            } else {
                session->output_credit = 0;
            }
            pthread_mutex_unlock(&platform->mutex);
            if (!pty_post_output(session->event_port, buffer, result)) {
                pthread_mutex_lock(&platform->mutex);
                platform->discard_output = 1;
                pthread_mutex_unlock(&platform->mutex);
            }
        }
        return 1;
    }
    if (result < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 1;
    }
    if (result < 0) {
        post_child_error(session, PTY_ERROR_IO, errno, "reading PTY output failed");
    }
    return 0;
}

static void *reactor_worker(void *argument)
{
    PtySession *session = argument;
    PtyUnixPlatform *platform = platform_for(session);
    while (true) {
        pthread_mutex_lock(&platform->mutex);
        const int stopping = platform->stopping;
        const int discard = platform->discard_output;
        const int has_writes = pty_write_queue_pending_bytes(&platform->write_queue) != 0;
        const uint64_t credit = session->output_credit;
        pthread_mutex_unlock(&platform->mutex);
        if (stopping) break;

        short events = 0;
        if (discard || credit > 0) events |= POLLIN;
        if (has_writes) events |= POLLOUT;
        struct pollfd descriptors[2] = {
            {.fd = platform->master_fd, .events = events},
            {.fd = platform->wake_pipe[0], .events = POLLIN},
        };
        int poll_result;
        do {
            poll_result = poll(descriptors, 2, -1);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result < 0) {
            post_child_error(session, PTY_ERROR_IO, errno, "polling PTY failed");
            break;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            pty_unix_drain_wake_pipe(platform);
        }
        pthread_mutex_lock(&platform->mutex);
        const int stopping_after_wake = platform->stopping;
        pthread_mutex_unlock(&platform->mutex);
        if (stopping_after_wake) break;
        if ((descriptors[0].revents & POLLOUT) != 0 &&
            !pty_unix_flush_write_queue(session)) {
            pthread_mutex_lock(&platform->mutex);
            platform->stopping = 1;
            pthread_mutex_unlock(&platform->mutex);
            break;
        }
        if ((descriptors[0].revents & POLLIN) != 0 && !read_output(session)) break;
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            read_output(session);
            break;
        }
    }
    mark_output_closed(session);
    mark_input_closed(session, NULL);
    close(platform->master_fd);
    platform->master_fd = -1;
    pthread_mutex_lock(&platform->mutex);
    platform->reactor_done = 1;
    pthread_mutex_unlock(&platform->mutex);
    maybe_post_session_closed(session);
    pty_session_release(session);
    return NULL;
}

static void *waiter_worker(void *argument)
{
    PtySession *session = argument;
    PtyUnixPlatform *platform = platform_for(session);
    int status = 0;
    pid_t result;
    do {
        result = waitpid(platform->process_id, &status, 0);
    } while (result < 0 && errno == EINTR);
    if (result == platform->process_id) {
        if (WIFEXITED(status)) {
            pty_post_process_exit(session->event_port, false, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            pty_post_process_exit(session->event_port, true, WTERMSIG(status));
        }
    } else if (result < 0) {
        post_child_error(session, PTY_ERROR_IO, errno, "waiting for PTY process failed");
    }
    atomic_store_explicit(&session->process_exited, 1, memory_order_release);
    pthread_mutex_lock(&platform->mutex);
    platform->waiter_done = 1;
    pthread_mutex_unlock(&platform->mutex);
    maybe_post_session_closed(session);
    pty_session_release(session);
    return NULL;
}

static void free_unix_session(PtySession *session)
{
    PtyUnixPlatform *platform = platform_for(session);
    if (platform != NULL) {
        if (platform->master_fd >= 0) close(platform->master_fd);
        if (platform->wake_pipe[0] >= 0) close(platform->wake_pipe[0]);
        if (platform->wake_pipe[1] >= 0) close(platform->wake_pipe[1]);
        pty_write_queue_dispose(&platform->write_queue);
        pthread_mutex_destroy(&platform->mutex);
        free(platform);
    }
    free(session);
}

static void discard_unix_platform(PtySession *session)
{
    PtyUnixPlatform *platform = platform_for(session);
    if (platform == NULL) return;
    if (platform->master_fd >= 0) close(platform->master_fd);
    if (platform->wake_pipe[0] >= 0) close(platform->wake_pipe[0]);
    if (platform->wake_pipe[1] >= 0) close(platform->wake_pipe[1]);
    pty_write_queue_dispose(&platform->write_queue);
    pthread_mutex_destroy(&platform->mutex);
    free(platform);
    session->platform = NULL;
}

static void stop_process(PtyUnixPlatform *platform)
{
    if (platform == NULL || platform->process_id <= 0) return;
    const pid_t foreground = tcgetpgrp(platform->master_fd);
    if (foreground > 0) kill(-foreground, SIGKILL);
    kill(-platform->process_id, SIGKILL);
    kill(platform->process_id, SIGKILL);
}

static void *bootstrap_worker(void *argument)
{
    PtyUnixBootstrap *bootstrap = argument;
    PtySession *session = bootstrap->session;
    int master_fd = -1;
    pid_t process_id = -1;
    PtyError error;
    if (!pty_unix_spawn(&bootstrap->options.options,
                        &master_fd,
                        &process_id,
                        &error)) {
        pty_post_spawn_failed(session->event_port, &error);
        pty_post_simple_event(session->event_port, PTY_EVENT_OUTPUT_CLOSED);
        pty_post_simple_event(session->event_port, PTY_EVENT_SESSION_CLOSED);
        pty_session_mark_closing(session);
        pty_unix_free_options(&bootstrap->options);
        free(bootstrap);
        pty_session_release(session);
        return NULL;
    }

    PtyUnixPlatform *platform = calloc(1, sizeof(*platform));
    int mutex_initialized = 0;
    if (platform != NULL) {
        platform->master_fd = -1;
        platform->wake_pipe[0] = -1;
        platform->wake_pipe[1] = -1;
        if (pthread_mutex_init(&platform->mutex, NULL) == 0) {
            mutex_initialized = 1;
        }
    }
    if (platform == NULL || !mutex_initialized || create_wake_pipe(platform) != 0) {
        const int error_number = errno == 0 ? ENOMEM : errno;
        if (platform != NULL) {
            if (platform->wake_pipe[0] >= 0) close(platform->wake_pipe[0]);
            if (platform->wake_pipe[1] >= 0) close(platform->wake_pipe[1]);
            if (mutex_initialized) pthread_mutex_destroy(&platform->mutex);
            free(platform);
        }
        close(master_fd);
        kill(process_id, SIGKILL);
        while (waitpid(process_id, NULL, 0) < 0 && errno == EINTR) {}
        pty_error_set_errno(&error, PTY_ERROR_OUT_OF_MEMORY, error_number,
                            "allocating Unix PTY session failed");
        pty_post_spawn_failed(session->event_port, &error);
        pty_post_simple_event(session->event_port, PTY_EVENT_SESSION_CLOSED);
        pty_session_mark_closing(session);
        pty_unix_free_options(&bootstrap->options);
        free(bootstrap);
        pty_session_release(session);
        return NULL;
    }
    platform->master_fd = master_fd;
    platform->process_id = process_id;
    pty_write_queue_init(&platform->write_queue,
                         session->input_buffer_limit);
    session->platform = platform;

    pthread_mutex_lock(&platform->mutex);
    const int closing = platform->stopping ||
                        atomic_load_explicit(&session->lifecycle,
                                             memory_order_acquire) ==
                            PTY_LIFECYCLE_CLOSING;
    pthread_mutex_unlock(&platform->mutex);
    if (closing) {
        stop_process(platform);
        close(platform->master_fd);
        platform->master_fd = -1;
        while (waitpid(process_id, NULL, 0) < 0 && errno == EINTR) {}
        pty_post_simple_event(session->event_port, PTY_EVENT_SESSION_CLOSED);
        pty_unix_free_options(&bootstrap->options);
        free(bootstrap);
        pty_session_release(session);
        return NULL;
    }

    pty_session_retain(session);
    const int reactor_result = pthread_create(&platform->reactor_thread,
                                              NULL,
                                              reactor_worker,
                                              session);
    if (reactor_result != 0) {
        pty_session_release(session);
        stop_process(platform);
        while (waitpid(process_id, NULL, 0) < 0 && errno == EINTR) {}
        discard_unix_platform(session);
        pty_error_set_errno(&error, PTY_ERROR_INTERNAL, reactor_result,
                            "starting PTY reactor failed");
        pty_post_spawn_failed(session->event_port, &error);
        pty_post_simple_event(session->event_port, PTY_EVENT_OUTPUT_CLOSED);
        pty_post_simple_event(session->event_port, PTY_EVENT_SESSION_CLOSED);
        pty_unix_free_options(&bootstrap->options);
        free(bootstrap);
        pty_session_release(session);
        return NULL;
    }
    platform->reactor_started = 1;
    pty_session_retain(session);
    const int waiter_result = pthread_create(&platform->waiter_thread,
                                             NULL,
                                             waiter_worker,
                                             session);
    if (waiter_result != 0) {
        pty_session_release(session);
        pthread_mutex_lock(&platform->mutex);
        platform->stopping = 1;
        platform->waiter_done = 1;
        pthread_mutex_unlock(&platform->mutex);
        stop_process(platform);
        wake_reactor(platform);
        while (waitpid(process_id, NULL, 0) < 0 && errno == EINTR) {}
        pty_error_set_errno(&error, PTY_ERROR_INTERNAL, waiter_result,
                            "starting PTY waiter failed");
        pty_post_spawn_failed(session->event_port, &error);
        pty_unix_free_options(&bootstrap->options);
        free(bootstrap);
        pty_session_release(session);
        return NULL;
    }
    platform->waiter_started = 1;
    int expected_lifecycle = PTY_LIFECYCLE_STARTING;
    if (!atomic_compare_exchange_strong_explicit(&session->lifecycle,
                                                 &expected_lifecycle,
                                                 PTY_LIFECYCLE_RUNNING,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        pthread_mutex_lock(&platform->mutex);
        platform->stopping = 1;
        pthread_mutex_unlock(&platform->mutex);
        stop_process(platform);
        wake_reactor(platform);
        pty_unix_free_options(&bootstrap->options);
        free(bootstrap);
        pty_session_release(session);
        return NULL;
    }
    pty_post_spawned(session->event_port, process_id, 0x07);
    pty_unix_free_options(&bootstrap->options);
    free(bootstrap);
    pty_session_release(session);
    return NULL;
}

FFI_PLUGIN_EXPORT int32_t pty_session_start(const PtySpawnOptions *options,
                                            PtySession **out_session,
                                            PtyError *out_error)
{
    pty_error_clear(out_error);
    if (out_session != NULL) *out_session = NULL;
    if (options == NULL || out_session == NULL || options->executable == NULL ||
        options->executable[0] == '\0' || options->argument_count < 0 ||
        options->environment_count < 0 || options->input_buffer_bytes == 0 ||
        options->output_window_bytes == 0) {
        pty_error_set(out_error,
                      PTY_ERROR_DOMAIN_INTERNAL,
                      PTY_ERROR_INVALID_ARGUMENT,
                      EINVAL,
                      "invalid PTY session options");
        return 0;
    }
    PtySession *session = calloc(1, sizeof(*session));
    if (session == NULL) {
        pty_error_set(out_error,
                      PTY_ERROR_DOMAIN_INTERNAL,
                      PTY_ERROR_OUT_OF_MEMORY,
                      ENOMEM,
                      "allocating PTY session failed");
        return 0;
    }
    pty_session_init(session);
    session->event_port = (Dart_Port_DL)options->event_port;
    session->input_buffer_limit = options->input_buffer_bytes;
    session->output_window_limit = options->output_window_bytes;
    session->output_credit = options->output_window_bytes;
    session->free_function = free_unix_session;

    PtyUnixBootstrap *bootstrap = calloc(1, sizeof(*bootstrap));
    if (bootstrap == NULL) {
        pty_session_release(session);
        pty_error_set(out_error,
                      PTY_ERROR_DOMAIN_INTERNAL,
                      PTY_ERROR_OUT_OF_MEMORY,
                      ENOMEM,
                      "allocating PTY bootstrap failed");
        return 0;
    }
    if (!pty_unix_clone_options(options, &bootstrap->options, out_error)) {
        pty_unix_free_options(&bootstrap->options);
        free(bootstrap);
        pty_session_release(session);
        return 0;
    }
    bootstrap->session = session;
    pty_session_retain(session);
    pthread_t bootstrap_thread;
    const int bootstrap_result = pthread_create(&bootstrap_thread,
                                                NULL,
                                                bootstrap_worker,
                                                bootstrap);
    if (bootstrap_result != 0) {
        pty_session_release(session);
        pty_unix_free_options(&bootstrap->options);
        free(bootstrap);
        pty_session_release(session);
        pty_error_set_errno(out_error, PTY_ERROR_INTERNAL, bootstrap_result,
                            "starting PTY bootstrap failed");
        return 0;
    }
    pthread_detach(bootstrap_thread);
    *out_session = session;
    return 1;
}

FFI_PLUGIN_EXPORT int64_t pty_session_pid(PtySession *session)
{
    PtyUnixPlatform *platform = platform_for(session);
    return platform == NULL ? -1 : platform->process_id;
}

FFI_PLUGIN_EXPORT int32_t pty_session_try_write(PtySession *session,
                                                uint64_t request_id,
                                                const uint8_t *bytes,
                                                uint64_t length,
                                                PtyError *out_error)
{
    pty_error_clear(out_error);
    PtyUnixPlatform *platform = platform_for(session);
    if (platform == NULL || length == 0) return PTY_WRITE_CLOSED;
    pthread_mutex_lock(&platform->mutex);
    if (platform->stopping) {
        pthread_mutex_unlock(&platform->mutex);
        return PTY_WRITE_CLOSED;
    }
    const int result = pty_write_queue_try_enqueue(&platform->write_queue,
                                                   bytes,
                                                   length,
                                                   request_id);
    if (result == PTY_WRITE_BACKPRESSURED) {
        platform->write_backpressured = 1;
    }
    pthread_mutex_unlock(&platform->mutex);
    if (result == PTY_WRITE_ERROR) {
        pty_error_set(out_error,
                      PTY_ERROR_DOMAIN_INTERNAL,
                      PTY_ERROR_OUT_OF_MEMORY,
                      ENOMEM,
                      "queueing PTY input failed");
        return PTY_WRITE_ERROR;
    }
    if (result == PTY_WRITE_ACCEPTED) wake_reactor(platform);
    return result;
}

FFI_PLUGIN_EXPORT void pty_session_ack_output(PtySession *session,
                                              uint64_t byte_count)
{
    PtyUnixPlatform *platform = platform_for(session);
    if (platform == NULL) return;
    pthread_mutex_lock(&platform->mutex);
    const uint64_t limit = session->output_window_limit;
    if (byte_count > limit - session->output_credit) {
        session->output_credit = limit;
    } else {
        session->output_credit += byte_count;
    }
    pthread_mutex_unlock(&platform->mutex);
    wake_reactor(platform);
}

FFI_PLUGIN_EXPORT int32_t pty_session_resize(PtySession *session,
                                              PtySize size,
                                              PtyError *out_error)
{
    pty_error_clear(out_error);
    PtyUnixPlatform *platform = platform_for(session);
    if (platform == NULL || platform->master_fd < 0) {
        pty_error_set(out_error, PTY_ERROR_DOMAIN_INTERNAL, PTY_ERROR_CLOSED,
                      EPIPE, "PTY session is closed");
        return 0;
    }
    struct winsize window = {
        .ws_row = (unsigned short)size.rows,
        .ws_col = (unsigned short)size.columns,
        .ws_xpixel = (unsigned short)size.pixel_width,
        .ws_ypixel = (unsigned short)size.pixel_height,
    };
    if (ioctl(platform->master_fd, TIOCSWINSZ, &window) != 0) {
        pty_error_set_errno(out_error, PTY_ERROR_IO, errno,
                            "resizing PTY failed");
        return 0;
    }
    return 1;
}

FFI_PLUGIN_EXPORT int32_t pty_session_kill(PtySession *session,
                                           PtyError *out_error)
{
    pty_error_clear(out_error);
    PtyUnixPlatform *platform = platform_for(session);
    if (platform == NULL) {
        pty_error_set(out_error, PTY_ERROR_DOMAIN_INTERNAL, PTY_ERROR_CLOSED,
                      EPIPE, "PTY session is closed");
        return 0;
    }
    stop_process(platform);
    wake_reactor(platform);
    return 1;
}

FFI_PLUGIN_EXPORT int32_t pty_session_send_signal(PtySession *session,
                                                  int32_t signal_number,
                                                  int32_t target,
                                                  PtyError *out_error)
{
    pty_error_clear(out_error);
    if (signal_number <= 0 || signal_number > 64 || target < 0 || target > 2) {
        pty_error_set(out_error, PTY_ERROR_DOMAIN_INTERNAL,
                      PTY_ERROR_INVALID_ARGUMENT, EINVAL,
                      "invalid POSIX signal target");
        return 0;
    }
    PtyUnixPlatform *platform = platform_for(session);
    if (platform == NULL) {
        pty_error_set(out_error, PTY_ERROR_DOMAIN_INTERNAL, PTY_ERROR_CLOSED,
                      EPIPE, "PTY session is closed");
        return 0;
    }
    pid_t target_pid = platform->process_id;
    if (target == 1) target_pid = -platform->process_id;
    if (target == 2) {
        const pid_t foreground = tcgetpgrp(platform->master_fd);
        if (foreground <= 0) {
            pty_error_set_errno(out_error, PTY_ERROR_IO, errno,
                                "finding foreground process group failed");
            return 0;
        }
        target_pid = -foreground;
    }
    if (kill(target_pid, signal_number) != 0 && errno != ESRCH) {
        pty_error_set_errno(out_error, PTY_ERROR_IO, errno,
                            "sending POSIX signal failed");
        return 0;
    }
    return 1;
}

FFI_PLUGIN_EXPORT void pty_session_discard_output(PtySession *session)
{
    PtyUnixPlatform *platform = platform_for(session);
    if (platform == NULL) return;
    pthread_mutex_lock(&platform->mutex);
    platform->discard_output = 1;
    pthread_mutex_unlock(&platform->mutex);
    wake_reactor(platform);
}

FFI_PLUGIN_EXPORT void pty_session_begin_close(PtySession *session)
{
    if (session == NULL) return;
    pty_session_mark_closing(session);
    PtyUnixPlatform *platform = platform_for(session);
    if (platform == NULL) return;
    pthread_mutex_lock(&platform->mutex);
    platform->stopping = 1;
    pthread_mutex_unlock(&platform->mutex);
    stop_process(platform);
    wake_reactor(platform);
    maybe_post_session_closed(session);
}

FFI_PLUGIN_EXPORT void pty_session_abandon(void *opaque_session)
{
    PtySession *session = opaque_session;
    if (session == NULL) return;
    pty_session_mark_abandoned(session);
    pty_session_begin_close(session);
    pty_session_release(session);
}
