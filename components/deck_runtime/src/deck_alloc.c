#include "deck_alloc.h"
#include "deck_intern.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "deck_alloc";

/* Sentinel refcount meaning "immortal — never retain/release". Using
 * 0 keeps the fast path simple: a newly-allocated value starts at 1. */
#define DECK_REFCOUNT_IMMORTAL   0u

static size_t                s_limit    = 0;
static size_t                s_used     = 0;
static size_t                s_peak     = 0;
static size_t                s_live     = 0;
static deck_alloc_panic_cb_t s_panic    = NULL;

/* --- immortal singletons ------------------------------------------ */

static deck_value_t s_unit  = { .type = DECK_T_UNIT,  .refcount = DECK_REFCOUNT_IMMORTAL };
static deck_value_t s_true  = { .type = DECK_T_BOOL,  .refcount = DECK_REFCOUNT_IMMORTAL, .as.b = true };
static deck_value_t s_false = { .type = DECK_T_BOOL,  .refcount = DECK_REFCOUNT_IMMORTAL, .as.b = false };

deck_value_t *deck_unit(void)  { return &s_unit; }
deck_value_t *deck_true(void)  { return &s_true; }
deck_value_t *deck_false(void) { return &s_false; }

void deck_alloc_init(size_t limit_bytes, deck_alloc_panic_cb_t panic_cb)
{
    s_limit = limit_bytes;
    s_panic = panic_cb;
    s_used  = 0;
    s_peak  = 0;
    s_live  = 0;
    ESP_LOGI(TAG, "init: limit=%u bytes", (unsigned)limit_bytes);
}

size_t deck_alloc_used(void)         { return s_used; }
size_t deck_alloc_peak(void)         { return s_peak; }
size_t deck_alloc_limit(void)        { return s_limit; }

void deck_alloc_set_limit(size_t limit_bytes)
{
    s_limit = limit_bytes;
}
size_t deck_alloc_live_values(void)  { return s_live; }

/* --- internal alloc helpers --------------------------------------- */

static void track(size_t added)
{
    s_used += added;
    if (s_used > s_peak) s_peak = s_used;
}
static void untrack(size_t removed)
{
    if (removed > s_used) s_used = 0;
    else                  s_used -= removed;
}

static void *alloc_bytes(size_t n)
{
    if (s_limit && s_used + n > s_limit) {
        if (s_panic) s_panic(DECK_RT_NO_MEMORY, "deck heap hard limit exceeded");
        return NULL;
    }
    void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL);
    if (!p) {
        if (s_panic) s_panic(DECK_RT_NO_MEMORY, "heap_caps_malloc failed");
        return NULL;
    }
    track(n);
    return p;
}

static void free_bytes(void *p, size_t n)
{
    if (!p) return;
    heap_caps_free(p);
    untrack(n);
}

static deck_value_t *alloc_value(deck_type_t t)
{
    deck_value_t *v = alloc_bytes(sizeof(deck_value_t));
    if (!v) return NULL;
    memset(v, 0, sizeof(*v));
    v->type = t;
    v->refcount = 1;
    s_live++;
    return v;
}

static void free_value(deck_value_t *v)
{
    if (!v) return;
    s_live--;
    free_bytes(v, sizeof(*v));
}

/* --- primitive constructors --------------------------------------- */

deck_value_t *deck_new_bool(bool v)    { return v ? deck_true() : deck_false(); }

deck_value_t *deck_new_int(int64_t v)
{
    deck_value_t *val = alloc_value(DECK_T_INT);
    if (val) val->as.i = v;
    return val;
}

deck_value_t *deck_new_float(double v)
{
    deck_value_t *val = alloc_value(DECK_T_FLOAT);
    if (val) val->as.f = v;
    return val;
}

deck_value_t *deck_new_atom(const char *name)
{
    if (!name) return NULL;
    const char *interned = deck_intern_cstr(name);
    if (!interned) return NULL;
    deck_value_t *val = alloc_value(DECK_T_ATOM);
    if (val) val->as.atom = interned;
    return val;
}

deck_value_t *deck_new_str(const char *s, uint32_t len)
{
    const char *interned = deck_intern(s, len);
    if (!interned) return NULL;
    deck_value_t *val = alloc_value(DECK_T_STR);
    if (!val) return NULL;
    val->as.s.ptr = interned;
    val->as.s.len = len;
    return val;
}

deck_value_t *deck_new_str_cstr(const char *s)
{
    return deck_new_str(s, s ? (uint32_t)strlen(s) : 0);
}

