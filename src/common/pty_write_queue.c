#include "../pty_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void mutex_init(PtyMutex *mutex)
{
#if defined(_WIN32)
    InitializeCriticalSection(mutex);
#else
    pthread_mutex_init(mutex, NULL);
#endif
}

static void mutex_lock(PtyMutex *mutex)
{
#if defined(_WIN32)
    EnterCriticalSection(mutex);
#else
    pthread_mutex_lock(mutex);
#endif
}

static void mutex_unlock(PtyMutex *mutex)
{
#if defined(_WIN32)
    LeaveCriticalSection(mutex);
#else
    pthread_mutex_unlock(mutex);
#endif
}

static void mutex_dispose(PtyMutex *mutex)
{
#if defined(_WIN32)
    DeleteCriticalSection(mutex);
#else
    pthread_mutex_destroy(mutex);
#endif
}

void pty_write_queue_init(PtyWriteQueue *queue, uint64_t limit)
{
    if (queue == NULL) return;
    queue->head = NULL;
    queue->tail = NULL;
    queue->limit = limit;
    queue->pending_bytes = 0;
    queue->initialized = 0;
    mutex_init(&queue->mutex);
    queue->initialized = 1;
}

void pty_write_chunk_free(PtyWriteChunk *chunk)
{
    if (chunk == NULL) return;
    free(chunk->bytes);
    free(chunk);
}

void pty_write_queue_dispose(PtyWriteQueue *queue)
{
    if (queue == NULL || !queue->initialized) return;
    mutex_lock(&queue->mutex);
    PtyWriteChunk *chunk = queue->head;
    queue->head = NULL;
    queue->tail = NULL;
    queue->pending_bytes = 0;
    mutex_unlock(&queue->mutex);
    while (chunk != NULL) {
        PtyWriteChunk *next = chunk->next;
        pty_debug_pending_write_dequeued(chunk->length - chunk->offset);
        pty_write_chunk_free(chunk);
        chunk = next;
    }
    mutex_dispose(&queue->mutex);
    queue->initialized = 0;
}

int pty_write_queue_try_enqueue(PtyWriteQueue *queue,
                                 const uint8_t *bytes,
                                 uint64_t length,
                                 uint64_t request_id)
{
    if (queue == NULL || !queue->initialized || (length != 0 && bytes == NULL)) {
        return PTY_WRITE_ERROR;
    }
    if (length == 0) return PTY_WRITE_ACCEPTED;
    if (length > SIZE_MAX || length > queue->limit) {
        return PTY_WRITE_BACKPRESSURED;
    }

    PtyWriteChunk *chunk = malloc(sizeof(*chunk));
    if (chunk == NULL) return PTY_WRITE_ERROR;
    chunk->bytes = malloc((size_t)length);
    if (chunk->bytes == NULL) {
        free(chunk);
        return PTY_WRITE_ERROR;
    }
    memcpy(chunk->bytes, bytes, (size_t)length);
    chunk->next = NULL;
    chunk->length = length;
    chunk->offset = 0;
    chunk->request_id = request_id;

    mutex_lock(&queue->mutex);
    if (queue->pending_bytes > queue->limit - length) {
        mutex_unlock(&queue->mutex);
        pty_write_chunk_free(chunk);
        return PTY_WRITE_BACKPRESSURED;
    }
    if (queue->tail == NULL) {
        queue->head = chunk;
    } else {
        queue->tail->next = chunk;
    }
    queue->tail = chunk;
    queue->pending_bytes += length;
    pty_debug_pending_write_enqueued(length);
    mutex_unlock(&queue->mutex);
    return PTY_WRITE_ACCEPTED;
}

PtyWriteChunk *pty_write_queue_dequeue(PtyWriteQueue *queue)
{
    if (queue == NULL || !queue->initialized) return NULL;
    mutex_lock(&queue->mutex);
    PtyWriteChunk *chunk = queue->head;
    if (chunk != NULL) {
        queue->head = chunk->next;
        if (queue->head == NULL) queue->tail = NULL;
        queue->pending_bytes -= chunk->length - chunk->offset;
        pty_debug_pending_write_dequeued(chunk->length - chunk->offset);
        chunk->next = NULL;
    }
    mutex_unlock(&queue->mutex);
    return chunk;
}

int pty_write_queue_requeue_front(PtyWriteQueue *queue, PtyWriteChunk *chunk)
{
    if (queue == NULL || !queue->initialized || chunk == NULL) {
        return PTY_WRITE_ERROR;
    }
    if (chunk->offset > chunk->length) return PTY_WRITE_ERROR;
    const uint64_t remaining = chunk->length - chunk->offset;
    mutex_lock(&queue->mutex);
    if (queue->pending_bytes > queue->limit ||
        remaining > queue->limit - queue->pending_bytes) {
        mutex_unlock(&queue->mutex);
        return PTY_WRITE_BACKPRESSURED;
    }
    chunk->next = queue->head;
    queue->head = chunk;
    if (queue->tail == NULL) queue->tail = chunk;
    queue->pending_bytes += remaining;
    pty_debug_pending_write_enqueued(remaining);
    mutex_unlock(&queue->mutex);
    return PTY_WRITE_ACCEPTED;
}

uint64_t pty_write_queue_pending_bytes(PtyWriteQueue *queue)
{
    if (queue == NULL || !queue->initialized) return 0;
    mutex_lock(&queue->mutex);
    const uint64_t pending = queue->pending_bytes;
    mutex_unlock(&queue->mutex);
    return pending;
}
