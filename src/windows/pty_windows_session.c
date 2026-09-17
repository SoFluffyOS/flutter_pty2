#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <windows.h>

#include "../common/pty_error.h"
#include "../common/pty_event.h"
#include "../pty_internal.h"

#define PTY_WINDOWS_IO_BUFFER_SIZE (64 * 1024)

/*
 * The platform mutex guards stop/close state, worker completion flags,
 * backpressure flags, input/output closure flags, and the closed-event guard.
 * The process, ConPTY, pipe, job, and process-id handles are initialized by
 * bootstrap before publication and remain stable until final teardown. Worker
 * handles and started flags are bootstrap-owned and are read during teardown
 * only after the corresponding worker has stopped. The write queue owns its
 * own mutex for chunk links and byte counts; whenever both mutexes are needed,
 * the platform mutex is acquired first. session->output_credit is protected
 * by this platform mutex. No platform mutex is held while a Dart event is
 * posted.
 */
typedef struct PtyWindowsPlatform {
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE condition;
    HANDLE input_write;
    HANDLE output_read;
    HPCON pseudo_console;
    HANDLE process;
    HANDLE job;
    DWORD process_id;
    HANDLE reader_thread;
    HANDLE writer_thread;
    HANDLE waiter_thread;
    HANDLE close_thread;
    int reader_started;
    int writer_started;
    int waiter_started;
    int reader_done;
    int writer_done;
    int waiter_done;
    int close_started;
    int close_done;
    int stopping;
    int write_backpressured;
    int discard_output;
    int output_closed;
    int input_closed;
    int session_closed_posted;
    PtyWriteQueue write_queue;
} PtyWindowsPlatform;

typedef struct PtyWindowsBootstrap {
    PtySession *session;
    PtyWindowsOwnedOptions options;
} PtyWindowsBootstrap;

static PtyWindowsPlatform *windows_platform(PtySession *session)
{
    return (PtyWindowsPlatform *)pty_session_platform_load(session);
}

static int handle_posted_session_event(PtySession *session, int posted)
{
    if (!posted) pty_session_abandon(session);
    return posted;
}

#define post_session_event(session, event) \
    (pty_session_is_abandoned(session) \
         ? 0 \
         : handle_posted_session_event((session), (event)))

static void windows_post_error(PtySession *session,
                               PtyErrorKind kind,
                               DWORD error_code,
                               const char *operation)
{
    PtyError error;
    pty_error_set(&error,
                  PTY_ERROR_DOMAIN_WIN32,
                  kind,
                  error_code,
                  operation);
    post_session_event(session, pty_post_error(session->event_port, &error));
}

static void windows_mark_output_closed(PtySession *session)
{
    PtyWindowsPlatform *platform = windows_platform(session);
    int should_post = 0;
    EnterCriticalSection(&platform->mutex);
    if (!platform->output_closed) {
        platform->output_closed = 1;
        should_post = 1;
    }
    LeaveCriticalSection(&platform->mutex);
    if (should_post) {
        InterlockedExchange(&session->output_closed, 1);
        post_session_event(
            session,
            pty_post_simple_event(session->event_port, PTY_EVENT_OUTPUT_CLOSED));
    }
}

static void windows_mark_input_closed(PtySession *session,
                                      const PtyError *error)
{
    PtyWindowsPlatform *platform = windows_platform(session);
    int should_post = 0;
    EnterCriticalSection(&platform->mutex);
    if (!platform->input_closed) {
        platform->input_closed = 1;
        should_post = 1;
    }
    LeaveCriticalSection(&platform->mutex);
    if (!should_post) return;
    InterlockedExchange(&session->input_closed, 1);
    if (error == NULL) {
        PtyError closed_error;
        pty_error_set(&closed_error,
                      PTY_ERROR_DOMAIN_WIN32,
                      PTY_ERROR_CLOSED,
                      ERROR_BROKEN_PIPE,
                      "PTY input closed");
        post_session_event(
            session,
            pty_post_input_closed(session->event_port, &closed_error));
        return;
    }
    post_session_event(session,
                       pty_post_input_closed(session->event_port, error));
}

