#include "deck_intern.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "deck_intern";

typedef struct {
    uint32_t hash;
    uint32_t len;      /* 0 = empty slot */
    char    *data;     /* heap-owned, NUL-terminated */
} intern_slot_t;

static intern_slot_t *s_table = NULL;
static uint32_t       s_cap   = 0;
static uint32_t       s_count = 0;
static size_t         s_bytes = 0;
static bool           s_init  = false;

/* FNV-1a 32-bit. */
static uint32_t fnv1a(const char *s, uint32_t len)
{
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 0x01000193u;
    }
    /* Ensure non-zero: hash 0 is reserved as "unset" sentinel. */
    return h ? h : 1u;
}

static uint32_t pow2_ge(uint32_t n)
{
    uint32_t p = 8;
    while (p < n) p <<= 1;
    return p;
}

static bool ensure_cap(uint32_t target_cap);

void deck_intern_init(uint32_t initial_cap)
{
    if (s_init) return;
    uint32_t cap = pow2_ge(initial_cap ? initial_cap : 64);
    s_table = heap_caps_calloc(cap, sizeof(intern_slot_t), MALLOC_CAP_INTERNAL);
    if (!s_table) {
        ESP_LOGE(TAG, "init: calloc failed");
        return;
    }
    s_cap   = cap;
    s_count = 0;
    s_bytes = cap * sizeof(intern_slot_t);
    s_init  = true;
    ESP_LOGI(TAG, "init: cap=%u (%u bytes)", (unsigned)cap, (unsigned)s_bytes);
}

void deck_intern_reset(void)
{
    if (!s_init) return;
    for (uint32_t i = 0; i < s_cap; i++) {
        if (s_table[i].data) {
            heap_caps_free(s_table[i].data);
            s_table[i].data = NULL;
            s_table[i].hash = 0;
            s_table[i].len  = 0;
        }
    }
    heap_caps_free(s_table);
    s_table = NULL;
    s_cap = 0;
    s_count = 0;
    s_bytes = 0;
    s_init  = false;
}

uint32_t deck_intern_count(void) { return s_count; }
size_t   deck_intern_bytes(void) { return s_bytes; }

/* Linear-probing lookup. Returns slot pointer; *found=true if exact match. */
static intern_slot_t *probe(const char *s, uint32_t len, uint32_t h, bool *found)
{
    uint32_t mask = s_cap - 1;
    uint32_t idx  = h & mask;
    for (uint32_t tries = 0; tries < s_cap; tries++) {
        intern_slot_t *slot = &s_table[idx];
        if (slot->hash == 0) { *found = false; return slot; }
        if (slot->hash == h && slot->len == len &&
            memcmp(slot->data, s, len) == 0) {
            *found = true;
            return slot;
        }
        idx = (idx + 1) & mask;
    }
    *found = false;
    return NULL;
}

static bool ensure_cap(uint32_t target_cap)
{
    uint32_t new_cap = pow2_ge(target_cap);
    if (new_cap <= s_cap) return true;
    intern_slot_t *old_table = s_table;
    uint32_t       old_cap   = s_cap;

    s_table = heap_caps_calloc(new_cap, sizeof(intern_slot_t), MALLOC_CAP_INTERNAL);
    if (!s_table) {
        /* Internal RAM is tight; at DL2 levels the intern table can
         * grow to 64+ KB (4096 slots × 16 B) which competes with LVGL
         * and WiFi working sets. Retry from SPIRAM — the table is
         * mostly hash-probed with pointer compares, so external RAM
         * access is acceptable. */
        s_table = heap_caps_calloc(new_cap, sizeof(intern_slot_t),
                                   MALLOC_CAP_SPIRAM);
    }
    if (!s_table) {
        ESP_LOGE(TAG, "grow to %u failed (internal+spiram)", (unsigned)new_cap);
        s_table = old_table;
        return false;
    }
    s_cap = new_cap;
    s_bytes = s_bytes - (old_cap * sizeof(intern_slot_t))
                      + (new_cap * sizeof(intern_slot_t));

    /* Rehash non-empty slots. */
    for (uint32_t i = 0; i < old_cap; i++) {
        if (old_table[i].hash == 0) continue;
        bool found = false;
        intern_slot_t *dst = probe(old_table[i].data, old_table[i].len,
                                    old_table[i].hash, &found);
        if (dst) *dst = old_table[i];
    }
    heap_caps_free(old_table);
    return true;
}

const char *deck_intern(const char *s, uint32_t len)
{
    if (!s_init) deck_intern_init(64);
    if (!s || !s_table) return NULL;

    /* Keep load factor ≤ 0.75. */
    if (s_count * 4 >= s_cap * 3) {
        if (!ensure_cap(s_cap * 2)) return NULL;
    }

    uint32_t h = fnv1a(s, len);
    bool found = false;
    intern_slot_t *slot = probe(s, len, h, &found);
    if (!slot) return NULL;
    if (found) return slot->data;

    /* New entry. Prefer internal RAM for cache locality on hot
     * identifiers; fall back to SPIRAM if internal is tight (same
     * policy as the table grow above). */
    char *buf = heap_caps_malloc(len + 1, MALLOC_CAP_INTERNAL);
    if (!buf) buf = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;
    memcpy(buf, s, len);
    buf[len] = '\0';
    slot->hash = h;
    slot->len  = len;
    slot->data = buf;
    s_count++;
    s_bytes += len + 1;
    return buf;
}

const char *deck_intern_cstr(const char *s)
{
    return s ? deck_intern(s, (uint32_t)strlen(s)) : NULL;
}
