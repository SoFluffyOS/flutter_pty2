#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Windows.h>
#include <TlHelp32.h>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE \
    ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)
typedef HANDLE HPCON;
HRESULT WINAPI CreatePseudoConsole(COORD size,
                                   HANDLE input,
                                   HANDLE output,
                                   DWORD flags,
                                   HPCON *pseudo_console);
HRESULT WINAPI ResizePseudoConsole(HPCON pseudo_console, COORD size);
void WINAPI ClosePseudoConsole(HPCON pseudo_console);
#endif

#include "flutter_pty.h"

#include "include/dart_api.h"
#include "include/dart_api_dl.h"
#include "include/dart_native_api.h"

#define MAX_PENDING_WRITE_BYTES (8 * 1024 * 1024)

typedef struct WriteChunk
{
    uint8_t *data;
    size_t length;
    struct WriteChunk *next;
} WriteChunk;

static LPWSTR utf8_to_wide(const char *value)
{
    if (value == NULL) return NULL;

    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0);
    if (length == 0) return NULL;

    LPWSTR converted = malloc(length * sizeof(WCHAR));
    if (converted == NULL) return NULL;

    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            value,
                            -1,
                            converted,
                            length) == 0)
    {
        free(converted);
        return NULL;
    }

    return converted;
}

static int append_quoted_argument(LPWSTR command, int offset, LPCWSTR argument)
{
    command[offset++] = L'"';
    int backslashes = 0;

    for (int index = 0; argument[index] != L'\0'; index++)
    {
        if (argument[index] == L'\\')
        {
            backslashes++;
            continue;
        }

        if (argument[index] == L'"')
        {
            for (int count = 0; count < backslashes * 2 + 1; count++)
                command[offset++] = L'\\';
            command[offset++] = L'"';
            backslashes = 0;
            continue;
        }

        for (int count = 0; count < backslashes; count++) command[offset++] = L'\\';
        backslashes = 0;
        command[offset++] = argument[index];
    }

    for (int count = 0; count < backslashes * 2; count++) command[offset++] = L'\\';
    command[offset++] = L'"';
    return offset;
}

static LPWSTR build_command(char *executable, char **arguments)
{
    int argument_count = 0;
    if (arguments != NULL)
    {
        while (arguments[argument_count] != NULL) argument_count++;
    }
    if (argument_count == 0 && executable == NULL) return NULL;

    int command_capacity = 1;
    if (argument_count == 0)
    {
        command_capacity += (int)strlen(executable) * 2 + 3;
    }
    for (int index = 0; index < argument_count; index++)
    {
        command_capacity += (int)strlen(arguments[index]) * 2 + 4;
    }

    LPWSTR command = malloc(command_capacity * sizeof(WCHAR));
    if (command == NULL) return NULL;

    int offset = 0;
    int values_count = argument_count == 0 ? 1 : argument_count;
    for (int index = 0; index < values_count; index++)
    {
        const char *value = argument_count == 0 ? executable : arguments[index];
        LPWSTR converted = utf8_to_wide(value);
        if (converted == NULL)
        {
            free(command);
            return NULL;
        }

        if (offset > 0) command[offset++] = L' ';
        offset = append_quoted_argument(command, offset, converted);
        free(converted);
    }

    command[offset] = L'\0';
    return command;
}

static LPWSTR build_environment(char **environment)
{
    LPWSTR environment_block = NULL;
    int environment_block_length = 0;

    if (environment != NULL)
    {
        int i = 0;

        while (environment[i] != NULL)
        {
            LPWSTR converted = utf8_to_wide(environment[i]);
            if (converted == NULL) return NULL;
            environment_block_length += (int)wcslen(converted) + 1;
            free(converted);
            i++;
        }
    }

    environment_block = malloc((environment_block_length + 2) * sizeof(WCHAR));

    if (environment_block != NULL)
    {
        int i = 0;

        if (environment != NULL)
        {
            int j = 0;

            while (environment[j] != NULL)
            {
                LPWSTR converted = utf8_to_wide(environment[j]);
                if (converted == NULL)
                {
                    free(environment_block);
                    return NULL;
                }
                int converted_length = (int)wcslen(converted);
                memcpy(environment_block + i,
                       converted,
                       (converted_length + 1) * sizeof(WCHAR));
                i += converted_length + 1;
                free(converted);

                j++;
            }
        }

        environment_block[i] = 0;
        environment_block[i + 1] = 0;
    }

    return environment_block;
}

static LPWSTR build_working_directory(char *working_directory)
{
    return utf8_to_wide(working_directory);
}

