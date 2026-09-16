#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../common/pty_error.h"
#include "../common/pty_event.h"
#include "../pty_internal.h"
#include "pty_windows_commandline.h"
#include "pty_windows_environment.h"

#define PTY_WINDOWS_IO_BUFFER_SIZE (64 * 1024)

typedef struct PtyWindowsOwnedOptions {
    PtySpawnOptions options;
    char *executable;
    char **arguments;
    char **environment;
    char *working_directory;
} PtyWindowsOwnedOptions;

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
    return session == NULL ? NULL : (PtyWindowsPlatform *)session->platform;
}

static char *windows_copy_string(const char *value)
{
    if (value == NULL) return NULL;
    const size_t length = strlen(value) + 1;
    char *copy = malloc(length);
    if (copy == NULL) return NULL;
    memcpy(copy, value, length);
    return copy;
}

static void windows_free_vector(char **values, int32_t count)
{
    if (values == NULL) return;
    for (int32_t index = 0; index < count; index++) free(values[index]);
    free(values);
}

static char **windows_copy_vector(const char *const *values,
                                  int32_t count,
                                  PtyError *error)
{
    if (count < 0) {
        pty_error_set(error,
                      PTY_ERROR_DOMAIN_INTERNAL,
                      PTY_ERROR_INVALID_ARGUMENT,
                      ERROR_INVALID_PARAMETER,
                      "invalid Windows string vector length");
        return NULL;
    }
    char **result = calloc((size_t)count + 1, sizeof(*result));
    if (result == NULL) {
        pty_error_set(error,
                      PTY_ERROR_DOMAIN_INTERNAL,
                      PTY_ERROR_OUT_OF_MEMORY,
                      ERROR_NOT_ENOUGH_MEMORY,
                      "allocating Windows string vector failed");
        return NULL;
    }
    for (int32_t index = 0; index < count; index++) {
        if (values == NULL || values[index] == NULL) {
            windows_free_vector(result, index);
            pty_error_set(error,
                          PTY_ERROR_DOMAIN_INTERNAL,
                          PTY_ERROR_INVALID_ARGUMENT,
                          ERROR_INVALID_PARAMETER,
                          "Windows string vector contains a null entry");
            return NULL;
        }
        result[index] = windows_copy_string(values[index]);
        if (result[index] == NULL) {
            windows_free_vector(result, index);
            pty_error_set(error,
                          PTY_ERROR_DOMAIN_INTERNAL,
                          PTY_ERROR_OUT_OF_MEMORY,
                          ERROR_NOT_ENOUGH_MEMORY,
                          "copying Windows spawn string failed");
            return NULL;
        }
    }
    return result;
}

static int windows_clone_options(const PtySpawnOptions *source,
                                 PtyWindowsOwnedOptions *destination,
                                 PtyError *error)
{
    memset(destination, 0, sizeof(*destination));
    destination->options = *source;
    destination->executable = windows_copy_string(source->executable);
    if (destination->executable == NULL) {
        pty_error_set(error,
                      PTY_ERROR_DOMAIN_INTERNAL,
                      PTY_ERROR_OUT_OF_MEMORY,
                      ERROR_NOT_ENOUGH_MEMORY,
                      "copying Windows executable failed");
        return 0;
    }
    destination->arguments = windows_copy_vector(source->arguments,
                                                 source->argument_count,
                                                 error);
    if (destination->arguments == NULL) return 0;
    destination->environment = windows_copy_vector(source->environment,
                                                   source->environment_count,
                                                   error);
    if (destination->environment == NULL) return 0;
    destination->working_directory =
        windows_copy_string(source->working_directory);
    if (source->working_directory != NULL &&
        destination->working_directory == NULL) {
        pty_error_set(error,
                      PTY_ERROR_DOMAIN_INTERNAL,
                      PTY_ERROR_OUT_OF_MEMORY,
                      ERROR_NOT_ENOUGH_MEMORY,
                      "copying Windows working directory failed");
        return 0;
    }
    destination->options.executable = destination->executable;
    destination->options.arguments =
        (const char *const *)destination->arguments;
    destination->options.environment =
        (const char *const *)destination->environment;
    destination->options.working_directory = destination->working_directory;
    return 1;
}