deck_value_t *deck_new_bytes(const uint8_t *buf, uint32_t len)
{
    deck_value_t *val = alloc_value(DECK_T_BYTES);
    if (!val) return NULL;
    if (len > 0) {
        val->as.bytes.buf = alloc_bytes(len);
        if (!val->as.bytes.buf) { free_value(val); return NULL; }
        if (buf) memcpy(val->as.bytes.buf, buf, len);
        else     memset(val->as.bytes.buf, 0, len);
    }
    val->as.bytes.len = len;
    return val;
}

/* --- composites --------------------------------------------------- */

deck_value_t *deck_new_list(uint32_t cap)
{
    deck_value_t *val = alloc_value(DECK_T_LIST);
    if (!val) return NULL;
    val->as.list.len = 0;
    val->as.list.cap = cap;
    if (cap > 0) {
        val->as.list.items = alloc_bytes(cap * sizeof(deck_value_t *));
        if (!val->as.list.items) { free_value(val); return NULL; }
    }
    return val;
}

deck_err_t deck_list_push(deck_value_t *list, deck_value_t *item)
{
    if (!list || list->type != DECK_T_LIST || !item) return DECK_RT_TYPE_MISMATCH;
    if (list->as.list.len >= list->as.list.cap) {
        uint32_t new_cap = list->as.list.cap ? list->as.list.cap * 2 : 4;
        deck_value_t **new_items = alloc_bytes(new_cap * sizeof(deck_value_t *));
        if (!new_items) return DECK_RT_NO_MEMORY;
        if (list->as.list.items) {
            memcpy(new_items, list->as.list.items,
                   list->as.list.len * sizeof(deck_value_t *));
            free_bytes(list->as.list.items,
                       list->as.list.cap * sizeof(deck_value_t *));
        }
        list->as.list.items = new_items;
        list->as.list.cap   = new_cap;
    }
    list->as.list.items[list->as.list.len++] = deck_retain(item);
    return DECK_RT_OK;
}

deck_value_t *deck_new_tuple(deck_value_t **items, uint32_t arity)
{
    if (arity == 0) return NULL;
    deck_value_t *val = alloc_value(DECK_T_TUPLE);
    if (!val) return NULL;
    val->as.tuple.items = alloc_bytes(arity * sizeof(deck_value_t *));
    if (!val->as.tuple.items) { free_value(val); return NULL; }
    val->as.tuple.arity = arity;
    for (uint32_t i = 0; i < arity; i++) {
        val->as.tuple.items[i] = deck_retain(items[i]);
    }
    return val;
}

deck_value_t *deck_new_none(void)
{
    deck_value_t *val = alloc_value(DECK_T_OPTIONAL);
    if (val) val->as.opt.inner = NULL;
    return val;
}

/* --- map (DL2 F21.6) --------------------------------------------- */

static bool map_keys_equal(const deck_value_t *a, const deck_value_t *b)
{
    if (!a || !b || a->type != b->type) return false;
    switch (a->type) {
        case DECK_T_INT:   return a->as.i == b->as.i;
        case DECK_T_STR:   return a->as.s.ptr == b->as.s.ptr;     /* interned */
        case DECK_T_ATOM:  return a->as.atom  == b->as.atom;       /* interned */
        case DECK_T_BOOL:  return a->as.b == b->as.b;
        default:           return false;
    }
}

deck_value_t *deck_new_map(uint32_t initial_cap)
{
    deck_value_t *val = alloc_value(DECK_T_MAP);
    if (!val) return NULL;
    val->as.map.len = 0;
    val->as.map.cap = initial_cap;
    if (initial_cap > 0) {
        val->as.map.entries = alloc_bytes(initial_cap * sizeof(deck_map_entry_t));
        if (!val->as.map.entries) { free_value(val); return NULL; }
        memset(val->as.map.entries, 0, initial_cap * sizeof(deck_map_entry_t));
    }
    return val;
}

