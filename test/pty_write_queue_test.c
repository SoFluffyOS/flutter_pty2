#include <assert.h>
#include <stdint.h>

#include "../src/pty_internal.h"

int main(void)
{
    PtyDebugStats stats;
    pty_debug_get_stats(&stats);
    assert(stats.pending_write_chunks == 0);
    assert(stats.pending_write_bytes == 0);

    PtyWriteQueue queue;
    pty_write_queue_init(&queue, 4);

    const uint8_t first[] = {1, 2, 3};
    const uint8_t second[] = {4, 5};
    const uint8_t rejected[] = {6};

    assert(pty_write_queue_try_enqueue(&queue, first, sizeof(first), 11) ==
           PTY_WRITE_ACCEPTED);
    pty_debug_get_stats(&stats);
    assert(stats.pending_write_chunks == 1);
    assert(stats.pending_write_bytes == sizeof(first));
    assert(pty_write_queue_try_enqueue(&queue, second, sizeof(second), 12) ==
           PTY_WRITE_BACKPRESSURED);
    assert(pty_write_queue_pending_bytes(&queue) == sizeof(first));

    PtyWriteChunk *chunk = pty_write_queue_dequeue(&queue);
    assert(chunk != NULL);
    assert(chunk->request_id == 11);
    assert(chunk->length == sizeof(first));
    assert(chunk->offset == 0);
    assert(chunk->bytes[0] == 1);
    chunk->offset = 2;
    assert(pty_write_queue_requeue_front(&queue, chunk) == PTY_WRITE_ACCEPTED);
    assert(pty_write_queue_pending_bytes(&queue) == sizeof(first) - 2);
    pty_debug_get_stats(&stats);
    assert(stats.pending_write_chunks == 1);
    assert(stats.pending_write_bytes == sizeof(first) - 2);

    chunk = pty_write_queue_dequeue(&queue);
    assert(chunk != NULL);
    assert(chunk->request_id == 11);
    assert(chunk->offset == 2);
    pty_write_chunk_free(chunk);
    pty_debug_get_stats(&stats);
    assert(stats.pending_write_chunks == 0);
    assert(stats.pending_write_bytes == 0);

    assert(pty_write_queue_try_enqueue(&queue, rejected, sizeof(rejected), 13) ==
           PTY_WRITE_ACCEPTED);
    assert(pty_write_queue_pending_bytes(&queue) == sizeof(rejected));
    pty_write_queue_dispose(&queue);
    pty_debug_get_stats(&stats);
    assert(stats.pending_write_chunks == 0);
    assert(stats.pending_write_bytes == 0);
    return 0;
}