static void windows_discard_pending_writes(PtyWindowsPlatform *platform)
{
    if (platform == NULL) return;
    EnterCriticalSection(&platform->mutex);
    while (true) {
        PtyWriteChunk *chunk = pty_write_queue_dequeue(&platform->write_queue);
        if (chunk == NULL) break;
        pty_write_chunk_free(chunk);
    }
    platform->write_backpressured = 0;
    LeaveCriticalSection(&platform->mutex);
}

static void windows_maybe_post_closed(PtySession *session)
{
    PtyWindowsPlatform *platform = windows_platform(session);
    int should_post = 0;
    EnterCriticalSection(&platform->mutex);
    if (platform->stopping && platform->reader_done && platform->writer_done &&
        platform->waiter_done && platform->close_done &&
        !platform->session_closed_posted) {
        platform->session_closed_posted = 1;
        should_post = 1;
    }
    LeaveCriticalSection(&platform->mutex);
    if (!should_post) return;
    pty_session_mark_closed(session);
    post_session_event(
        session,
        pty_post_simple_event(session->event_port, PTY_EVENT_SESSION_CLOSED));
}

static DWORD WINAPI windows_reader(void *argument)
{
    PtySession *session = argument;
    PtyWindowsPlatform *platform = windows_platform(session);
    uint8_t buffer[PTY_WINDOWS_IO_BUFFER_SIZE];
    while (true) {
        EnterCriticalSection(&platform->mutex);
        while (session->output_credit == 0 && !platform->discard_output &&
               !platform->stopping) {
            SleepConditionVariableCS(&platform->condition,
                                     &platform->mutex,
                                     INFINITE);
        }
        const int stopping = platform->stopping;
        const int discard = platform->discard_output;
        const DWORD capacity = discard
                                   ? sizeof(buffer)
                                   : (DWORD)(session->output_credit <
                                                 sizeof(buffer)
                                             ? session->output_credit
                                             : sizeof(buffer));
        LeaveCriticalSection(&platform->mutex);
        if (stopping) break;

        DWORD length = 0;
        const BOOL read_succeeded = ReadFile(platform->output_read,
                                             buffer,
                                             capacity,
                                             &length,
                                             NULL);
        if (!read_succeeded || length == 0) {
            const DWORD read_error = read_succeeded ? ERROR_NO_DATA :
                                                       GetLastError();
            EnterCriticalSection(&platform->mutex);
            const int closing = platform->stopping;
            LeaveCriticalSection(&platform->mutex);
            const int process_exited =
                InterlockedCompareExchange(&session->process_exited,
                                           0,
                                           0) != 0 ||
                WaitForSingleObject(platform->process, 0) == WAIT_OBJECT_0;
            if (!closing && !process_exited) {
                windows_post_error(session,
                                   PTY_ERROR_IO,
                                   read_error,
                                   "reading ConPTY output failed");
            }
            break;
        }

        EnterCriticalSection(&platform->mutex);
        const int discard_after_read = platform->discard_output;
        if (!discard_after_read) {
            if (session->output_credit >= length) {
                session->output_credit -= length;
            } else {
                session->output_credit = 0;
            }
        }
        LeaveCriticalSection(&platform->mutex);
        if (discard_after_read) continue;
        if (!post_session_event(
                session,
                pty_post_output(session->event_port, buffer, length))) {
            break;
        }
    }
    windows_mark_output_closed(session);
    EnterCriticalSection(&platform->mutex);
    platform->reader_done = 1;
    LeaveCriticalSection(&platform->mutex);
    pty_debug_worker_finished(PTY_DEBUG_WORKER_READ);
    windows_maybe_post_closed(session);
    pty_session_release(session);
    return 0;
}

