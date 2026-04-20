#include "deck_arena.h"

#include "esp_heap_caps.h"
#include <string.h>

#define ALIGN        8
#define DEFAULT_SZ   4096

struct deck_arena_chunk {
    deck_arena_chunk_t *next;
    size_t              cap;
    size_t              used;
    /* payload starts at (&this + 1) via flexible array idiom */
    char                data[];
};

static size_t align_up(size_t n)
{
    return (n + (ALIGN - 1)) & ~(size_t)(ALIGN - 1);
}

void deck_arena_init(deck_arena_t *a, size_t chunk_size)
{
    if (!a) return;
    a->head         = NULL;
    a->chunk_bytes  = chunk_size ? chunk_size : DEFAULT_SZ;
    a->total_bytes  = 0;
    a->used_bytes   = 0;
}

static deck_arena_chunk_t *new_chunk(deck_arena_t *a, size_t min_bytes)
{
    size_t cap = a->chunk_bytes;
    if (min_bytes > cap) cap = min_bytes;
    /* Prefer internal RAM for the common small-AST case (cache hit
     * rate matters), fall back to SPIRAM when internal is tight. At
     * DL2 with the full stdlib + WiFi + LVGL loaded, internal stays
     * in the 30-50 KB free band during app runs; any 4 KB chunk that
     * can't fit internal goes external without failing the load. */
    deck_arena_chunk_t *c =
        heap_caps_malloc(sizeof(deck_arena_chunk_t) + cap, MALLOC_CAP_INTERNAL);
    if (!c) c = heap_caps_malloc(sizeof(deck_arena_chunk_t) + cap, MALLOC_CAP_SPIRAM);
    if (!c) return NULL;
    c->next = a->head;
    c->cap  = cap;
    c->used = 0;
    a->head = c;
    a->total_bytes += sizeof(deck_arena_chunk_t) + cap;
    return c;
}

void *deck_arena_alloc(deck_arena_t *a, size_t bytes)
{
    if (!a) return NULL;
    size_t n = align_up(bytes);
    if (!a->head || a->head->used + n > a->head->cap) {
        if (!new_chunk(a, n)) return NULL;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += n;
    a->used_bytes += n;
    return p;
}

void *deck_arena_zalloc(deck_arena_t *a, size_t bytes)
{
    void *p = deck_arena_alloc(a, bytes);
    if (p) memset(p, 0, bytes);
    return p;
}

void *deck_arena_memdup(deck_arena_t *a, const void *src, size_t bytes)
{
    void *p = deck_arena_alloc(a, bytes);
    if (p && src && bytes) memcpy(p, src, bytes);
    return p;
}

char *deck_arena_strdup(deck_arena_t *a, const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = deck_arena_alloc(a, n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void deck_arena_reset(deck_arena_t *a)
{
    if (!a) return;
    deck_arena_chunk_t *c = a->head;
    while (c) {
        deck_arena_chunk_t *next = c->next;
        heap_caps_free(c);
        c = next;
    }
    a->head        = NULL;
    a->total_bytes = 0;
    a->used_bytes  = 0;
}