typedef struct ReadLoopOptions
{
    HANDLE fd;

    Dart_Port port;

    Dart_Port done_port;

    HANDLE hMutex;

    BOOL ackRead;

} ReadLoopOptions;

static DWORD WINAPI read_loop(LPVOID arg)
{
    ReadLoopOptions *options = (ReadLoopOptions *)arg;

    char buffer[65536];

    while (1)
    {
        DWORD readlen = 0;

        if (options->ackRead &&
            WaitForSingleObject(options->hMutex, INFINITE) != WAIT_OBJECT_0)
        {
            break;
        }

        BOOL ok = ReadFile(options->fd, buffer, sizeof(buffer), &readlen, NULL);

        if (!ok)
        {
            break;
        }

        if (readlen <= 0)
        {
            break;
        }

        Dart_CObject result;
        result.type = Dart_CObject_kTypedData;
        result.value.as_typed_data.type = Dart_TypedData_kUint8;
        result.value.as_typed_data.length = readlen;
        result.value.as_typed_data.values = (uint8_t *)buffer;

        if (!Dart_PostCObject_DL(options->port, &result)) break;
    }

    Dart_PostInteger_DL(options->done_port, 0);
    free(options);
    return 0;
}

static HANDLE start_read_thread(HANDLE fd,
                                Dart_Port port,
                                Dart_Port done_port,
                                HANDLE mutex,
                                BOOL ackRead)
{
    ReadLoopOptions *options = malloc(sizeof(ReadLoopOptions));
    if (options == NULL) return NULL;

    options->fd = fd;
    options->port = port;
    options->done_port = done_port;
    options->hMutex = mutex;
    options->ackRead = ackRead;

    DWORD thread_id;

    HANDLE thread = CreateThread(NULL, 0, read_loop, options, 0, &thread_id);

    if (thread == NULL)
    {
        free(options);
    }

    return thread;
}

typedef struct WaitExitOptions
{
    HANDLE pid;

    Dart_Port port;

} WaitExitOptions;

static DWORD WINAPI wait_exit_thread(LPVOID arg)
{
    WaitExitOptions *options = (WaitExitOptions *)arg;

    DWORD exit_code = UINT32_MAX;

    DWORD wait_result = WaitForSingleObject(options->pid, INFINITE);
    if (wait_result == WAIT_OBJECT_0)
    {
        GetExitCodeProcess(options->pid, &exit_code);
    }

    Dart_PostInteger_DL(options->port, (int32_t)exit_code);

    free(options);
    return 0;
}

static HANDLE start_wait_exit_thread(HANDLE pid, Dart_Port port)
{
    WaitExitOptions *options = malloc(sizeof(WaitExitOptions));
    if (options == NULL) return NULL;

    options->pid = pid;
    options->port = port;
    DWORD thread_id;

    HANDLE thread = CreateThread(NULL, 0, wait_exit_thread, options, 0, &thread_id);

    if (thread == NULL)
    {
        free(options);
    }

    return thread;
}

typedef struct WriteLoopOptions
{
    HANDLE fd;
    CRITICAL_SECTION *mutex;
    CONDITION_VARIABLE *condition;
    BOOL *stopping;
    WriteChunk **head;
    WriteChunk **tail;
    size_t *pending_bytes;
} WriteLoopOptions;

static DWORD WINAPI write_loop(LPVOID arg)
{
    WriteLoopOptions *options = (WriteLoopOptions *)arg;

    while (1)
    {
        EnterCriticalSection(options->mutex);
        while (*options->head == NULL && !*options->stopping)
        {
            SleepConditionVariableCS(options->condition,
                                     options->mutex,
                                     INFINITE);
        }

        if (*options->stopping)
        {
            LeaveCriticalSection(options->mutex);
            break;
        }

        WriteChunk *chunk = *options->head;
        LeaveCriticalSection(options->mutex);

        DWORD written = 0;
        BOOL succeeded = TRUE;
        while (written < (DWORD)chunk->length)
        {
            DWORD bytes_written = 0;
            if (!WriteFile(options->fd,
                           chunk->data + written,
                           (DWORD)chunk->length - written,
                           &bytes_written,
                           NULL) ||
                bytes_written == 0)
            {
                succeeded = FALSE;
                break;
            }
            written += bytes_written;
        }

        EnterCriticalSection(options->mutex);
        if (*options->head == chunk)
        {
            *options->head = chunk->next;
            if (*options->head == NULL) *options->tail = NULL;
            *options->pending_bytes -= chunk->length;
        }
        if (!succeeded) *options->stopping = TRUE;
        LeaveCriticalSection(options->mutex);

        free(chunk->data);
        free(chunk);
        if (!succeeded) break;
    }

    free(options);
    return 0;
}