static DWORD WINAPI windows_writer(void *argument)
{
    PtySession *session = argument;
    PtyWindowsPlatform *platform = windows_platform(session);
    while (true) {
        EnterCriticalSection(&platform->mutex);
        while (!platform->stopping &&
               pty_write_queue_pending_bytes(&platform->write_queue) == 0) {
            SleepConditionVariableCS(&platform->condition,
                                     &platform->mutex,
                                     INFINITE);
        }
        const int stopping = platform->stopping;
        LeaveCriticalSection(&platform->mutex);
        if (stopping) break;

        PtyWriteChunk *chunk =
            pty_write_queue_dequeue(&platform->write_queue);
        if (chunk == NULL) continue;
        uint64_t offset = 0;
        int succeeded = 1;
        DWORD failure_error = ERROR_WRITE_FAULT;
        while (offset < chunk->length) {
            DWORD written = 0;
            const DWORD requested = (DWORD)(chunk->length - offset);
            const BOOL write_succeeded = WriteFile(platform->input_write,
                                                   chunk->bytes + offset,
                                                   requested,
                                                   &written,
                                                   NULL);
            if (!write_succeeded) {
                failure_error = GetLastError();
                succeeded = 0;
                break;
            }
            if (written == 0) {
                succeeded = 0;
                break;
            }
            offset += written;
        }
        if (succeeded) {
            post_session_event(
                session,
                pty_post_write_complete(session->event_port, chunk->request_id));
            int should_post_writable = 0;
            EnterCriticalSection(&platform->mutex);
            if (platform->write_backpressured &&
                pty_write_queue_pending_bytes(&platform->write_queue) <=
                    session->input_buffer_limit / 2) {
                platform->write_backpressured = 0;
                should_post_writable = 1;
            }
            LeaveCriticalSection(&platform->mutex);
            if (should_post_writable) {
                post_session_event(
                    session,
                    pty_post_simple_event(session->event_port,
                                          PTY_EVENT_WRITABLE));
            }
        } else {
            PtyError error;
            pty_error_set(&error,
                          PTY_ERROR_DOMAIN_WIN32,
                          PTY_ERROR_IO,
                          failure_error,
                          "writing ConPTY input failed");
            windows_mark_input_closed(session, &error);
            windows_discard_pending_writes(platform);
        }
        pty_write_chunk_free(chunk);
        if (!succeeded) break;
    }
    windows_mark_input_closed(session, NULL);
    EnterCriticalSection(&platform->mutex);
    platform->writer_done = 1;
    LeaveCriticalSection(&platform->mutex);
    pty_debug_worker_finished(PTY_DEBUG_WORKER_WRITE);
    windows_maybe_post_closed(session);
    pty_session_release(session);
    return 0;
}

static DWORD WINAPI windows_waiter(void *argument)
{
    PtySession *session = argument;
    PtyWindowsPlatform *platform = windows_platform(session);
    const DWORD wait_result = WaitForSingleObject(platform->process, INFINITE);
    if (wait_result == WAIT_OBJECT_0) {
        DWORD exit_code = 1;
        if (GetExitCodeProcess(platform->process, &exit_code)) {
            post_session_event(
                session,
                pty_post_process_exit(session->event_port, false, exit_code));
        } else {
            windows_post_error(session,
                               PTY_ERROR_IO,
                               GetLastError(),
                               "reading ConPTY process exit failed");
        }
    } else if (wait_result == WAIT_FAILED) {
        windows_post_error(session,
                           PTY_ERROR_IO,
                           GetLastError(),
                           "waiting for ConPTY process failed");
    } else {
        windows_post_error(session,
                           PTY_ERROR_IO,
                           ERROR_INVALID_DATA,
                           "waiting for ConPTY process returned an invalid status");
    }
    InterlockedExchange(&session->process_exited, 1);
    EnterCriticalSection(&platform->mutex);
    platform->waiter_done = 1;
    LeaveCriticalSection(&platform->mutex);
    pty_debug_worker_finished(PTY_DEBUG_WORKER_WAIT);
    windows_maybe_post_closed(session);
    pty_session_release(session);
    return 0;
}

static void windows_stop_process(PtyWindowsPlatform *platform)
{
    if (platform == NULL) return;
    if (platform->job != NULL) TerminateJobObject(platform->job, 1);
    EnterCriticalSection(&platform->mutex);
    platform->stopping = 1;
    WakeAllConditionVariable(&platform->condition);
    LeaveCriticalSection(&platform->mutex);
    if (platform->reader_started) CancelSynchronousIo(platform->reader_thread);
    if (platform->writer_started) CancelSynchronousIo(platform->writer_thread);
}

static void windows_post_startup_cancelled(PtySession *session)
{
    PtyError error;
    pty_error_set(&error,
                  PTY_ERROR_DOMAIN_INTERNAL,
                  PTY_ERROR_CLOSED,
                  ERROR_OPERATION_ABORTED,
                  "PTY session closed during startup");
    post_session_event(session,
                       pty_post_spawn_failed(session->event_port, &error));
}