static void windows_free_options(PtyWindowsOwnedOptions *options)
{
    if (options == NULL) return;
    free(options->executable);
    windows_free_vector(options->arguments, options->options.argument_count);
    windows_free_vector(options->environment,
                        options->options.environment_count);
    free(options->working_directory);
    memset(options, 0, sizeof(*options));
}

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
    pty_post_error(session->event_port, &error);
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
        pty_post_simple_event(session->event_port, PTY_EVENT_OUTPUT_CLOSED);
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
        pty_post_input_closed(session->event_port, &closed_error);
        return;
    }
    pty_post_input_closed(session->event_port, error);
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
    pty_post_simple_event(session->event_port, PTY_EVENT_SESSION_CLOSED);
}

static DWORD WINAPI windows_reader(void *argument)
{
    PtySession *session = argument;
    PtyWindowsPlatform *platform = windows_platform(session);
    uint8_t buffer[PTY_WINDOWS_IO_BUFFER_SIZE];
    while (true) {
        EnterCriticalSection(&platform->mutex);
        while (session->output_credit == 0 && !platform->stopping) {
            SleepConditionVariableCS(&platform->condition,
                                     &platform->mutex,
                                     INFINITE);
        }
        const int stopping = platform->stopping;
        const DWORD capacity = (DWORD)(session->output_credit <
                                               sizeof(buffer)
                                           ? session->output_credit
                                           : sizeof(buffer));
        LeaveCriticalSection(&platform->mutex);
        if (stopping) break;

        DWORD length = 0;
        if (!ReadFile(platform->output_read,
                      buffer,
                      capacity,
                      &length,
                      NULL) ||
            length == 0) {
            EnterCriticalSection(&platform->mutex);
            const int closing = platform->stopping;
            LeaveCriticalSection(&platform->mutex);
            if (!closing) {
                windows_post_error(session,
                                   PTY_ERROR_IO,
                                   GetLastError(),
                                   "reading ConPTY output failed");
            }
            break;
        }

        EnterCriticalSection(&platform->mutex);
        if (session->output_credit >= length) {
            session->output_credit -= length;
        } else {
            session->output_credit = 0;
        }
        LeaveCriticalSection(&platform->mutex);
        if (!pty_post_output(session->event_port, buffer, length)) break;
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
        while (offset < chunk->length) {
            DWORD written = 0;
            const DWORD requested = (DWORD)(chunk->length - offset);
            if (!WriteFile(platform->input_write,
                           chunk->bytes + offset,
                           requested,
                           &written,
                           NULL) ||
                written == 0) {
                succeeded = 0;
                break;
            }
            offset += written;
        }
        if (succeeded) {
            pty_post_write_complete(session->event_port, chunk->request_id);
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
                pty_post_simple_event(session->event_port, PTY_EVENT_WRITABLE);
            }
        } else {
            PtyError error;
            pty_error_set(&error,
                          PTY_ERROR_DOMAIN_WIN32,
                          PTY_ERROR_IO,
                          GetLastError(),
                          "writing ConPTY input failed");
            windows_mark_input_closed(session, &error);
            EnterCriticalSection(&platform->mutex);
            platform->stopping = 1;
            WakeAllConditionVariable(&platform->condition);
            LeaveCriticalSection(&platform->mutex);
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
    WaitForSingleObject(platform->process, INFINITE);
    DWORD exit_code = 1;
    if (GetExitCodeProcess(platform->process, &exit_code)) {
        pty_post_process_exit(session->event_port, false, exit_code);
    } else {
        windows_post_error(session,
                           PTY_ERROR_IO,
                           GetLastError(),
                           "reading ConPTY process exit failed");
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

static int windows_create_process(const PtySpawnOptions *options,
                                  HANDLE *input_write,
                                  HANDLE *output_read,
                                  HANDLE *process,
                                  HANDLE *process_thread,
                                  HANDLE *job,
                                  DWORD *process_id,
                                  HPCON *pseudo_console,
                                  PtyError *error)
{
    HANDLE input_read = NULL;
    HANDLE output_write = NULL;
    STARTUPINFOEXW startup_info;
    PROCESS_INFORMATION process_info;
    SIZE_T attribute_size = 0;
    PPROC_THREAD_ATTRIBUTE_LIST attributes = NULL;
    LPWSTR command = NULL;
    LPWSTR environment = NULL;
    LPWSTR working_directory = NULL;
    int attributes_initialized = 0;
    memset(&startup_info, 0, sizeof(startup_info));
    memset(&process_info, 0, sizeof(process_info));

    if (!CreatePipe(&input_read, input_write, NULL, 0) ||
        !CreatePipe(output_read, &output_write, NULL, 0)) {
        pty_error_set(error,
                      PTY_ERROR_DOMAIN_WIN32,
                      PTY_ERROR_IO,
                      GetLastError(),
                      "creating ConPTY pipes failed");
        goto failure;
    }
    const COORD size = {
        .X = (SHORT)options->size.columns,
        .Y = (SHORT)options->size.rows,
    };
    if (FAILED(CreatePseudoConsole(size,
                                   input_read,
                                   output_write,
                                   0,
                                   pseudo_console))) {
        pty_error_set(error,
                      PTY_ERROR_DOMAIN_WIN32,
                      PTY_ERROR_SPAWN_FAILED,
                      GetLastError(),
                      "creating ConPTY failed");
        goto failure;
    }
    *job = CreateJobObjectW(NULL, NULL);
    if (*job == NULL) {
        pty_error_set(error,
                      PTY_ERROR_DOMAIN_WIN32,
                      PTY_ERROR_SPAWN_FAILED,
                      GetLastError(),
                      "creating process Job Object failed");
        goto failure;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info;
    memset(&job_info, 0, sizeof(job_info));
    job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(*job,
                                 JobObjectExtendedLimitInformation,
                                 &job_info,
                                 sizeof(job_info))) {
        pty_error_set(error,
                      PTY_ERROR_DOMAIN_WIN32,
                      PTY_ERROR_SPAWN_FAILED,
                      GetLastError(),
                      "configuring process Job Object failed");
        goto failure;
    }
    CloseHandle(input_read);
    input_read = NULL;
    CloseHandle(output_write);
    output_write = NULL;

    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    attributes = malloc(attribute_size);
    if (attributes == NULL ||
        !InitializeProcThreadAttributeList(attributes,
                                           1,
                                           0,
                                           &attribute_size) ||
        !UpdateProcThreadAttribute(attributes,
                                   0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   *pseudo_console,
                                   sizeof(*pseudo_console),
                                   NULL,
                                   NULL)) {
        pty_error_set(error,
                      PTY_ERROR_DOMAIN_WIN32,
                      PTY_ERROR_OUT_OF_MEMORY,
                      GetLastError(),
                      "configuring ConPTY process attributes failed");
        goto failure;
    }
    attributes_initialized = 1;

    command = pty_windows_build_command_line(options->executable,
                                             options->arguments,
                                             options->argument_count);
    environment = pty_windows_build_environment(options->environment,
                                                options->environment_count);
    working_directory = build_working_directory((char *)options->working_directory);
    if (command == NULL || environment == NULL ||
        (options->working_directory != NULL && working_directory == NULL)) {
        pty_error_set(error,
                      PTY_ERROR_DOMAIN_WIN32,
                      PTY_ERROR_OUT_OF_MEMORY,
                      ERROR_NOT_ENOUGH_MEMORY,
                      "building Windows process arguments failed");
        goto failure;
    }
    startup_info.StartupInfo.cb = sizeof(startup_info);
    startup_info.lpAttributeList = attributes;
    if (!CreateProcessW(NULL,
                        command,
                        NULL,
                        NULL,
                        FALSE,
                        EXTENDED_STARTUPINFO_PRESENT |
                            CREATE_UNICODE_ENVIRONMENT |
                            CREATE_SUSPENDED,
                        environment,
                        working_directory,
                        &startup_info.StartupInfo,
                        &process_info)) {
        pty_error_set(error,
                      PTY_ERROR_DOMAIN_WIN32,
                      PTY_ERROR_SPAWN_FAILED,
                      GetLastError(),
                      "creating ConPTY process failed");
        goto failure;
    }
    if (!AssignProcessToJobObject(*job, process_info.hProcess)) {
        pty_error_set(error,
                      PTY_ERROR_DOMAIN_WIN32,
                      PTY_ERROR_SPAWN_FAILED,
                      GetLastError(),
                      "assigning process to Job Object failed");
        goto process_failure;
    }
    *process = process_info.hProcess;
    *process_thread = process_info.hThread;
    *process_id = process_info.dwProcessId;
    if (attributes_initialized) DeleteProcThreadAttributeList(attributes);
    free(attributes);
    free(command);
    free(environment);
    free(working_directory);
    return 1;

process_failure:
    TerminateProcess(process_info.hProcess, 1);
    WaitForSingleObject(process_info.hProcess, INFINITE);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
failure:
    if (attributes_initialized) DeleteProcThreadAttributeList(attributes);
    free(attributes);
    free(command);
    free(environment);
    free(working_directory);
    if (*job != NULL) {
        CloseHandle(*job);
        *job = NULL;
    }
    if (*pseudo_console != NULL) {
        ClosePseudoConsole(*pseudo_console);
        *pseudo_console = NULL;
    }
    if (input_read != NULL) CloseHandle(input_read);
    if (output_write != NULL) CloseHandle(output_write);
    if (*input_write != NULL) {
        CloseHandle(*input_write);
        *input_write = NULL;
    }
    if (*output_read != NULL) {
        CloseHandle(*output_read);
        *output_read = NULL;
    }
    return 0;
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
        pty_post_spawn_failed(session->event_port, &error);
        pty_post_simple_event(session->event_port, PTY_EVENT_OUTPUT_CLOSED);
        pty_post_simple_event(session->event_port, PTY_EVENT_SESSION_CLOSED);
        pty_session_mark_closing(session);
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
        pty_post_spawn_failed(session->event_port, &error);
        pty_post_simple_event(session->event_port, PTY_EVENT_OUTPUT_CLOSED);
        pty_post_simple_event(session->event_port, PTY_EVENT_SESSION_CLOSED);
        pty_session_mark_closing(session);
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
    session->platform = platform;

    const int close_requested =
        InterlockedCompareExchange(&session->lifecycle,
                                   PTY_LIFECYCLE_STARTING,
                                   PTY_LIFECYCLE_STARTING) ==
        PTY_LIFECYCLE_CLOSING;
    if (close_requested) {
        windows_stop_process(platform);
        CloseHandle(process_thread);
        platform->reader_done = 1;
        platform->writer_done = 1;
        platform->waiter_done = 1;
        windows_mark_output_closed(session);
        windows_mark_input_closed(session, NULL);
        pty_post_simple_event(session->event_port, PTY_EVENT_SESSION_CLOSED);
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
        pty_debug_worker_finished(PTY_DEBUG_WORKER_READ);
        pty_session_release(session);
    }
    pty_debug_worker_started(PTY_DEBUG_WORKER_WRITE);
    pty_session_retain(session);
    platform->writer_thread = CreateThread(NULL, 0, windows_writer, session, 0, NULL);
    if (platform->writer_thread != NULL) platform->writer_started = 1;
    else {
        pty_debug_worker_finished(PTY_DEBUG_WORKER_WRITE);
        pty_session_release(session);
    }
    pty_debug_worker_started(PTY_DEBUG_WORKER_WAIT);
    pty_session_retain(session);
    platform->waiter_thread = CreateThread(NULL, 0, windows_waiter, session, 0, NULL);
    if (platform->waiter_thread != NULL) platform->waiter_started = 1;
    else {
        pty_debug_worker_finished(PTY_DEBUG_WORKER_WAIT);
        pty_session_release(session);
    }
    if (!platform->reader_started || !platform->writer_started ||
        !platform->waiter_started) {
        windows_stop_process(platform);
        ResumeThread(process_thread);
        CloseHandle(process_thread);
        if (platform->reader_started) WaitForSingleObject(platform->reader_thread, INFINITE);
        if (platform->writer_started) WaitForSingleObject(platform->writer_thread, INFINITE);
        if (platform->waiter_started) WaitForSingleObject(platform->waiter_thread, INFINITE);
        pty_error_set(&error,
                      PTY_ERROR_DOMAIN_INTERNAL,
                      PTY_ERROR_INTERNAL,
                      GetLastError(),
                      "starting Windows PTY workers failed");
        pty_post_spawn_failed(session->event_port, &error);
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
        pty_post_spawn_failed(session->event_port, &error);
        InterlockedExchange(&session->lifecycle, PTY_LIFECYCLE_CLOSING);
        windows_stop_process(platform);
        CloseHandle(process_thread);
        windows_free_options(&bootstrap->options);
        free(bootstrap);
        pty_session_release(session);
        return 0;
    }
    CloseHandle(process_thread);
    pty_post_spawned(session->event_port, process_id, 0x18);
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
        options->output_window_bytes == 0 || options->size.rows <= 0 ||
        options->size.columns <= 0) {
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
        pty_session_release(session);
        windows_free_options(&bootstrap->options);
        free(bootstrap);
        pty_session_release(session);
        pty_error_set(out_error,
                      PTY_ERROR_DOMAIN_WIN32,
                      PTY_ERROR_INTERNAL,
                      GetLastError(),
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
    PtyWindowsPlatform *platform = windows_platform(session);
    if (platform == NULL || length == 0) return PTY_WRITE_CLOSED;
    EnterCriticalSection(&platform->mutex);
    if (platform->stopping) {
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
    if (FAILED(ResizePseudoConsole(platform->pseudo_console, console_size))) {
        pty_error_set(out_error,
                      PTY_ERROR_DOMAIN_HRESULT,
                      PTY_ERROR_IO,
                      GetLastError(),
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
    if (!TerminateJobObject(platform->job, 1)) {
        pty_error_set(out_error,
                      PTY_ERROR_DOMAIN_WIN32,
                      PTY_ERROR_IO,
                      GetLastError(),
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
    session->output_credit = session->output_window_limit;
    WakeAllConditionVariable(&platform->condition);
    LeaveCriticalSection(&platform->mutex);
}

FFI_PLUGIN_EXPORT void pty_session_begin_close(PtySession *session)
{
    if (session == NULL) return;
    const LONG lifecycle = InterlockedCompareExchange(&session->lifecycle,
                                                      PTY_LIFECYCLE_CLOSING,
                                                      PTY_LIFECYCLE_RUNNING);
    if (lifecycle == PTY_LIFECYCLE_STARTING) {
        InterlockedExchange(&session->lifecycle, PTY_LIFECYCLE_CLOSING);
        return;
    }
    if (lifecycle != PTY_LIFECYCLE_RUNNING &&
        lifecycle != PTY_LIFECYCLE_CLOSING) {
        return;
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
    if (session == NULL) return;
    InterlockedExchange(&session->abandoned, 1);
    pty_session_begin_close(session);
    pty_session_release(session);
}