deck_err_t deck_map_put(deck_value_t *m, deck_value_t *key, deck_value_t *val)
{
    if (!m || m->type != DECK_T_MAP || !key) return DECK_RT_TYPE_MISMATCH;
    /* Update existing key if present. */
    for (uint32_t i = 0; i < m->as.map.len; i++) {
        if (m->as.map.entries[i].used &&
            map_keys_equal(m->as.map.entries[i].key, key)) {
            deck_release(m->as.map.entries[i].val);
            m->as.map.entries[i].val = deck_retain(val);
            return DECK_RT_OK;
        }
    }
    /* Grow if needed. */
    if (m->as.map.len >= m->as.map.cap) {
        uint32_t new_cap = m->as.map.cap ? m->as.map.cap * 2 : 4;
        deck_map_entry_t *ne = alloc_bytes(new_cap * sizeof(deck_map_entry_t));
        if (!ne) return DECK_RT_NO_MEMORY;
        memset(ne, 0, new_cap * sizeof(deck_map_entry_t));
        if (m->as.map.entries) {
            memcpy(ne, m->as.map.entries, m->as.map.len * sizeof(deck_map_entry_t));
            free_bytes(m->as.map.entries, m->as.map.cap * sizeof(deck_map_entry_t));
        }
        m->as.map.entries = ne;
        m->as.map.cap = new_cap;
    }
    deck_map_entry_t *e = &m->as.map.entries[m->as.map.len++];
    e->key  = deck_retain(key);
    e->val  = deck_retain(val);
    e->used = true;
    return DECK_RT_OK;
}

deck_value_t *deck_map_get(deck_value_t *m, deck_value_t *key)
{
    if (!m || m->type != DECK_T_MAP || !key) return NULL;
    for (uint32_t i = 0; i < m->as.map.len; i++) {
        if (m->as.map.entries[i].used &&
            map_keys_equal(m->as.map.entries[i].key, key))
            return m->as.map.entries[i].val;
    }
    return NULL;
}

deck_value_t *deck_new_some(deck_value_t *inner)
{
    if (!inner) return NULL;
    deck_value_t *val = alloc_value(DECK_T_OPTIONAL);
    if (!val) return NULL;
    val->as.opt.inner = deck_retain(inner);
    return val;
}

/* Forward decls — env retain/release live in deck_interp.c, but the
 * fn value owns a refcount on its closure env so it must be able to
 * call those from here. */
struct deck_env *deck_env_retain(struct deck_env *e);
void             deck_env_release(struct deck_env *e);

deck_value_t *deck_new_fn(const char *name,
                          const char **params,
                          uint32_t n_params,
                          const struct ast_node *body,
                          struct deck_env *closure)
{
    deck_value_t *val = alloc_value(DECK_T_FN);
    if (!val) return NULL;
    val->as.fn.name     = name;
    val->as.fn.params   = params;
    val->as.fn.n_params = n_params;
    val->as.fn.body     = body;
    val->as.fn.closure  = deck_env_retain(closure);
    return val;
}

/* --- refcount ----------------------------------------------------- */

deck_value_t *deck_retain(deck_value_t *v)
{
    if (!v || v->refcount == DECK_REFCOUNT_IMMORTAL) return v;
    v->refcount++;
    return v;
}

static void release_children(deck_value_t *v)
{
    switch (v->type) {
        case DECK_T_BYTES:
            if (v->as.bytes.buf) {
                free_bytes(v->as.bytes.buf, v->as.bytes.len);
                v->as.bytes.buf = NULL;
            }
            break;
        case DECK_T_LIST:
            for (uint32_t i = 0; i < v->as.list.len; i++)
                deck_release(v->as.list.items[i]);
            if (v->as.list.items)
                free_bytes(v->as.list.items,
                           v->as.list.cap * sizeof(deck_value_t *));
            break;
        case DECK_T_TUPLE:
            for (uint32_t i = 0; i < v->as.tuple.arity; i++)
                deck_release(v->as.tuple.items[i]);
            if (v->as.tuple.items)
                free_bytes(v->as.tuple.items,
                           v->as.tuple.arity * sizeof(deck_value_t *));
            break;
        case DECK_T_OPTIONAL:
            if (v->as.opt.inner) deck_release(v->as.opt.inner);
            break;
        case DECK_T_MAP:
            for (uint32_t i = 0; i < v->as.map.len; i++) {
                if (v->as.map.entries[i].used) {
                    deck_release(v->as.map.entries[i].key);
                    deck_release(v->as.map.entries[i].val);
                }
            }
            if (v->as.map.entries)
                free_bytes(v->as.map.entries,
                           v->as.map.cap * sizeof(deck_map_entry_t));
            break;
        case DECK_T_FN:
            deck_env_release(v->as.fn.closure);
            v->as.fn.closure = NULL;
            break;
        default: break; /* primitives + interned atoms/strs have no children */
    }
}

void deck_release(deck_value_t *v)
{
    if (!v || v->refcount == DECK_REFCOUNT_IMMORTAL) return;
    if (--v->refcount == 0) {
        release_children(v);
        free_value(v);
    }
}