static void windows_finish_unstarted_close(PtySession *session,
                                           HANDLE process_thread)
{
    PtyWindowsPlatform *platform = windows_platform(session);
    if (platform == NULL) return;

    windows_stop_process(platform);
    WaitForSingleObject(platform->process, INFINITE);
    if (process_thread != NULL) CloseHandle(process_thread);

    EnterCriticalSection(&platform->mutex);
    platform->reader_done = 1;
    platform->writer_done = 1;
    platform->waiter_done = 1;
    platform->close_done = 1;
    platform->session_closed_posted = 1;
    LeaveCriticalSection(&platform->mutex);

    windows_post_startup_cancelled(session);
    windows_mark_output_closed(session);
    windows_mark_input_closed(session, NULL);
    pty_session_mark_closed(session);
    post_session_event(
        session,
        pty_post_simple_event(session->event_port, PTY_EVENT_SESSION_CLOSED));
}

static void windows_finish_worker_startup_failure(PtySession *session,
                                                  HANDLE process_thread,
                                                  const PtyError *error)
{
    PtyWindowsPlatform *platform = windows_platform(session);
    if (platform == NULL) return;

    windows_stop_process(platform);
    if (process_thread != NULL) {
        ResumeThread(process_thread);
        CloseHandle(process_thread);
    }
    if (platform->reader_started) {
        WaitForSingleObject(platform->reader_thread, INFINITE);
    }
    if (platform->writer_started) {
        WaitForSingleObject(platform->writer_thread, INFINITE);
    }
    if (platform->waiter_started) {
        WaitForSingleObject(platform->waiter_thread, INFINITE);
    }
    EnterCriticalSection(&platform->mutex);
    if (!platform->reader_started) platform->reader_done = 1;
    if (!platform->writer_started) platform->writer_done = 1;
    if (!platform->waiter_started) platform->waiter_done = 1;
    platform->close_done = 1;
    LeaveCriticalSection(&platform->mutex);

    pty_session_mark_closing(session);
    post_session_event(session,
                       pty_post_spawn_failed(session->event_port, error));
    windows_mark_output_closed(session);
    windows_mark_input_closed(session, NULL);
    windows_maybe_post_closed(session);
}

static DWORD WINAPI windows_close_worker(void *argument)
{
    PtySession *session = argument;
    PtyWindowsPlatform *platform = windows_platform(session);
    windows_stop_process(platform);
    EnterCriticalSection(&platform->mutex);
    platform->close_done = 1;
    LeaveCriticalSection(&platform->mutex);
    pty_debug_worker_finished(PTY_DEBUG_WORKER_CLOSE);
    windows_maybe_post_closed(session);
    pty_session_release(session);
    return 0;
}

static void windows_free_session(PtySession *session)
{
    PtyWindowsPlatform *platform = windows_platform(session);
    if (platform != NULL) {
        if (platform->reader_thread != NULL) CloseHandle(platform->reader_thread);
        if (platform->writer_thread != NULL) CloseHandle(platform->writer_thread);
        if (platform->waiter_thread != NULL) CloseHandle(platform->waiter_thread);
        if (platform->close_thread != NULL) CloseHandle(platform->close_thread);
        if (platform->input_write != NULL) CloseHandle(platform->input_write);
        if (platform->output_read != NULL) CloseHandle(platform->output_read);
        if (platform->process != NULL) CloseHandle(platform->process);
        if (platform->job != NULL) CloseHandle(platform->job);
        if (platform->pseudo_console != NULL) {
            ClosePseudoConsole(platform->pseudo_console);
        }
        pty_write_queue_dispose(&platform->write_queue);
        DeleteCriticalSection(&platform->mutex);
        free(platform);
    }
    free(session);
}