typedef struct PtyHandle
{
    HANDLE inputWriteSide;

    HANDLE outputReadSide;

    HPCON hPty;

    DWORD dwProcessId;

    BOOL ackRead;

    HANDLE hMutex;

    HANDLE processHandle;

    HANDLE readThread;

    HANDLE waitThread;

    HANDLE writeThread;

    CRITICAL_SECTION writeMutex;

    CONDITION_VARIABLE writeCondition;

    BOOL stopping;

    WriteChunk *writeHead;

    WriteChunk *writeTail;

    size_t pendingWriteBytes;

} PtyHandle;

static HANDLE start_write_thread(PtyHandle *pty)
{
    WriteLoopOptions *options = malloc(sizeof(WriteLoopOptions));
    if (options == NULL) return NULL;

    options->fd = pty->inputWriteSide;
    options->mutex = &pty->writeMutex;
    options->condition = &pty->writeCondition;
    options->stopping = &pty->stopping;
    options->head = &pty->writeHead;
    options->tail = &pty->writeTail;
    options->pending_bytes = &pty->pendingWriteBytes;

    DWORD thread_id;
    HANDLE thread = CreateThread(NULL, 0, write_loop, options, 0, &thread_id);
    if (thread == NULL) free(options);
    return thread;
}

char *error_message = NULL;

