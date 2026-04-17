#pragma once

/* deck_arena — bump allocator for per-module AST + transient load state.
 *
 * The AST for an app is built once, read many times by the evaluator, and
 * freed all at once when the app unloads. Refcount bookkeeping per node
 * is unnecessary overhead; a bump arena gives zero-overhead allocation
 * and trivial bulk-free.
 *
 * Chunks grow on demand; alignment is 8 bytes.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct deck_arena_chunk deck_arena_chunk_t;

typedef struct {
    deck_arena_chunk_t *head;    /* newest chunk */
    size_t              chunk_bytes;
    size_t              total_bytes;
    size_t              used_bytes;
} deck_arena_t;

/* Initialize. chunk_size=0 → 4 KB default. */
void  deck_arena_init(deck_arena_t *a, size_t chunk_size);

/* Allocate aligned bytes. Returns NULL on OOM. */
void *deck_arena_alloc(deck_arena_t *a, size_t bytes);

/* Allocate + zero. */
void *deck_arena_zalloc(deck_arena_t *a, size_t bytes);

/* Duplicate a buffer into the arena. */
void *deck_arena_memdup(deck_arena_t *a, const void *src, size_t bytes);

/* Duplicate a C string. */
char *deck_arena_strdup(deck_arena_t *a, const char *s);

/* Release all chunks. Subsequent alloc calls transparently reinit. */
void  deck_arena_reset(deck_arena_t *a);

#ifdef __cplusplus
}
#endif