static DWORD WINAPI windows_bootstrap(void *argument)
{
    PtyWindowsBootstrap *bootstrap = argument;
    PtySession *session = bootstrap->session;
    HANDLE input_write = NULL;
    HANDLE output_read = NULL;
    HANDLE process = NULL;
    HANDLE process_thread = NULL;
    HANDLE job = NULL;
    HPCON pseudo_console = NULL;
    DWORD process_id = 0;
    DWORD worker_error = ERROR_NOT_ENOUGH_MEMORY;
    int worker_start_failed = 0;
    PtyError error;
    if (!windows_create_process(&bootstrap->options.options,
                                &input_write,
                                &output_read,
                                &process,
                                &process_thread,
                                &job,
                                &process_id,
                                &pseudo_console,
                                &error)) {
        post_session_event(
            session,
            pty_post_spawn_failed(session->event_port, &error));
        post_session_event(
            session,
            pty_post_simple_event(session->event_port, PTY_EVENT_OUTPUT_CLOSED));
        pty_session_mark_closing(session);
        pty_session_mark_closed(session);
        post_session_event(
            session,
            pty_post_simple_event(session->event_port, PTY_EVENT_SESSION_CLOSED));
        windows_free_options(&bootstrap->options);
        free(bootstrap);
        pty_session_release(session);
        return 0;
    }

    PtyWindowsPlatform *platform = calloc(1, sizeof(*platform));
    if (platform == NULL) {
        pty_error_set(&error,
                      PTY_ERROR_DOMAIN_INTERNAL,
                      PTY_ERROR_OUT_OF_MEMORY,
                      ERROR_NOT_ENOUGH_MEMORY,
                      "allocating Windows PTY session failed");
        TerminateJobObject(job, 1);
        CloseHandle(process);
        CloseHandle(process_thread);
        CloseHandle(job);
        CloseHandle(input_write);
        CloseHandle(output_read);
        ClosePseudoConsole(pseudo_console);
        post_session_event(
            session,
            pty_post_spawn_failed(session->event_port, &error));
        post_session_event(
            session,
            pty_post_simple_event(session->event_port, PTY_EVENT_OUTPUT_CLOSED));
        pty_session_mark_closing(session);
        pty_session_mark_closed(session);
        post_session_event(
            session,
            pty_post_simple_event(session->event_port, PTY_EVENT_SESSION_CLOSED));
        windows_free_options(&bootstrap->options);
        free(bootstrap);
        pty_session_release(session);
        return 0;
    }
    InitializeCriticalSection(&platform->mutex);
    InitializeConditionVariable(&platform->condition);
    platform->input_write = input_write;
    platform->output_read = output_read;
    platform->pseudo_console = pseudo_console;
    platform->process = process;
    platform->job = job;
    platform->process_id = process_id;
    pty_write_queue_init(&platform->write_queue, session->input_buffer_limit);
    pty_session_platform_store(session, platform);

    const int close_requested =
        InterlockedCompareExchange(&session->lifecycle,
                                   PTY_LIFECYCLE_STARTING,
                                   PTY_LIFECYCLE_STARTING) ==
        PTY_LIFECYCLE_CLOSING;
    if (close_requested) {
        windows_finish_unstarted_close(session, process_thread);
        windows_free_options(&bootstrap->options);
        free(bootstrap);
        pty_session_release(session);
        return 0;
    }

    pty_debug_worker_started(PTY_DEBUG_WORKER_READ);
    pty_session_retain(session);
    platform->reader_thread = CreateThread(NULL, 0, windows_reader, session, 0, NULL);
    if (platform->reader_thread != NULL) platform->reader_started = 1;
    else {
        worker_error = GetLastError();
        worker_start_failed = 1;
        pty_debug_worker_finished(PTY_DEBUG_WORKER_READ);
        pty_session_release(session);
    }
    pty_debug_worker_started(PTY_DEBUG_WORKER_WRITE);
    pty_session_retain(session);
    platform->writer_thread = CreateThread(NULL, 0, windows_writer, session, 0, NULL);
    if (platform->writer_thread != NULL) platform->writer_started = 1;
    else {
        if (!worker_start_failed) worker_error = GetLastError();
        worker_start_failed = 1;
        pty_debug_worker_finished(PTY_DEBUG_WORKER_WRITE);
        pty_session_release(session);
    }
    pty_debug_worker_started(PTY_DEBUG_WORKER_WAIT);
    pty_session_retain(session);
    platform->waiter_thread = CreateThread(NULL, 0, windows_waiter, session, 0, NULL);
    if (platform->waiter_thread != NULL) platform->waiter_started = 1;
    else {
        if (!worker_start_failed) worker_error = GetLastError();
        worker_start_failed = 1;
        pty_debug_worker_finished(PTY_DEBUG_WORKER_WAIT);
        pty_session_release(session);
    }
    if (!platform->reader_started || !platform->writer_started ||
        !platform->waiter_started) {
        pty_error_set(&error,
                      PTY_ERROR_DOMAIN_INTERNAL,
                      PTY_ERROR_INTERNAL,
                      worker_error,
                      "starting Windows PTY workers failed");
        windows_finish_worker_startup_failure(session, process_thread, &error);
        windows_free_options(&bootstrap->options);
        free(bootstrap);
        pty_session_release(session);
        return 0;
    }

    if (InterlockedCompareExchange(&session->lifecycle,
                                   PTY_LIFECYCLE_RUNNING,
                                   PTY_LIFECYCLE_STARTING) !=
        PTY_LIFECYCLE_STARTING) {
        windows_stop_process(platform);
        ResumeThread(process_thread);
        CloseHandle(process_thread);
        if (platform->reader_started) WaitForSingleObject(platform->reader_thread, INFINITE);
        if (platform->writer_started) WaitForSingleObject(platform->writer_thread, INFINITE);
        if (platform->waiter_started) WaitForSingleObject(platform->waiter_thread, INFINITE);
        EnterCriticalSection(&platform->mutex);
        platform->close_done = 1;
        LeaveCriticalSection(&platform->mutex);
        windows_post_startup_cancelled(session);
        windows_maybe_post_closed(session);
        windows_free_options(&bootstrap->options);
        free(bootstrap);
        pty_session_release(session);
        return 0;
    }
    if (ResumeThread(process_thread) == (DWORD)-1) {
        pty_error_set(&error,
                      PTY_ERROR_DOMAIN_WIN32,
                      PTY_ERROR_SPAWN_FAILED,
                      GetLastError(),
                      "resuming ConPTY process failed");
        windows_finish_worker_startup_failure(session, process_thread, &error);
        windows_free_options(&bootstrap->options);
        free(bootstrap);
        pty_session_release(session);
        return 0;
    }
    CloseHandle(process_thread);
    post_session_event(
        session,
        pty_post_spawned(
            session->event_port,
            process_id,
            PTY_CAPABILITY_RELIABLE_PROCESS_TREE_KILL |
                PTY_CAPABILITY_CONPTY));
    windows_free_options(&bootstrap->options);
    free(bootstrap);
    pty_session_release(session);
    return 0;
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
        options->output_window_bytes == 0 || !pty_size_is_valid(options->size)) {
        pty_error_set(out_error,
                      PTY_ERROR_DOMAIN_INTERNAL,
                      PTY_ERROR_INVALID_ARGUMENT,
                      ERROR_INVALID_PARAMETER,
                      "invalid Windows PTY session options");
        return 0;
    }
    PtySession *session = calloc(1, sizeof(*session));
    if (session == NULL) {
        pty_error_set(out_error,
                      PTY_ERROR_DOMAIN_INTERNAL,
                      PTY_ERROR_OUT_OF_MEMORY,
                      ERROR_NOT_ENOUGH_MEMORY,
                      "allocating PTY session failed");
        return 0;
    }
    pty_session_init(session);
    session->event_port = (Dart_Port_DL)options->event_port;
    session->input_buffer_limit = options->input_buffer_bytes;
    session->output_window_limit = options->output_window_bytes;
    session->output_credit = options->output_window_bytes;
    session->free_function = windows_free_session;
    PtyWindowsBootstrap *bootstrap = calloc(1, sizeof(*bootstrap));
    if (bootstrap == NULL ||
        !windows_clone_options(options, &bootstrap->options, out_error)) {
        if (bootstrap != NULL) windows_free_options(&bootstrap->options);
        free(bootstrap);
        pty_session_release(session);
        return 0;
    }
    bootstrap->session = session;
    pty_session_retain(session);
    HANDLE thread = CreateThread(NULL, 0, windows_bootstrap, bootstrap, 0, NULL);
    if (thread == NULL) {
        const DWORD thread_error = GetLastError();
        pty_session_release(session);
        windows_free_options(&bootstrap->options);
        free(bootstrap);
        pty_session_release(session);
        pty_error_set(out_error,
                      PTY_ERROR_DOMAIN_WIN32,
                      PTY_ERROR_INTERNAL,
                      thread_error,
                      "starting PTY bootstrap failed");
        return 0;
    }
    CloseHandle(thread);
    *out_session = session;
    return 1;
}