FFI_PLUGIN_EXPORT PtyHandle *pty_create(PtyOptions *options)
{
    error_message = NULL;

    if (options == NULL || options->executable == NULL ||
        options->arguments == NULL || options->rows <= 0 ||
        options->rows > INT16_MAX || options->cols <= 0 ||
        options->cols > INT16_MAX)
    {
        error_message = "Invalid PTY options";
        return NULL;
    }
    HANDLE inputReadSide = NULL;
    HANDLE inputWriteSide = NULL;
    HANDLE outputReadSide = NULL;
    HANDLE outputWriteSide = NULL;
    HPCON hPty = NULL;
    STARTUPINFOEX startupInfo;
    PROCESS_INFORMATION processInfo;
    LPWSTR command = NULL;
    LPWSTR environment_block = NULL;
    LPWSTR working_directory = NULL;
    BOOL attributesInitialized = FALSE;

    ZeroMemory(&startupInfo, sizeof(startupInfo));
    ZeroMemory(&processInfo, sizeof(processInfo));

    if (!CreatePipe(&inputReadSide, &inputWriteSide, NULL, 0))
    {
        error_message = "Failed to create input pipe";
        return NULL;
    }

    if (!CreatePipe(&outputReadSide, &outputWriteSide, NULL, 0))
    {
        error_message = "Failed to create output pipe";
        goto fail;
    }

    COORD size;

    size.X = options->cols;
    size.Y = options->rows;

    HRESULT result = CreatePseudoConsole(size, inputReadSide, outputWriteSide, 0, &hPty);

    if (FAILED(result))
    {
        error_message = "Failed to create pseudo console";
        goto fail;
    }

    CloseHandle(inputReadSide);
    inputReadSide = NULL;
    CloseHandle(outputWriteSide);
    outputWriteSide = NULL;

    startupInfo.StartupInfo.cb = sizeof(startupInfo);

    startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.StartupInfo.hStdInput = NULL;
    startupInfo.StartupInfo.hStdOutput = NULL;
    startupInfo.StartupInfo.hStdError = NULL;

    SIZE_T bytesRequired = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytesRequired);
    startupInfo.lpAttributeList = (PPROC_THREAD_ATTRIBUTE_LIST)malloc(bytesRequired);
    if (startupInfo.lpAttributeList == NULL)
    {
        error_message = "Failed to allocate proc thread attribute list";
        goto fail;
    }

    BOOL ok = InitializeProcThreadAttributeList(startupInfo.lpAttributeList, 1, 0, &bytesRequired);

    if (!ok)
    {
        error_message = "Failed to initialize proc thread attribute list";
        goto fail;
    }
    attributesInitialized = TRUE;

    ok = UpdateProcThreadAttribute(startupInfo.lpAttributeList,
                                   0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   hPty,
                                   sizeof(hPty),
                                   NULL,
                                   NULL);

    if (!ok)
    {
        error_message = "Failed to update proc thread attribute list";
        goto fail;
    }

    command = build_command(options->executable, options->arguments);
    environment_block = build_environment(options->environment);
    working_directory = build_working_directory(options->working_directory);
    if (command == NULL || environment_block == NULL ||
        (options->working_directory != NULL && working_directory == NULL))
    {
        error_message = "Failed to allocate process arguments";
        goto fail;
    }

    ok = CreateProcessW(NULL,
                        command,
                        NULL,
                        NULL,
                        FALSE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                        environment_block,
                        working_directory,
                        &startupInfo.StartupInfo,
                        &processInfo);

    free(command);
    command = NULL;
    free(environment_block);
    environment_block = NULL;
    free(working_directory);
    working_directory = NULL;
    DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
    attributesInitialized = FALSE;
    free(startupInfo.lpAttributeList);
    startupInfo.lpAttributeList = NULL;

    if (!ok)
    {
        error_message = "Failed to create process";
        goto fail;
    }

    CloseHandle(processInfo.hThread);
    processInfo.hThread = NULL;

    HANDLE mutex = CreateSemaphore(
        NULL, // default security attributes
        1,    // initial count
        1,    // maximum count
        NULL);
    if (mutex == NULL)
    {
        error_message = "Failed to create read semaphore";
        goto fail_process;
    }

    PtyHandle *pty = calloc(1, sizeof(PtyHandle));

    if (pty == NULL)
    {
        error_message = "Failed to allocate pty handle";
        CloseHandle(mutex);
        goto fail_process;
    }

    pty->inputWriteSide = inputWriteSide;
    pty->outputReadSide = outputReadSide;
    pty->hPty = hPty;
    pty->dwProcessId = processInfo.dwProcessId;
    pty->ackRead = options->ackRead;
    pty->hMutex = mutex;
    pty->processHandle = processInfo.hProcess;
    InitializeCriticalSection(&pty->writeMutex);
    InitializeConditionVariable(&pty->writeCondition);

    pty->readThread = start_read_thread(outputReadSide,
                                        options->stdout_port,
                                        options->output_done_port,
                                        mutex,
                                        options->ackRead);
    pty->waitThread = start_wait_exit_thread(processInfo.hProcess, options->exit_port);
    pty->writeThread = start_write_thread(pty);

    if (pty->readThread == NULL || pty->waitThread == NULL ||
        pty->writeThread == NULL)
    {
        error_message = "Failed to start ConPTY worker threads";
        TerminateProcess(processInfo.hProcess, 1);
        ReleaseSemaphore(mutex, 1, NULL);
        EnterCriticalSection(&pty->writeMutex);
        pty->stopping = TRUE;
        WakeAllConditionVariable(&pty->writeCondition);
        LeaveCriticalSection(&pty->writeMutex);
        if (pty->readThread != NULL)
        {
            CancelSynchronousIo(pty->readThread);
        }
        ClosePseudoConsole(hPty);
        CloseHandle(inputWriteSide);
        CloseHandle(outputReadSide);
        if (pty->readThread != NULL)
        {
            WaitForSingleObject(pty->readThread, INFINITE);
            CloseHandle(pty->readThread);
        }
        if (pty->waitThread != NULL)
        {
            WaitForSingleObject(pty->waitThread, INFINITE);
            CloseHandle(pty->waitThread);
        }
        if (pty->writeThread != NULL)
        {
            WaitForSingleObject(pty->writeThread, INFINITE);
            CloseHandle(pty->writeThread);
        }
        CloseHandle(processInfo.hProcess);
        CloseHandle(mutex);
        DeleteCriticalSection(&pty->writeMutex);
        free(pty);
        return NULL;
    }

    return pty;

fail_process:
    TerminateProcess(processInfo.hProcess, 1);
    WaitForSingleObject(processInfo.hProcess, INFINITE);

fail:
    if (startupInfo.lpAttributeList != NULL)
    {
        if (attributesInitialized)
            DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
        free(startupInfo.lpAttributeList);
    }
    free(command);
    free(environment_block);
    free(working_directory);
    if (processInfo.hThread != NULL) CloseHandle(processInfo.hThread);
    if (processInfo.hProcess != NULL) CloseHandle(processInfo.hProcess);
    if (hPty != NULL) ClosePseudoConsole(hPty);
    if (inputReadSide != NULL) CloseHandle(inputReadSide);
    if (inputWriteSide != NULL) CloseHandle(inputWriteSide);
    if (outputReadSide != NULL) CloseHandle(outputReadSide);
    if (outputWriteSide != NULL) CloseHandle(outputWriteSide);
    return NULL;
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
    chunk->next = NULL;

    EnterCriticalSection(&handle->writeMutex);
    if (handle->stopping ||
        handle->pendingWriteBytes >
            MAX_PENDING_WRITE_BYTES - (size_t)length)
    {
        LeaveCriticalSection(&handle->writeMutex);
        free(chunk->data);
        free(chunk);
        return 0;
    }

    if (handle->writeTail == NULL)
    {
        handle->writeHead = chunk;
    }
    else
    {
        handle->writeTail->next = chunk;
    }
    handle->writeTail = chunk;
    handle->pendingWriteBytes += chunk->length;
    WakeConditionVariable(&handle->writeCondition);
    LeaveCriticalSection(&handle->writeMutex);

    return 1;
}