FFI_PLUGIN_EXPORT int64_t pty_session_pid(PtySession *session)
{
    PtyWindowsPlatform *platform = windows_platform(session);
    return platform == NULL ? -1 : (int64_t)platform->process_id;
}

FFI_PLUGIN_EXPORT int32_t pty_session_try_write(PtySession *session,
                                                uint64_t request_id,
                                                const uint8_t *bytes,
                                                uint64_t length,
                                                PtyError *out_error)
{
    pty_error_clear(out_error);
    if (session == NULL || bytes == NULL || length == 0) {
        pty_error_set(out_error,
                      PTY_ERROR_DOMAIN_INTERNAL,
                      PTY_ERROR_INVALID_ARGUMENT,
                      ERROR_INVALID_PARAMETER,
                      "invalid PTY input write");
        return PTY_WRITE_ERROR;
    }
    PtyWindowsPlatform *platform = windows_platform(session);
    if (platform == NULL) return PTY_WRITE_CLOSED;
    EnterCriticalSection(&platform->mutex);
    const LONG lifecycle = InterlockedCompareExchange(&session->lifecycle,
                                                      PTY_LIFECYCLE_CLOSING,
                                                      PTY_LIFECYCLE_CLOSING);
    if (platform->stopping || platform->input_closed ||
        lifecycle >= PTY_LIFECYCLE_CLOSING) {
        LeaveCriticalSection(&platform->mutex);
        return PTY_WRITE_CLOSED;
    }
    const int result = pty_write_queue_try_enqueue(&platform->write_queue,
                                                   bytes,
                                                   length,
                                                   request_id);
    if (result == PTY_WRITE_BACKPRESSURED) {
        platform->write_backpressured = 1;
    }
    LeaveCriticalSection(&platform->mutex);
    if (result == PTY_WRITE_ERROR) {
        pty_error_set(out_error,
                      PTY_ERROR_DOMAIN_INTERNAL,
                      PTY_ERROR_OUT_OF_MEMORY,
                      ERROR_NOT_ENOUGH_MEMORY,
                      "queueing PTY input failed");
        return PTY_WRITE_ERROR;
    }
    if (result == PTY_WRITE_ACCEPTED) {
        EnterCriticalSection(&platform->mutex);
        WakeAllConditionVariable(&platform->condition);
        LeaveCriticalSection(&platform->mutex);
    }
    return result;
}

FFI_PLUGIN_EXPORT void pty_session_ack_output(PtySession *session,
                                              uint64_t byte_count)
{
    PtyWindowsPlatform *platform = windows_platform(session);
    if (platform == NULL) return;
    EnterCriticalSection(&platform->mutex);
    const uint64_t limit = session->output_window_limit;
    if (byte_count > limit - session->output_credit) {
        session->output_credit = limit;
    } else {
        session->output_credit += byte_count;
    }
    WakeAllConditionVariable(&platform->condition);
    LeaveCriticalSection(&platform->mutex);
}

FFI_PLUGIN_EXPORT int32_t pty_session_resize(PtySession *session,
                                              PtySize size,
                                              PtyError *out_error)
{
    pty_error_clear(out_error);
    PtyWindowsPlatform *platform = windows_platform(session);
    if (!pty_size_is_valid(size)) {
        pty_error_set(out_error,
                      PTY_ERROR_DOMAIN_INTERNAL,
                      PTY_ERROR_INVALID_ARGUMENT,
                      ERROR_INVALID_PARAMETER,
                      "invalid Windows PTY size");
        return 0;
    }
    if (platform == NULL || platform->pseudo_console == NULL) {
        pty_error_set(out_error,
                      PTY_ERROR_DOMAIN_INTERNAL,
                      PTY_ERROR_CLOSED,
                      ERROR_INVALID_HANDLE,
                      "PTY session is closed");
        return 0;
    }
    const COORD console_size = {
        .X = (SHORT)size.columns,
        .Y = (SHORT)size.rows,
    };
    const HRESULT resize_result =
        ResizePseudoConsole(platform->pseudo_console, console_size);
    if (FAILED(resize_result)) {
        pty_error_set(out_error,
                      PTY_ERROR_DOMAIN_HRESULT,
                      PTY_ERROR_IO,
                      resize_result,
                      "resizing ConPTY failed");
        return 0;
    }
    return 1;
}