FFI_PLUGIN_EXPORT void pty_ack_read(PtyHandle *handle)
{
    if (handle == NULL) return;

    if (handle->ackRead)
    {
        ReleaseSemaphore(handle->hMutex, 1, NULL);
    }
}

FFI_PLUGIN_EXPORT int pty_resize(PtyHandle *handle,
                                 int rows,
                                 int cols,
                                 int pixel_width,
                                 int pixel_height)
{
    if (handle == NULL || rows <= 0 || rows > INT16_MAX ||
        cols <= 0 || cols > INT16_MAX ||
        pixel_width < 0 || pixel_width > UINT16_MAX ||
        pixel_height < 0 || pixel_height > UINT16_MAX)
    {
        return -1;
    }

    (void)pixel_width;
    (void)pixel_height;

    COORD size;

    size.X = cols;
    size.Y = rows;

    return ResizePseudoConsole(handle->hPty, size);
}

FFI_PLUGIN_EXPORT int pty_getpid(PtyHandle *handle)
{
    if (handle == NULL) return -1;
    return (int)handle->dwProcessId;
}

FFI_PLUGIN_EXPORT int pty_has_running_foreground_process(PtyHandle *handle)
{
    if (handle == NULL) return 0;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(PROCESSENTRY32);
    BOOL has_entry = Process32First(snapshot, &entry);

    while (has_entry)
    {
        if (entry.th32ParentProcessID == handle->dwProcessId)
        {
            CloseHandle(snapshot);
            return 1;
        }

        has_entry = Process32Next(snapshot, &entry);
    }

    CloseHandle(snapshot);
    return 0;
}

static void terminate_child_processes(DWORD parent_process_id)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(PROCESSENTRY32);
    BOOL has_entry = Process32First(snapshot, &entry);

    while (has_entry)
    {
        if (entry.th32ParentProcessID == parent_process_id)
        {
            terminate_child_processes(entry.th32ProcessID);

            HANDLE child = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
            if (child != NULL)
            {
                TerminateProcess(child, 1);
                CloseHandle(child);
            }
        }

        has_entry = Process32Next(snapshot, &entry);
    }

    CloseHandle(snapshot);
}

FFI_PLUGIN_EXPORT int pty_kill(PtyHandle *handle, int signal_number)
{
    if (handle == NULL) return 0;

    (void)signal_number;
    terminate_child_processes(handle->dwProcessId);
    return TerminateProcess(handle->processHandle, 1) != 0;
}

FFI_PLUGIN_EXPORT char *pty_error()
{
    return error_message;
}

FFI_PLUGIN_EXPORT void pty_destroy(PtyHandle *handle)
{
    if (handle == NULL) return;

    EnterCriticalSection(&handle->writeMutex);
    handle->stopping = TRUE;
    WakeAllConditionVariable(&handle->writeCondition);
    LeaveCriticalSection(&handle->writeMutex);
    terminate_child_processes(handle->dwProcessId);
    TerminateProcess(handle->processHandle, 1);
    ReleaseSemaphore(handle->hMutex, 1, NULL);
    CancelSynchronousIo(handle->readThread);
    CancelSynchronousIo(handle->writeThread);
    ClosePseudoConsole(handle->hPty);
    CloseHandle(handle->inputWriteSide);
    CloseHandle(handle->outputReadSide);
    WaitForSingleObject(handle->readThread, INFINITE);
    WaitForSingleObject(handle->waitThread, INFINITE);
    WaitForSingleObject(handle->writeThread, INFINITE);
    CloseHandle(handle->readThread);
    CloseHandle(handle->waitThread);
    CloseHandle(handle->writeThread);
    CloseHandle(handle->processHandle);
    CloseHandle(handle->hMutex);
    WriteChunk *chunk = handle->writeHead;
    while (chunk != NULL)
    {
        WriteChunk *next = chunk->next;
        free(chunk->data);
        free(chunk);
        chunk = next;
    }
    DeleteCriticalSection(&handle->writeMutex);
    free(handle);
}