FFI_PLUGIN_EXPORT int32_t pty_session_kill(PtySession *session,
                                           PtyError *out_error)
{
    pty_error_clear(out_error);
    PtyWindowsPlatform *platform = windows_platform(session);
    if (platform == NULL || platform->job == NULL) {
        pty_error_set(out_error,
                      PTY_ERROR_DOMAIN_INTERNAL,
                      PTY_ERROR_CLOSED,
                      ERROR_INVALID_HANDLE,
                      "PTY session is closed");
        return 0;
    }
    if (InterlockedCompareExchange(&session->process_exited, 0, 0) != 0) {
        return 1;
    }
    if (!TerminateJobObject(platform->job, 1)) {
        const DWORD error_code = GetLastError();
        if (WaitForSingleObject(platform->process, 0) == WAIT_OBJECT_0) {
            return 1;
        }
        pty_error_set(out_error,
                      PTY_ERROR_DOMAIN_WIN32,
                      PTY_ERROR_IO,
                      error_code,
                      "terminating PTY Job Object failed");
        return 0;
    }
    return 1;
}

FFI_PLUGIN_EXPORT int32_t pty_session_send_signal(PtySession *session,
                                                  int32_t signal_number,
                                                  int32_t target,
                                                  PtyError *out_error)
{
    (void)session;
    (void)signal_number;
    (void)target;
    pty_error_set(out_error,
                  PTY_ERROR_DOMAIN_INTERNAL,
                  PTY_ERROR_UNSUPPORTED,
                  ERROR_CALL_NOT_IMPLEMENTED,
                  "POSIX signals are unsupported on Windows");
    return 0;
}

FFI_PLUGIN_EXPORT void pty_session_discard_output(PtySession *session)
{
    PtyWindowsPlatform *platform = windows_platform(session);
    if (platform == NULL) return;
    EnterCriticalSection(&platform->mutex);
    platform->discard_output = 1;
    session->output_credit = session->output_window_limit;
    WakeAllConditionVariable(&platform->condition);
    LeaveCriticalSection(&platform->mutex);
}

FFI_PLUGIN_EXPORT void pty_session_begin_close(PtySession *session)
{
    if (session == NULL) return;
    while (true) {
        const LONG lifecycle = InterlockedCompareExchange(&session->lifecycle,
                                                          0,
                                                          0);
        if (lifecycle == PTY_LIFECYCLE_STARTING) {
            if (InterlockedCompareExchange(&session->lifecycle,
                                           PTY_LIFECYCLE_CLOSING,
                                           PTY_LIFECYCLE_STARTING) ==
                PTY_LIFECYCLE_STARTING) {
                return;
            }
            continue;
        }
        if (lifecycle != PTY_LIFECYCLE_RUNNING &&
            lifecycle != PTY_LIFECYCLE_CLOSING) {
            return;
        }
        if (lifecycle == PTY_LIFECYCLE_CLOSING) break;
        if (InterlockedCompareExchange(&session->lifecycle,
                                       PTY_LIFECYCLE_CLOSING,
                                       PTY_LIFECYCLE_RUNNING) ==
            PTY_LIFECYCLE_RUNNING) {
            break;
        }
    }
    PtyWindowsPlatform *platform = windows_platform(session);
    if (platform == NULL) return;
    EnterCriticalSection(&platform->mutex);
    if (platform->close_started || platform->close_done) {
        LeaveCriticalSection(&platform->mutex);
        return;
    }
    platform->close_started = 1;
    LeaveCriticalSection(&platform->mutex);
    pty_debug_worker_started(PTY_DEBUG_WORKER_CLOSE);
    pty_session_retain(session);
    platform->close_thread = CreateThread(NULL,
                                          0,
                                          windows_close_worker,
                                          session,
                                          0,
                                          NULL);
    if (platform->close_thread != NULL) return;

    pty_debug_worker_finished(PTY_DEBUG_WORKER_CLOSE);
    pty_session_release(session);
    EnterCriticalSection(&platform->mutex);
    platform->stopping = 1;
    platform->close_done = 1;
    LeaveCriticalSection(&platform->mutex);
    windows_stop_process(platform);
    windows_maybe_post_closed(session);
}

FFI_PLUGIN_EXPORT void pty_session_abandon(void *opaque_session)
{
    PtySession *session = opaque_session;
    if (session == NULL || !pty_session_mark_abandoned(session)) return;
    pty_session_begin_close(session);
    pty_session_release(session);
}
