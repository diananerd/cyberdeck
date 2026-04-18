#include "deck_interp.h"
#include "deck_alloc.h"
#include "deck_intern.h"
#include "deck_loader.h"
#include "drivers/deck_sdi_time.h"
#include "drivers/deck_sdi_info.h"
#include "drivers/deck_sdi_nvs.h"
#include "drivers/deck_sdi_fs.h"
#include "drivers/deck_sdi_shell.h"

#include "esp_log.h"

#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

static const char *TAG = "deck_interp";

/* ================================================================
 * Environment
 * ================================================================ */

typedef struct { const char *name; deck_value_t *val; } binding_t;

/* DL2 F21.2: refcounted environment.
 *
 * The struct lives in the per-run arena (so the storage is bulk-freed
 * at arena reset), but the bindings retain heap-allocated values whose
 * lifetimes outlive the immediate scope when captured by closures.
 *
 * Refcount semantics:
 *   - new returns ref=1; retains parent so the chain stays valid
 *   - retain bumps; release decrements
 *   - on release-to-zero we clear bindings (deck_release each value)
 *     and release the parent — `tearing_down` guards re-entry from a
 *     binding whose value (a fn) holds a back-pointer to this env */
struct deck_env {
    deck_env_t *parent;
    binding_t  *bindings;
    uint32_t    count;
    uint32_t    cap;
    uint32_t    refcount;
    bool        tearing_down;
};

deck_env_t *deck_env_new(deck_arena_t *a, deck_env_t *parent)
{
    deck_env_t *e = deck_arena_zalloc(a, sizeof(*e));
    if (!e) return NULL;
    e->parent   = parent;
    e->refcount = 1;
    if (parent) parent->refcount++;
    return e;
}

deck_env_t *deck_env_retain(deck_env_t *e)
{
    if (e && !e->tearing_down) e->refcount++;
    return e;
}

void deck_env_release(deck_env_t *e)
{
    if (!e || e->tearing_down) return;
    if (e->refcount == 0) return;       /* already torn down */
    if (--e->refcount > 0) return;
    e->tearing_down = true;
    for (uint32_t i = 0; i < e->count; i++) {
        deck_release(e->bindings[i].val);
        e->bindings[i].val  = NULL;
        e->bindings[i].name = NULL;
    }
    e->count = 0;
    deck_env_t *p = e->parent;
    e->parent = NULL;
    e->tearing_down = false;
    deck_env_release(p);
}

bool deck_env_bind(deck_arena_t *a, deck_env_t *e,
                   const char *name, deck_value_t *val)
{
    if (!e) return false;
    if (e->count >= e->cap) {
        uint32_t new_cap = e->cap ? e->cap * 2 : 4;
        binding_t *nb = deck_arena_alloc(a, new_cap * sizeof(binding_t));
        if (!nb) return false;
        if (e->bindings) memcpy(nb, e->bindings, e->count * sizeof(binding_t));
        e->bindings = nb; e->cap = new_cap;
    }
    e->bindings[e->count].name = name;
    e->bindings[e->count].val  = deck_retain(val);
    e->count++;
    return true;
}

deck_value_t *deck_env_lookup(deck_env_t *e, const char *name)
{
    for (deck_env_t *cur = e; cur; cur = cur->parent) {
        for (int32_t i = (int32_t)cur->count - 1; i >= 0; i--) {
            if (cur->bindings[i].name == name ||
                (cur->bindings[i].name && name &&
                 strcmp(cur->bindings[i].name, name) == 0))
                return cur->bindings[i].val;
        }
    }
    return NULL;
}


/* ================================================================
 * Error reporting
 * ================================================================ */

static void set_err(deck_interp_ctx_t *c, deck_err_t code,
                    uint32_t ln, uint32_t co, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

static void set_err(deck_interp_ctx_t *c, deck_err_t code,
                    uint32_t ln, uint32_t co, const char *fmt, ...)
{
    if (c->err != DECK_RT_OK) return;
    c->err = code;
    c->err_line = ln; c->err_col = co;
    va_list ap; va_start(ap, fmt);
    vsnprintf(c->err_msg, sizeof(c->err_msg), fmt, ap);
    va_end(ap);
}

/* ================================================================
 * Builtins — "cap.method" → fn
 * ================================================================ */

typedef deck_value_t *(*builtin_fn_t)(deck_value_t **args, uint32_t n,
                                       deck_interp_ctx_t *c);

typedef struct {
    const char *name;
    builtin_fn_t fn;
    int min_arity, max_arity;
} builtin_t;

static deck_value_t *b_log_info(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    if (args[0] && args[0]->type == DECK_T_STR)
        ESP_LOGI("deck.app", "%.*s", (int)args[0]->as.s.len, args[0]->as.s.ptr);
    else if (args[0] && args[0]->type == DECK_T_ATOM)
        ESP_LOGI("deck.app", ":%s", args[0]->as.atom);
    else if (args[0] && args[0]->type == DECK_T_INT)
        ESP_LOGI("deck.app", "%lld", (long long)args[0]->as.i);
    else
        ESP_LOGI("deck.app", "(non-str value)");
    return deck_retain(deck_unit());
}
static deck_value_t *b_log_warn(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    if (args[0] && args[0]->type == DECK_T_STR)
        ESP_LOGW("deck.app", "%.*s", (int)args[0]->as.s.len, args[0]->as.s.ptr);
    return deck_retain(deck_unit());
}
static deck_value_t *b_log_error(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    if (args[0] && args[0]->type == DECK_T_STR)
        ESP_LOGE("deck.app", "%.*s", (int)args[0]->as.s.len, args[0]->as.s.ptr);
    return deck_retain(deck_unit());
}
static deck_value_t *b_time_now(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{ (void)a; (void)n; (void)c; return deck_new_int(deck_sdi_time_monotonic_us() / 1000); }
static deck_value_t *b_info_device_id(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{ (void)a; (void)n; (void)c; const char *d = deck_sdi_info_device_id(); return deck_new_str_cstr(d ? d : ""); }
static deck_value_t *b_info_free_heap(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{ (void)a; (void)n; (void)c; return deck_new_int((int64_t)deck_sdi_info_free_heap()); }
static deck_value_t *b_info_deck_level(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{ (void)a; (void)n; (void)c; return deck_new_int(deck_sdi_info_deck_level()); }
static deck_value_t *b_text_upper(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.upper expects str");
        return NULL;
    }
    char buf[256]; uint32_t L = args[0]->as.s.len < 255 ? args[0]->as.s.len : 255;
    for (uint32_t i = 0; i < L; i++) {
        char ch = args[0]->as.s.ptr[i];
        buf[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    }
    return deck_new_str(buf, L);
}

/* ---- math.* ---- */
static double numf(deck_value_t *v) { return v->type == DECK_T_INT ? (double)v->as.i : v->as.f; }
static bool   is_num(deck_value_t *v) { return v && (v->type == DECK_T_INT || v->type == DECK_T_FLOAT); }

static deck_value_t *b_math_abs(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!is_num(args[0])) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "math.abs needs number"); return NULL; }
    if (args[0]->type == DECK_T_INT) {
        int64_t x = args[0]->as.i; return deck_new_int(x < 0 ? -x : x);
    }
    double x = args[0]->as.f; return deck_new_float(x < 0 ? -x : x);
}
static deck_value_t *b_math_min(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!is_num(args[0]) || !is_num(args[1])) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "math.min needs numbers"); return NULL; }
    if (args[0]->type == DECK_T_INT && args[1]->type == DECK_T_INT)
        return deck_new_int(args[0]->as.i < args[1]->as.i ? args[0]->as.i : args[1]->as.i);
    double a = numf(args[0]), b = numf(args[1]);
    return deck_new_float(a < b ? a : b);
}
static deck_value_t *b_math_max(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!is_num(args[0]) || !is_num(args[1])) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "math.max needs numbers"); return NULL; }
    if (args[0]->type == DECK_T_INT && args[1]->type == DECK_T_INT)
        return deck_new_int(args[0]->as.i > args[1]->as.i ? args[0]->as.i : args[1]->as.i);
    double a = numf(args[0]), b = numf(args[1]);
    return deck_new_float(a > b ? a : b);
}
static deck_value_t *b_math_floor(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!is_num(args[0])) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "math.floor needs number"); return NULL; }
    if (args[0]->type == DECK_T_INT) return deck_new_int(args[0]->as.i);
    return deck_new_int((int64_t)floor(args[0]->as.f));
}
static deck_value_t *b_math_ceil(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!is_num(args[0])) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "math.ceil needs number"); return NULL; }
    if (args[0]->type == DECK_T_INT) return deck_new_int(args[0]->as.i);
    return deck_new_int((int64_t)ceil(args[0]->as.f));
}
static deck_value_t *b_math_round(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!is_num(args[0])) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "math.round needs number"); return NULL; }
    if (args[0]->type == DECK_T_INT) return deck_new_int(args[0]->as.i);
    return deck_new_int((int64_t)round(args[0]->as.f));
}

/* ---- text.* ---- */
static deck_value_t *b_text_lower(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.lower expects str"); return NULL; }
    char buf[256]; uint32_t L = args[0]->as.s.len < 255 ? args[0]->as.s.len : 255;
    for (uint32_t i = 0; i < L; i++) {
        char ch = args[0]->as.s.ptr[i];
        buf[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
    }
    return deck_new_str(buf, L);
}
static deck_value_t *b_text_len(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.len expects str"); return NULL; }
    return deck_new_int((int64_t)args[0]->as.s.len);
}
static deck_value_t *b_text_starts_with(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR ||
        !args[1] || args[1]->type != DECK_T_STR) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.starts_with expects (str, str)"); return NULL; }
    if (args[1]->as.s.len > args[0]->as.s.len) return deck_retain(deck_false());
    return deck_retain(memcmp(args[0]->as.s.ptr, args[1]->as.s.ptr, args[1]->as.s.len) == 0 ? deck_true() : deck_false());
}
static deck_value_t *b_text_ends_with(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR ||
        !args[1] || args[1]->type != DECK_T_STR) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.ends_with expects (str, str)"); return NULL; }
    if (args[1]->as.s.len > args[0]->as.s.len) return deck_retain(deck_false());
    const char *tail = args[0]->as.s.ptr + args[0]->as.s.len - args[1]->as.s.len;
    return deck_retain(memcmp(tail, args[1]->as.s.ptr, args[1]->as.s.len) == 0 ? deck_true() : deck_false());
}
static deck_value_t *b_text_contains(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR ||
        !args[1] || args[1]->type != DECK_T_STR) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.contains expects (str, str)"); return NULL; }
    if (args[1]->as.s.len == 0) return deck_retain(deck_true());
    if (args[1]->as.s.len > args[0]->as.s.len) return deck_retain(deck_false());
    uint32_t limit = args[0]->as.s.len - args[1]->as.s.len;
    for (uint32_t i = 0; i <= limit; i++) {
        if (memcmp(args[0]->as.s.ptr + i, args[1]->as.s.ptr, args[1]->as.s.len) == 0)
            return deck_retain(deck_true());
    }
    return deck_retain(deck_false());
}

/* DL2 F22 — call a fn value with N args from C. Used by stdlib higher-
 * order helpers (list.map, list.filter, list.reduce, map_ok, and_then). */
static deck_value_t *call_fn_value_c(deck_interp_ctx_t *c,
                                     deck_value_t *fnv,
                                     deck_value_t **argv, uint32_t argc)
{
    if (!fnv || fnv->type != DECK_T_FN) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "expected function");
        return NULL;
    }
    if (argc != fnv->as.fn.n_params) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0,
                "fn '%s' takes %u args, got %u",
                fnv->as.fn.name ? fnv->as.fn.name : "<anon>",
                (unsigned)fnv->as.fn.n_params, (unsigned)argc);
        return NULL;
    }
    deck_env_t *call_env = deck_env_new(c->arena, fnv->as.fn.closure);
    if (!call_env) return NULL;
    for (uint32_t i = 0; i < argc; i++)
        deck_env_bind(c->arena, call_env, fnv->as.fn.params[i], argv[i]);
    deck_value_t *result = deck_interp_run(c, call_env, fnv->as.fn.body);
    deck_env_release(call_env);
    return result;
}

/* ---- list.* ---- */
static deck_value_t *b_list_len(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.len expects list"); return NULL;
    }
    return deck_new_int((int64_t)args[0]->as.list.len);
}
static deck_value_t *b_list_head(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.head expects list"); return NULL;
    }
    if (args[0]->as.list.len == 0) return deck_new_none();
    return deck_new_some(args[0]->as.list.items[0]);
}
static deck_value_t *b_list_get(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST ||
        !args[1] || args[1]->type != DECK_T_INT) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.get(list, int)"); return NULL;
    }
    int64_t i = args[1]->as.i;
    if (i < 0 || i >= (int64_t)args[0]->as.list.len) return deck_new_none();
    return deck_new_some(args[0]->as.list.items[(uint32_t)i]);
}
static deck_value_t *b_list_tail(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.tail expects list"); return NULL;
    }
    uint32_t len = args[0]->as.list.len;
    if (len == 0) return deck_new_list(0);
    deck_value_t *out = deck_new_list(len - 1);
    if (!out) return NULL;
    for (uint32_t i = 1; i < len; i++)
        deck_list_push(out, args[0]->as.list.items[i]);
    return out;
}
static deck_value_t *b_list_map(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST ||
        !args[1] || args[1]->type != DECK_T_FN) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.map(list, fn)"); return NULL;
    }
    deck_value_t *out = deck_new_list(args[0]->as.list.len);
    if (!out) return NULL;
    for (uint32_t i = 0; i < args[0]->as.list.len; i++) {
        deck_value_t *callargs[1] = { args[0]->as.list.items[i] };
        deck_value_t *r = call_fn_value_c(c, args[1], callargs, 1);
        if (!r) { deck_release(out); return NULL; }
        deck_list_push(out, r);
        deck_release(r);
    }
    return out;
}
static deck_value_t *b_list_filter(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST ||
        !args[1] || args[1]->type != DECK_T_FN) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.filter(list, pred)"); return NULL;
    }
    deck_value_t *out = deck_new_list(args[0]->as.list.len);
    if (!out) return NULL;
    for (uint32_t i = 0; i < args[0]->as.list.len; i++) {
        deck_value_t *callargs[1] = { args[0]->as.list.items[i] };
        deck_value_t *r = call_fn_value_c(c, args[1], callargs, 1);
        if (!r) { deck_release(out); return NULL; }
        bool keep = deck_is_truthy(r);
        deck_release(r);
        if (keep) deck_list_push(out, args[0]->as.list.items[i]);
    }
    return out;
}
static deck_value_t *b_list_reduce(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST ||
        !args[1] || args[1]->type != DECK_T_FN || !args[2]) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.reduce(list, fn, init)"); return NULL;
    }
    deck_value_t *acc = deck_retain(args[2]);
    for (uint32_t i = 0; i < args[0]->as.list.len; i++) {
        deck_value_t *callargs[2] = { acc, args[0]->as.list.items[i] };
        deck_value_t *r = call_fn_value_c(c, args[1], callargs, 2);
        deck_release(acc);
        if (!r) return NULL;
        acc = r;
    }
    return acc;
}

/* ---- map.* (DL2 F21.6) ---- */
static deck_value_t *b_map_len(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_MAP) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "map.len expects map"); return NULL;
    }
    return deck_new_int((int64_t)args[0]->as.map.len);
}
static deck_value_t *b_map_get_b(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_MAP) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "map.get(map, key)"); return NULL;
    }
    deck_value_t *v = deck_map_get(args[0], args[1]);
    if (!v) return deck_new_none();
    return deck_new_some(v);
}
static deck_value_t *b_map_put_b(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_MAP) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "map.put(map, key, val)"); return NULL;
    }
    /* Immutable update: copy the map then put. Small maps make this cheap;
     * F22 may add a real persistent structure. */
    deck_value_t *m = args[0];
    deck_value_t *out = deck_new_map(m->as.map.cap > 0 ? m->as.map.cap : 4);
    if (!out) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "map.put alloc"); return NULL; }
    for (uint32_t i = 0; i < m->as.map.len; i++) {
        if (m->as.map.entries[i].used)
            deck_map_put(out, m->as.map.entries[i].key, m->as.map.entries[i].val);
    }
    deck_map_put(out, args[1], args[2]);
    return out;
}
static deck_value_t *b_map_keys(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_MAP) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "map.keys(map)"); return NULL;
    }
    deck_value_t *out = deck_new_list(args[0]->as.map.len);
    if (!out) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "map.keys alloc"); return NULL; }
    for (uint32_t i = 0; i < args[0]->as.map.len; i++)
        if (args[0]->as.map.entries[i].used)
            deck_list_push(out, args[0]->as.map.entries[i].key);
    return out;
}
static deck_value_t *b_map_values(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_MAP) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "map.values(map)"); return NULL;
    }
    deck_value_t *out = deck_new_list(args[0]->as.map.len);
    if (!out) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "map.values alloc"); return NULL; }
    for (uint32_t i = 0; i < args[0]->as.map.len; i++)
        if (args[0]->as.map.entries[i].used)
            deck_list_push(out, args[0]->as.map.entries[i].val);
    return out;
}

/* ---- bytes.* ---- */
static deck_value_t *b_bytes_len(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_BYTES) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "bytes.len expects bytes"); return NULL; }
    return deck_new_int((int64_t)args[0]->as.bytes.len);
}

/* ---- time.* ---- */
static deck_value_t *b_time_duration(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!is_num(args[0]) || !is_num(args[1])) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "time.duration needs numbers"); return NULL; }
    int64_t a = args[0]->type == DECK_T_INT ? args[0]->as.i : (int64_t)args[0]->as.f;
    int64_t b = args[1]->type == DECK_T_INT ? args[1]->as.i : (int64_t)args[1]->as.f;
    return deck_new_int(a - b);
}
static deck_value_t *b_time_to_iso(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!is_num(args[0])) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "time.to_iso needs epoch seconds"); return NULL; }
    int64_t epoch_s = args[0]->type == DECK_T_INT ? args[0]->as.i : (int64_t)args[0]->as.f;
    time_t t = (time_t)epoch_s;
    struct tm tm; gmtime_r(&t, &tm);
    char buf[32];
    int k = snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
    return deck_new_str(buf, (uint32_t)k);
}

/* ---- nvs.* ---- */
static deck_value_t *b_nvs_get(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR ||
        !args[1] || args[1]->type != DECK_T_STR) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "nvs.get(ns, key)"); return NULL; }
    char out[128]; memcpy(out, args[0]->as.s.ptr, args[0]->as.s.len); out[args[0]->as.s.len] = 0;
    char key[64]; memcpy(key, args[1]->as.s.ptr, args[1]->as.s.len); key[args[1]->as.s.len] = 0;
    char val[128]; val[0] = 0;
    deck_sdi_err_t rc = deck_sdi_nvs_get_str(out, key, val, sizeof(val));
    if (rc == DECK_SDI_OK) return deck_new_str_cstr(val);
    if (rc == DECK_SDI_ERR_NOT_FOUND) return deck_new_none();
    set_err(c, DECK_RT_INTERNAL, 0, 0, "nvs.get failed: %s", deck_sdi_strerror(rc));
    return NULL;
}
static deck_value_t *b_nvs_set(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR ||
        !args[1] || args[1]->type != DECK_T_STR ||
        !args[2] || args[2]->type != DECK_T_STR) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "nvs.set(ns, key, str)"); return NULL; }
    char ns[64]; memcpy(ns, args[0]->as.s.ptr, args[0]->as.s.len); ns[args[0]->as.s.len] = 0;
    char key[64]; memcpy(key, args[1]->as.s.ptr, args[1]->as.s.len); key[args[1]->as.s.len] = 0;
    char val[128]; memcpy(val, args[2]->as.s.ptr, args[2]->as.s.len); val[args[2]->as.s.len] = 0;
    deck_sdi_err_t rc = deck_sdi_nvs_set_str(ns, key, val);
    if (rc != DECK_SDI_OK) { set_err(c, DECK_RT_INTERNAL, 0, 0, "nvs.set failed: %s", deck_sdi_strerror(rc)); return NULL; }
    return deck_retain(deck_unit());
}
static deck_value_t *b_nvs_delete(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR ||
        !args[1] || args[1]->type != DECK_T_STR) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "nvs.delete(ns, key)"); return NULL; }
    char ns[64]; memcpy(ns, args[0]->as.s.ptr, args[0]->as.s.len); ns[args[0]->as.s.len] = 0;
    char key[64]; memcpy(key, args[1]->as.s.ptr, args[1]->as.s.len); key[args[1]->as.s.len] = 0;
    deck_sdi_nvs_del(ns, key);
    return deck_retain(deck_unit());
}

/* ---- fs.* ---- */
static deck_value_t *b_fs_exists(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    if (!args[0] || args[0]->type != DECK_T_STR) return deck_retain(deck_false());
    char path[128]; memcpy(path, args[0]->as.s.ptr, args[0]->as.s.len); path[args[0]->as.s.len] = 0;
    deck_sdi_err_t rc = deck_sdi_fs_exists(path, NULL);
    return deck_retain(rc == DECK_SDI_OK ? deck_true() : deck_false());
}
static deck_value_t *b_fs_read(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "fs.read(path)"); return NULL; }
    char path[128]; memcpy(path, args[0]->as.s.ptr, args[0]->as.s.len); path[args[0]->as.s.len] = 0;
    char buf[512]; size_t sz = sizeof(buf);
    deck_sdi_err_t rc = deck_sdi_fs_read(path, buf, &sz);
    if (rc == DECK_SDI_ERR_NOT_FOUND) return deck_new_none();
    if (rc != DECK_SDI_OK) { set_err(c, DECK_RT_INTERNAL, 0, 0, "fs.read failed: %s", deck_sdi_strerror(rc)); return NULL; }
    deck_value_t *inner = deck_new_str(buf, (uint32_t)sz);
    if (!inner) return NULL;
    deck_value_t *some = deck_new_some(inner);
    deck_release(inner);
    return some;
}

/* fs.list — returns entries separated by '\n' as a single string.
 * DL1 lacks list literal syntax, so a newline-joined string is the
 * pragmatic representation: callable via text.contains / text.len.
 * DL2 will replace this with a real list<str> value once list syntax
 * lands. */
#define FS_LIST_BUF 4096
typedef struct {
    char   *buf;
    size_t  cap;
    size_t  len;
    bool    overflow;
} fs_list_ctx_t;

static bool fs_list_cb(const char *name, bool is_dir, void *user)
{
    (void)is_dir;
    fs_list_ctx_t *lc = user;
    if (!name) return true;
    size_t need = strlen(name) + (lc->len > 0 ? 1 : 0);
    if (lc->len + need >= lc->cap) { lc->overflow = true; return false; }
    if (lc->len > 0) lc->buf[lc->len++] = '\n';
    size_t nl = strlen(name);
    memcpy(lc->buf + lc->len, name, nl);
    lc->len += nl;
    lc->buf[lc->len] = '\0';
    return true;
}

static deck_value_t *b_fs_list(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "fs.list(path)");
        return NULL;
    }
    char path[128];
    memcpy(path, args[0]->as.s.ptr, args[0]->as.s.len);
    path[args[0]->as.s.len] = 0;

    static char        s_buf[FS_LIST_BUF];
    s_buf[0] = '\0';
    fs_list_ctx_t lc = { .buf = s_buf, .cap = FS_LIST_BUF, .len = 0, .overflow = false };
    deck_sdi_err_t rc = deck_sdi_fs_list(path, fs_list_cb, &lc);
    if (rc == DECK_SDI_ERR_NOT_FOUND) return deck_new_str_cstr("");
    if (rc != DECK_SDI_OK) {
        set_err(c, DECK_RT_INTERNAL, 0, 0,
                "fs.list failed: %s", deck_sdi_strerror(rc));
        return NULL;
    }
    if (lc.overflow) {
        set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0,
                "fs.list: buffer overflow (>%u bytes)", (unsigned)FS_LIST_BUF);
        return NULL;
    }
    return deck_new_str(s_buf, (uint32_t)lc.len);
}

/* ---- text.split / text.repeat (DL2) ---- */
static deck_value_t *b_text_split(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR ||
        !args[1] || args[1]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.split(str, sep_str)"); return NULL;
    }
    deck_value_t *out = deck_new_list(4);
    if (!out) return NULL;
    const char *src = args[0]->as.s.ptr;
    uint32_t    src_len = args[0]->as.s.len;
    const char *sep = args[1]->as.s.ptr;
    uint32_t    sep_len = args[1]->as.s.len;
    if (sep_len == 0) {
        /* Split into individual characters. */
        for (uint32_t i = 0; i < src_len; i++) {
            deck_value_t *ch = deck_new_str(src + i, 1);
            if (!ch) { deck_release(out); return NULL; }
            deck_list_push(out, ch);
            deck_release(ch);
        }
        return out;
    }
    uint32_t start = 0;
    for (uint32_t i = 0; i + sep_len <= src_len; ) {
        if (memcmp(src + i, sep, sep_len) == 0) {
            deck_value_t *frag = deck_new_str(src + start, i - start);
            deck_list_push(out, frag); deck_release(frag);
            i += sep_len;
            start = i;
        } else {
            i++;
        }
    }
    deck_value_t *last = deck_new_str(src + start, src_len - start);
    deck_list_push(out, last); deck_release(last);
    return out;
}
static deck_value_t *b_text_repeat(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR ||
        !args[1] || args[1]->type != DECK_T_INT) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.repeat(str, int)"); return NULL;
    }
    int64_t k = args[1]->as.i;
    if (k <= 0) return deck_new_str("", 0);
    uint32_t single = args[0]->as.s.len;
    uint64_t total = (uint64_t)single * (uint64_t)k;
    if (total > 1024) {
        set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "text.repeat: result > 1024 bytes");
        return NULL;
    }
    char buf[1024];
    uint32_t off = 0;
    for (int64_t i = 0; i < k; i++) {
        memcpy(buf + off, args[0]->as.s.ptr, single);
        off += single;
    }
    return deck_new_str(buf, off);
}

/* ---- os.sleep_ms (DL2) ---- */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static deck_value_t *b_os_sleep_ms(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_INT) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "os.sleep_ms(int)"); return NULL;
    }
    int64_t ms = args[0]->as.i;
    if (ms < 0) ms = 0;
    if (ms > 60000) ms = 60000;   /* clamp 1 minute max — apps shouldn't block longer */
    vTaskDelay(pdMS_TO_TICKS((uint32_t)ms));
    return deck_retain(deck_unit());
}
static deck_value_t *b_time_now_us(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{ (void)a; (void)n; (void)c; return deck_new_int(deck_sdi_time_monotonic_us()); }

/* ---- os.* lifecycle ---- */
static deck_value_t *b_os_resume(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)args; (void)n; (void)c;
    /* DL1 single-app: resume is a no-op from the app's POV. */
    return deck_retain(deck_unit());
}
static deck_value_t *b_os_suspend(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)args; (void)n; (void)c;
    /* DL1 single-app: suspend is a no-op. DL2 hands control back to shell. */
    return deck_retain(deck_unit());
}
static deck_value_t *b_os_terminate(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)args; (void)n; (void)c;
    (void)deck_sdi_shell_terminate();
    return deck_retain(deck_unit());
}

/* ---- type conversions (bare ident calls) ---- */
static deck_value_t *b_to_str(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    if (!args[0]) return deck_new_str_cstr("");
    char buf[64];
    switch (args[0]->type) {
        case DECK_T_STR:   return deck_retain(args[0]);
        case DECK_T_INT:   snprintf(buf, sizeof(buf), "%lld", (long long)args[0]->as.i); return deck_new_str_cstr(buf);
        case DECK_T_FLOAT: snprintf(buf, sizeof(buf), "%g", args[0]->as.f); return deck_new_str_cstr(buf);
        case DECK_T_BOOL:  return deck_new_str_cstr(args[0]->as.b ? "true" : "false");
        case DECK_T_ATOM:  snprintf(buf, sizeof(buf), ":%s", args[0]->as.atom); return deck_new_str_cstr(buf);
        case DECK_T_UNIT:  return deck_new_str_cstr("unit");
        default:           return deck_new_str_cstr("?");
    }
}
static deck_value_t *b_to_int(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0]) return deck_new_int(0);
    switch (args[0]->type) {
        case DECK_T_INT:   return deck_retain(args[0]);
        case DECK_T_FLOAT: return deck_new_int((int64_t)args[0]->as.f);
        case DECK_T_BOOL:  return deck_new_int(args[0]->as.b ? 1 : 0);
        case DECK_T_STR: {
            char buf[64]; uint32_t L = args[0]->as.s.len < 63 ? args[0]->as.s.len : 63;
            memcpy(buf, args[0]->as.s.ptr, L); buf[L] = 0;
            return deck_new_int(strtoll(buf, NULL, 10));
        }
        default: set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "int() unsupported type"); return NULL;
    }
}
static deck_value_t *b_to_float(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0]) return deck_new_float(0);
    switch (args[0]->type) {
        case DECK_T_INT:   return deck_new_float((double)args[0]->as.i);
        case DECK_T_FLOAT: return deck_retain(args[0]);
        case DECK_T_STR: {
            char buf[64]; uint32_t L = args[0]->as.s.len < 63 ? args[0]->as.s.len : 63;
            memcpy(buf, args[0]->as.s.ptr, L); buf[L] = 0;
            return deck_new_float(strtod(buf, NULL));
        }
        default: set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "float() unsupported type"); return NULL;
    }
}
static deck_value_t *b_to_bool(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    return deck_retain(deck_is_truthy(args[0]) ? deck_true() : deck_false());
}

static const builtin_t BUILTINS[] = {
    /* log */
    { "log.info",               b_log_info,          1, 1 },
    { "log.warn",               b_log_warn,          1, 1 },
    { "log.error",              b_log_error,         1, 1 },

    /* time */
    { "time.now",               b_time_now,          0, 0 },
    { "time.now_us",            b_time_now_us,       0, 0 },
    { "time.duration",          b_time_duration,     2, 2 },
    { "time.to_iso",            b_time_to_iso,       1, 1 },

    /* system.info */
    { "system.info.device_id",  b_info_device_id,    0, 0 },
    { "system.info.free_heap",  b_info_free_heap,    0, 0 },
    { "system.info.deck_level", b_info_deck_level,   0, 0 },

    /* text */
    { "text.upper",             b_text_upper,        1, 1 },
    { "text.lower",             b_text_lower,        1, 1 },
    { "text.len",               b_text_len,          1, 1 },
    { "text.starts_with",       b_text_starts_with,  2, 2 },
    { "text.ends_with",         b_text_ends_with,    2, 2 },
    { "text.contains",          b_text_contains,     2, 2 },
    { "text.split",             b_text_split,        2, 2 },
    { "text.repeat",            b_text_repeat,       2, 2 },

    /* list (DL2 F21.4 + F22 stdlib) */
    { "list.len",               b_list_len,          1, 1 },
    { "list.head",              b_list_head,         1, 1 },
    { "list.tail",              b_list_tail,         1, 1 },
    { "list.get",               b_list_get,          2, 2 },
    { "list.map",               b_list_map,          2, 2 },
    { "list.filter",            b_list_filter,       2, 2 },
    { "list.reduce",            b_list_reduce,       3, 3 },

    /* map (DL2 F21.6) */
    { "map.len",                b_map_len,           1, 1 },
    { "map.get",                b_map_get_b,         2, 2 },
    { "map.put",                b_map_put_b,         3, 3 },
    { "map.keys",               b_map_keys,          1, 1 },
    { "map.values",             b_map_values,        1, 1 },

    /* bytes */
    { "bytes.len",              b_bytes_len,         1, 1 },

    /* math */
    { "math.abs",               b_math_abs,          1, 1 },
    { "math.min",               b_math_min,          2, 2 },
    { "math.max",               b_math_max,          2, 2 },
    { "math.floor",             b_math_floor,        1, 1 },
    { "math.ceil",              b_math_ceil,         1, 1 },
    { "math.round",             b_math_round,        1, 1 },

    /* nvs */
    { "nvs.get",                b_nvs_get,           2, 2 },
    { "nvs.set",                b_nvs_set,           3, 3 },
    { "nvs.delete",             b_nvs_delete,        2, 2 },

    /* fs (read-only) */
    { "fs.exists",              b_fs_exists,         1, 1 },
    { "fs.read",                b_fs_read,           1, 1 },
    { "fs.list",                b_fs_list,           1, 1 },

    /* os lifecycle (single-app stubs in DL1) */
    { "os.resume",              b_os_resume,         0, 0 },
    { "os.suspend",             b_os_suspend,        0, 0 },
    { "os.terminate",           b_os_terminate,      0, 0 },
    { "os.sleep_ms",            b_os_sleep_ms,       1, 1 },

    { NULL, NULL, 0, 0 },
};

static deck_value_t *b_to_some(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    if (!args[0]) return deck_new_none();
    return deck_new_some(args[0]);
}

/* DL2 F22 — Result. Internally a tuple (atom, payload).
 *   ok(v)  → (:ok,  v)
 *   err(e) → (:err, e)
 * Pattern matching can destructure via tuple patterns once those land,
 * or callers can use is_ok/is_err/unwrap. */
static deck_value_t *make_result_tag(const char *tag, deck_value_t *payload)
{
    deck_value_t *atom_v = deck_new_atom(tag);
    if (!atom_v) return NULL;
    deck_value_t *items[2] = { atom_v, payload };
    deck_value_t *t = deck_new_tuple(items, 2);
    deck_release(atom_v);
    return t;
}
static deck_value_t *b_to_ok(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    return make_result_tag("ok", args[0]);
}
static deck_value_t *b_to_err(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    return make_result_tag("err", args[0]);
}
static bool result_tag_is(deck_value_t *v, const char *tag)
{
    if (!v || v->type != DECK_T_TUPLE || v->as.tuple.arity != 2) return false;
    deck_value_t *first = v->as.tuple.items[0];
    if (!first || first->type != DECK_T_ATOM) return false;
    return strcmp(first->as.atom, tag) == 0;
}
static deck_value_t *b_is_ok(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{ (void)n; (void)c; return deck_retain(result_tag_is(args[0], "ok") ? deck_true() : deck_false()); }
static deck_value_t *b_is_err(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{ (void)n; (void)c; return deck_retain(result_tag_is(args[0], "err") ? deck_true() : deck_false()); }
static deck_value_t *b_unwrap(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0]) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "unwrap: nil"); return NULL; }
    if (args[0]->type == DECK_T_OPTIONAL) {
        if (args[0]->as.opt.inner == NULL) {
            set_err(c, DECK_RT_PATTERN_FAILED, 0, 0, "unwrap on none");
            return NULL;
        }
        return deck_retain(args[0]->as.opt.inner);
    }
    if (result_tag_is(args[0], "ok"))  return deck_retain(args[0]->as.tuple.items[1]);
    if (result_tag_is(args[0], "err")) {
        set_err(c, DECK_RT_PATTERN_FAILED, 0, 0, "unwrap on err result");
        return NULL;
    }
    set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "unwrap: not optional or result");
    return NULL;
}
static deck_value_t *b_map_ok(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || !args[1] || args[1]->type != DECK_T_FN) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "map_ok(result, fn)"); return NULL;
    }
    if (result_tag_is(args[0], "ok")) {
        deck_value_t *callargs[1] = { args[0]->as.tuple.items[1] };
        deck_value_t *r = call_fn_value_c(c, args[1], callargs, 1);
        if (!r) return NULL;
        deck_value_t *out = make_result_tag("ok", r);
        deck_release(r);
        return out;
    }
    return deck_retain(args[0]);   /* err pass-through */
}
static deck_value_t *b_and_then(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || !args[1] || args[1]->type != DECK_T_FN) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "and_then(result, fn)"); return NULL;
    }
    if (result_tag_is(args[0], "ok")) {
        deck_value_t *callargs[1] = { args[0]->as.tuple.items[1] };
        return call_fn_value_c(c, args[1], callargs, 1);
    }
    return deck_retain(args[0]);
}

/* DL2 F22 — type inspection. */
static deck_value_t *b_type_of(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    return deck_new_atom(args[0] ? deck_type_name(args[0]->type) : "nil");
}
static deck_value_t *b_is_int(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{ (void)n; (void)c; return deck_retain(args[0] && args[0]->type == DECK_T_INT ? deck_true() : deck_false()); }
static deck_value_t *b_is_str(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{ (void)n; (void)c; return deck_retain(args[0] && args[0]->type == DECK_T_STR ? deck_true() : deck_false()); }
static deck_value_t *b_is_atom(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{ (void)n; (void)c; return deck_retain(args[0] && args[0]->type == DECK_T_ATOM ? deck_true() : deck_false()); }
static deck_value_t *b_is_list(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{ (void)n; (void)c; return deck_retain(args[0] && args[0]->type == DECK_T_LIST ? deck_true() : deck_false()); }
static deck_value_t *b_is_map(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{ (void)n; (void)c; return deck_retain(args[0] && args[0]->type == DECK_T_MAP ? deck_true() : deck_false()); }
static deck_value_t *b_is_fn(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{ (void)n; (void)c; return deck_retain(args[0] && args[0]->type == DECK_T_FN ? deck_true() : deck_false()); }

/* Bare-ident builtins: str(), int(), float(), bool() type conversions. */
static const builtin_t BARE_BUILTINS[] = {
    { "str",   b_to_str,   1, 1 },
    { "int",   b_to_int,   1, 1 },
    { "float", b_to_float, 1, 1 },
    { "bool",  b_to_bool,  1, 1 },
    { "some",  b_to_some,  1, 1 },   /* DL2 F21.9 — Optional constructor */

    /* DL2 F22 — Result constructors + helpers. */
    { "ok",       b_to_ok,    1, 1 },
    { "err",      b_to_err,   1, 1 },
    { "is_ok",    b_is_ok,    1, 1 },
    { "is_err",   b_is_err,   1, 1 },
    { "unwrap",   b_unwrap,   1, 1 },
    { "map_ok",   b_map_ok,   2, 2 },
    { "and_then", b_and_then, 2, 2 },

    /* DL2 F22 — type inspection. */
    { "type_of", b_type_of, 1, 1 },
    { "is_int",  b_is_int,  1, 1 },
    { "is_str",  b_is_str,  1, 1 },
    { "is_atom", b_is_atom, 1, 1 },
    { "is_list", b_is_list, 1, 1 },
    { "is_map",  b_is_map,  1, 1 },
    { "is_fn",   b_is_fn,   1, 1 },

    { NULL, NULL, 0, 0 },
};

static const builtin_t *find_bare_builtin(const char *name)
{
    for (const builtin_t *b = BARE_BUILTINS; b->name; b++)
        if (strcmp(b->name, name) == 0) return b;
    return NULL;
}

static const builtin_t *find_builtin(const char *name)
{
    for (const builtin_t *b = BUILTINS; b->name; b++)
        if (strcmp(b->name, name) == 0) return b;
    return NULL;
}

static bool build_cap_name(const ast_node_t *dot, char *out, size_t out_size)
{
    const ast_node_t *parts[8]; int n = 0;
    const ast_node_t *cur = dot;
    while (cur && cur->kind == AST_DOT && n < 7) { parts[n++] = cur; cur = cur->as.dot.obj; }
    if (!cur || cur->kind != AST_IDENT) return false;
    size_t off = (size_t)snprintf(out, out_size, "%s", cur->as.s);
    for (int i = n - 1; i >= 0; i--)
        off += (size_t)snprintf(out + off, out_size - off, ".%s", parts[i]->as.dot.field);
    return true;
}

/* ================================================================
 * Pattern matching
 * ================================================================ */

static bool match_pattern(deck_arena_t *a, deck_env_t *env,
                          const ast_node_t *pat, deck_value_t *val)
{
    if (!pat) return false;
    switch (pat->kind) {
        case AST_PAT_WILD: return true;
        case AST_PAT_IDENT:
            deck_env_bind(a, env, pat->as.pat_ident, val);
            return true;
        case AST_PAT_LIT: {
            const ast_node_t *lit = pat->as.pat_lit;
            if (!lit || !val) return false;
            switch (lit->kind) {
                case AST_LIT_INT:   return val->type == DECK_T_INT   && val->as.i == lit->as.i;
                case AST_LIT_FLOAT: return val->type == DECK_T_FLOAT && val->as.f == lit->as.f;
                case AST_LIT_BOOL:  return val->type == DECK_T_BOOL  && val->as.b == lit->as.b;
                case AST_LIT_STR:   return val->type == DECK_T_STR   && val->as.s.ptr == lit->as.s;
                case AST_LIT_ATOM:  return val->type == DECK_T_ATOM  && val->as.atom == lit->as.s;
                case AST_LIT_UNIT:  return val->type == DECK_T_UNIT;
                case AST_LIT_NONE:  return val->type == DECK_T_OPTIONAL && val->as.opt.inner == NULL;
                default: return false;
            }
        }
        case AST_PAT_VARIANT: {
            /* DL2 F22 — built-in variants:
             *   some(p)  matches DECK_T_OPTIONAL with inner != NULL;
             *            sub matches the inner value.
             *   ok(p)    matches tuple (:ok,  v); sub matches v.
             *   err(p)   matches tuple (:err, e); sub matches e.
             */
            if (!val) return false;
            const char *ctor = pat->as.pat_variant.ctor;
            if (!ctor) return false;
            if (strcmp(ctor, "some") == 0) {
                if (val->type != DECK_T_OPTIONAL || val->as.opt.inner == NULL) return false;
                if (pat->as.pat_variant.n_subs != 1) return false;
                return match_pattern(a, env, pat->as.pat_variant.subs[0], val->as.opt.inner);
            }
            if (strcmp(ctor, "ok") == 0 || strcmp(ctor, "err") == 0) {
                if (val->type != DECK_T_TUPLE || val->as.tuple.arity != 2) return false;
                deck_value_t *tag = val->as.tuple.items[0];
                if (!tag || tag->type != DECK_T_ATOM) return false;
                if (strcmp(tag->as.atom, ctor) != 0) return false;
                if (pat->as.pat_variant.n_subs != 1) return false;
                return match_pattern(a, env, pat->as.pat_variant.subs[0], val->as.tuple.items[1]);
            }
            return false;   /* unknown ctor */
        }
        default: return false;
    }
}

/* ================================================================
 * Expression interpretation
 * ================================================================ */

static deck_value_t *run_binop(deck_interp_ctx_t *c, deck_env_t *env, const ast_node_t *n);
static deck_value_t *run_call (deck_interp_ctx_t *c, deck_env_t *env, const ast_node_t *n);
static deck_value_t *run_match(deck_interp_ctx_t *c, deck_env_t *env, const ast_node_t *n);

deck_value_t *deck_interp_run(deck_interp_ctx_t *c, deck_env_t *env, const ast_node_t *n)
{
    if (!n || c->err != DECK_RT_OK) return NULL;
    if (++c->depth > DECK_INTERP_STACK_MAX) {
        set_err(c, DECK_RT_STACK_OVERFLOW, n->line, n->col, "stack depth > %u", DECK_INTERP_STACK_MAX);
        c->depth--; return NULL;
    }
    /* DL2 F21.3 — tail-position management.
     *
     * Save the inherited tail flag and restore on exit. Sub-expressions
     * that are NOT in tail position (operands, args, conditions,
     * scrutinees) reset the flag locally before recursing; subs that
     * ARE in tail position (then/else of if, last item of do, body of
     * let, arm body of match) re-assert `saved_tail` before recursing.
     * The trampoline itself fires inside run_call when c->tail_pos is
     * true at a fn-value call site. */
    bool saved_tail = c->tail_pos;
    deck_value_t *r = NULL;

    switch (n->kind) {
        case AST_LIT_INT:    r = deck_new_int(n->as.i); break;
        case AST_LIT_FLOAT:  r = deck_new_float(n->as.f); break;
        case AST_LIT_BOOL:   r = deck_retain(deck_new_bool(n->as.b)); break;
        case AST_LIT_STR:    r = deck_new_str_cstr(n->as.s); break;
        case AST_LIT_ATOM:   r = deck_new_atom(n->as.s); break;
        case AST_LIT_UNIT:   r = deck_retain(deck_unit()); break;
        case AST_LIT_NONE:   r = deck_new_none(); break;

        case AST_LIT_LIST: {
            /* DL2 F21.4: build a deck list value from the literal nodes. */
            uint32_t len = n->as.list.items.len;
            r = deck_new_list(len);
            if (!r) { set_err(c, DECK_RT_NO_MEMORY, n->line, n->col, "list alloc"); break; }
            c->tail_pos = false;
            for (uint32_t i = 0; i < len; i++) {
                deck_value_t *item = deck_interp_run(c, env, n->as.list.items.items[i]);
                if (!item) { deck_release(r); r = NULL; break; }
                if (deck_list_push(r, item) != DECK_RT_OK) {
                    deck_release(item); deck_release(r); r = NULL;
                    set_err(c, DECK_RT_NO_MEMORY, n->line, n->col, "list push");
                    break;
                }
                deck_release(item);
            }
            break;
        }

        case AST_LIT_TUPLE: {
            /* DL2 F21.5: build a deck tuple value. Items evaluated left
             * to right (non-tail). */
            uint32_t arity = n->as.tuple_lit.items.len;
            if (arity == 0) {
                r = deck_retain(deck_unit());
                break;
            }
            c->tail_pos = false;
            deck_value_t *items[16] = {0};
            if (arity > 16) {
                set_err(c, DECK_RT_TYPE_MISMATCH, n->line, n->col,
                        "tuple arity > 16 not supported");
                break;
            }
            bool ok = true;
            for (uint32_t i = 0; i < arity; i++) {
                items[i] = deck_interp_run(c, env, n->as.tuple_lit.items.items[i]);
                if (!items[i]) { ok = false; break; }
            }
            if (ok) r = deck_new_tuple(items, arity);
            for (uint32_t i = 0; i < arity; i++) if (items[i]) deck_release(items[i]);
            break;
        }

        case AST_WITH: {
            /* DL2 F22.2 — clone base map and apply field updates. */
            c->tail_pos = false;
            deck_value_t *base = deck_interp_run(c, env, n->as.with_.base);
            if (!base) break;
            if (base->type != DECK_T_MAP) {
                set_err(c, DECK_RT_TYPE_MISMATCH, n->line, n->col,
                        "`with` requires map/record (got %s)",
                        deck_type_name(base->type));
                deck_release(base);
                break;
            }
            deck_value_t *out = deck_new_map(base->as.map.cap > 0 ? base->as.map.cap : 4);
            if (!out) { deck_release(base); break; }
            for (uint32_t i = 0; i < base->as.map.len; i++) {
                if (base->as.map.entries[i].used)
                    deck_map_put(out, base->as.map.entries[i].key, base->as.map.entries[i].val);
            }
            deck_release(base);
            uint32_t nu = n->as.with_.keys.len;
            for (uint32_t i = 0; i < nu; i++) {
                deck_value_t *k = deck_interp_run(c, env, n->as.with_.keys.items[i]);
                if (!k) { deck_release(out); out = NULL; break; }
                deck_value_t *v = deck_interp_run(c, env, n->as.with_.vals.items[i]);
                if (!v) { deck_release(k); deck_release(out); out = NULL; break; }
                deck_map_put(out, k, v);
                deck_release(k); deck_release(v);
            }
            r = out;
            break;
        }

        case AST_LIT_MAP: {
            /* DL2 F21.6: build a map from {k: v} entries. */
            uint32_t entries = n->as.map_lit.keys.len;
            r = deck_new_map(entries);
            if (!r) { set_err(c, DECK_RT_NO_MEMORY, n->line, n->col, "map alloc"); break; }
            c->tail_pos = false;
            for (uint32_t i = 0; i < entries; i++) {
                deck_value_t *k = deck_interp_run(c, env, n->as.map_lit.keys.items[i]);
                if (!k) { deck_release(r); r = NULL; break; }
                deck_value_t *v = deck_interp_run(c, env, n->as.map_lit.vals.items[i]);
                if (!v) { deck_release(k); deck_release(r); r = NULL; break; }
                deck_err_t pr = deck_map_put(r, k, v);
                deck_release(k); deck_release(v);
                if (pr != DECK_RT_OK) {
                    deck_release(r); r = NULL;
                    set_err(c, pr, n->line, n->col, "map put failed");
                    break;
                }
            }
            break;
        }

        case AST_TUPLE_GET: {
            c->tail_pos = false;
            deck_value_t *t = deck_interp_run(c, env, n->as.tuple_get.obj);
            if (!t) break;
            if (t->type != DECK_T_TUPLE) {
                set_err(c, DECK_RT_TYPE_MISMATCH, n->line, n->col,
                        "tuple-field access requires tuple (got %s)",
                        deck_type_name(t->type));
                deck_release(t);
                break;
            }
            if (n->as.tuple_get.idx >= t->as.tuple.arity) {
                set_err(c, DECK_RT_OUT_OF_RANGE, n->line, n->col,
                        "tuple index %u out of range (arity=%u)",
                        (unsigned)n->as.tuple_get.idx, (unsigned)t->as.tuple.arity);
                deck_release(t);
                break;
            }
            r = deck_retain(t->as.tuple.items[n->as.tuple_get.idx]);
            deck_release(t);
            break;
        }

        case AST_IDENT: {
            deck_value_t *v = deck_env_lookup(env, n->as.s);
            if (v) { r = deck_retain(v); break; }
            set_err(c, DECK_RT_INTERNAL, n->line, n->col, "unbound identifier '%s'", n->as.s);
            break;
        }

        case AST_BINOP:
            c->tail_pos = false;
            r = run_binop(c, env, n);
            break;

        case AST_UNARY: {
            c->tail_pos = false;
            deck_value_t *x = deck_interp_run(c, env, n->as.unary.expr);
            if (!x) break;
            if (n->as.unary.op == UNARY_NEG) {
                if      (x->type == DECK_T_INT)   r = deck_new_int(-x->as.i);
                else if (x->type == DECK_T_FLOAT) r = deck_new_float(-x->as.f);
                else set_err(c, DECK_RT_TYPE_MISMATCH, n->line, n->col, "- needs int or float");
            } else {
                r = deck_retain(deck_new_bool(!deck_is_truthy(x)));
            }
            deck_release(x);
            break;
        }

        case AST_CALL:
            /* run_call inspects c->tail_pos to decide trampoline vs invoke. */
            r = run_call(c, env, n);
            break;

        case AST_DOT: {
            c->tail_pos = false;
            /* Capability dispatch first (e.g. system.info.deck_level). */
            char full[96];
            if (build_cap_name(n, full, sizeof(full))) {
                const builtin_t *b = find_builtin(full);
                if (b && b->min_arity == 0) { r = b->fn(NULL, 0, c); break; }
            }
            /* DL2 F22.2: record/map field access. Evaluate obj and look
             * up the field as an interned atom key. Missing field → none.
             * Records are just maps with conventional keys (or maps with
             * an extra `:__type` tag). */
            deck_value_t *obj = deck_interp_run(c, env, n->as.dot.obj);
            if (!obj) break;
            if (obj->type == DECK_T_MAP) {
                deck_value_t *key = deck_new_atom(n->as.dot.field);
                if (!key) { deck_release(obj); break; }
                deck_value_t *v = deck_map_get(obj, key);
                deck_release(key);
                r = v ? deck_retain(v) : deck_new_none();
                deck_release(obj);
                break;
            }
            set_err(c, DECK_RT_TYPE_MISMATCH, n->line, n->col,
                    "field access on %s — only map/record supported",
                    deck_type_name(obj->type));
            deck_release(obj);
            break;
        }

        case AST_IF: {
            c->tail_pos = false;
            deck_value_t *cond = deck_interp_run(c, env, n->as.if_.cond);
            if (!cond) break;
            bool t = deck_is_truthy(cond);
            deck_release(cond);
            c->tail_pos = saved_tail;     /* then/else inherit */
            r = deck_interp_run(c, env, t ? n->as.if_.then_ : n->as.if_.else_);
            break;
        }

        case AST_LET: {
            c->tail_pos = false;
            deck_value_t *v = deck_interp_run(c, env, n->as.let.value);
            if (!v) break;
            deck_env_bind(c->arena, env, n->as.let.name, v);
            deck_release(v);
            if (n->as.let.body) {
                c->tail_pos = saved_tail;  /* body inherits */
                r = deck_interp_run(c, env, n->as.let.body);
            } else {
                r = deck_retain(deck_unit());
            }
            break;
        }

        case AST_DO: {
            deck_value_t *last = deck_retain(deck_unit());
            uint32_t last_i = n->as.do_.exprs.len;
            for (uint32_t i = 0; i < n->as.do_.exprs.len; i++) {
                deck_release(last);
                /* Only the LAST item is in tail position. */
                c->tail_pos = (i + 1 == last_i) ? saved_tail : false;
                last = deck_interp_run(c, env, n->as.do_.exprs.items[i]);
                if (!last) { r = NULL; goto do_done; }
            }
            r = last;
        do_done: break;
        }

        case AST_MATCH: r = run_match(c, env, n); break;

        case AST_SEND:       r = deck_new_atom(n->as.send.event); break;
        case AST_TRANSITION: r = deck_new_atom(n->as.transition.target); break;

        case AST_FN_DEF: {
            /* The closure captures `env` so that mutually-recursive
             * named fns at the same scope can find each other (top-level
             * pre-binding adds all fn names to the global env BEFORE any
             * body runs).
             *
             * F21.2: when name == NULL the node is an anonymous lambda
             * — return the fn value directly so it can flow into a let
             * binding, an argument, or any other expression position.
             * When name != NULL bind it into the current env and return
             * unit (declaration semantics). */
            deck_value_t *fnv = deck_new_fn(n->as.fndef.name,
                                            n->as.fndef.params,
                                            n->as.fndef.n_params,
                                            n->as.fndef.body,
                                            env);
            if (!fnv) {
                set_err(c, DECK_RT_NO_MEMORY, n->line, n->col, "fn alloc failed");
                break;
            }
            if (n->as.fndef.name) {
                deck_env_bind(c->arena, env, n->as.fndef.name, fnv);
                deck_release(fnv);
                r = deck_retain(deck_unit());
            } else {
                r = fnv;
            }
            break;
        }

        default:
            set_err(c, DECK_RT_INTERNAL, n->line, n->col,
                    "unsupported AST kind %d", (int)n->kind);
            break;
    }

    c->tail_pos = saved_tail;
    c->depth--;
    return r;
}

static deck_value_t *run_match(deck_interp_ctx_t *c, deck_env_t *env, const ast_node_t *n)
{
    bool tail = c->tail_pos;
    c->tail_pos = false;
    deck_value_t *scrut = deck_interp_run(c, env, n->as.match.scrut);
    c->tail_pos = tail;
    if (!scrut) return NULL;
    deck_value_t *out = NULL;
    for (uint32_t i = 0; i < n->as.match.n_arms; i++) {
        deck_env_t *arm_env = deck_env_new(c->arena, env);
        if (!match_pattern(c->arena, arm_env, n->as.match.arms[i].pattern, scrut)) {
            deck_env_release(arm_env);
            continue;
        }
        if (n->as.match.arms[i].guard) {
            c->tail_pos = false;
            deck_value_t *g = deck_interp_run(c, arm_env, n->as.match.arms[i].guard);
            c->tail_pos = tail;
            if (!g) { deck_env_release(arm_env); deck_release(scrut); return NULL; }
            bool gt = deck_is_truthy(g);
            deck_release(g);
            if (!gt) { deck_env_release(arm_env); continue; }
        }
        c->tail_pos = tail;     /* arm body inherits */
        out = deck_interp_run(c, arm_env, n->as.match.arms[i].body);
        deck_env_release(arm_env);
        goto done;
    }
    set_err(c, DECK_RT_PATTERN_FAILED, n->line, n->col, "no match arm matched");
done:
    deck_release(scrut);
    return out;
}

static deck_value_t *do_arith(deck_interp_ctx_t *c, binop_t op,
                               deck_value_t *L, deck_value_t *R,
                               uint32_t ln, uint32_t co)
{
    bool fl = (L->type == DECK_T_FLOAT || R->type == DECK_T_FLOAT);
    if ((L->type != DECK_T_INT && L->type != DECK_T_FLOAT) ||
        (R->type != DECK_T_INT && R->type != DECK_T_FLOAT)) {
        set_err(c, DECK_RT_TYPE_MISMATCH, ln, co, "arithmetic needs numeric operands");
        return NULL;
    }
    if (fl) {
        double a = L->type == DECK_T_INT ? (double)L->as.i : L->as.f;
        double b = R->type == DECK_T_INT ? (double)R->as.i : R->as.f;
        double z = 0;
        switch (op) {
            case BINOP_ADD: z = a + b; break;
            case BINOP_SUB: z = a - b; break;
            case BINOP_MUL: z = a * b; break;
            case BINOP_DIV: if (b == 0) { set_err(c, DECK_RT_DIVIDE_BY_ZERO, ln, co, "div0"); return NULL; } z = a / b; break;
            case BINOP_MOD: z = fmod(a, b); break;
            case BINOP_POW: z = pow(a, b); break;
            default: set_err(c, DECK_RT_INTERNAL, ln, co, "binop?"); return NULL;
        }
        return deck_new_float(z);
    } else {
        int64_t a = L->as.i, b = R->as.i, z = 0;
        switch (op) {
            case BINOP_ADD: z = a + b; break;
            case BINOP_SUB: z = a - b; break;
            case BINOP_MUL: z = a * b; break;
            case BINOP_DIV: if (b == 0) { set_err(c, DECK_RT_DIVIDE_BY_ZERO, ln, co, "div0"); return NULL; } z = a / b; break;
            case BINOP_MOD: if (b == 0) { set_err(c, DECK_RT_DIVIDE_BY_ZERO, ln, co, "div0"); return NULL; } z = a % b; break;
            case BINOP_POW: z = 1; for (int64_t k = 0; k < b; k++) z *= a; break;
            default: set_err(c, DECK_RT_INTERNAL, ln, co, "binop?"); return NULL;
        }
        return deck_new_int(z);
    }
}

static deck_value_t *do_compare(deck_interp_ctx_t *c, binop_t op,
                                 deck_value_t *L, deck_value_t *R)
{
    (void)c;
    int cmp = 0;
    if ((L->type == DECK_T_INT || L->type == DECK_T_FLOAT) &&
        (R->type == DECK_T_INT || R->type == DECK_T_FLOAT)) {
        double a = L->type == DECK_T_INT ? (double)L->as.i : L->as.f;
        double b = R->type == DECK_T_INT ? (double)R->as.i : R->as.f;
        cmp = (a < b) ? -1 : (a > b) ? 1 : 0;
    } else if (L->type == R->type) {
        switch (L->type) {
            case DECK_T_STR:  cmp = (L->as.s.ptr == R->as.s.ptr) ? 0 : 1; break;
            case DECK_T_ATOM: cmp = (L->as.atom  == R->as.atom)  ? 0 : 1; break;
            case DECK_T_BOOL: cmp = (L->as.b == R->as.b) ? 0 : 1; break;
            case DECK_T_UNIT: cmp = 0; break;
            default: cmp = 1; break;
        }
    } else { cmp = 1; }
    bool out = false;
    switch (op) {
        case BINOP_LT: out = cmp <  0; break;
        case BINOP_LE: out = cmp <= 0; break;
        case BINOP_GT: out = cmp >  0; break;
        case BINOP_GE: out = cmp >= 0; break;
        case BINOP_EQ: out = cmp == 0; break;
        case BINOP_NE: out = cmp != 0; break;
        default: break;
    }
    return deck_new_bool(out);
}

static deck_value_t *do_concat(deck_interp_ctx_t *c, deck_value_t *L, deck_value_t *R,
                                uint32_t ln, uint32_t co)
{
    if (L->type != DECK_T_STR || R->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, ln, co, "<> needs two strings");
        return NULL;
    }
    char buf[256];
    uint32_t a = L->as.s.len, b = R->as.s.len;
    if (a + b >= sizeof(buf)) b = (uint32_t)(sizeof(buf) - 1 - a);
    memcpy(buf, L->as.s.ptr, a);
    memcpy(buf + a, R->as.s.ptr, b);
    return deck_new_str(buf, a + b);
}

static deck_value_t *run_binop(deck_interp_ctx_t *c, deck_env_t *env, const ast_node_t *n)
{
    binop_t op = n->as.binop.op;
    if (op == BINOP_AND || op == BINOP_OR) {
        deck_value_t *L = deck_interp_run(c, env, n->as.binop.lhs); if (!L) return NULL;
        bool lt = deck_is_truthy(L); deck_release(L);
        if ((op == BINOP_AND && !lt) || (op == BINOP_OR && lt))
            return deck_retain(deck_new_bool(op == BINOP_OR));
        deck_value_t *R = deck_interp_run(c, env, n->as.binop.rhs); if (!R) return NULL;
        bool rt = deck_is_truthy(R); deck_release(R);
        return deck_retain(deck_new_bool(rt));
    }

    /* DL2 F21.10 — pipe operators rewrite shape to a call.
     *   x |>  f  →  f(x)
     *   x |>? f  →  none if x is none; else f(some-unwrapped(x) or x)
     * We don't want to synthesise AST_CALL nodes at runtime, so dispatch
     * via the same call machinery: evaluate the callee expression, then
     * invoke. When `|>?` and L is a none Optional, short-circuit. */
    if (op == BINOP_PIPE || op == BINOP_PIPE_OPT) {
        deck_value_t *L = deck_interp_run(c, env, n->as.binop.lhs); if (!L) return NULL;
        if (op == BINOP_PIPE_OPT &&
            L->type == DECK_T_OPTIONAL && L->as.opt.inner == NULL) {
            deck_release(L);
            return deck_new_none();
        }
        if (op == BINOP_PIPE_OPT &&
            L->type == DECK_T_OPTIONAL && L->as.opt.inner != NULL) {
            deck_value_t *inner = deck_retain(L->as.opt.inner);
            deck_release(L);
            L = inner;
        }
        deck_value_t *R = deck_interp_run(c, env, n->as.binop.rhs);
        if (!R) { deck_release(L); return NULL; }
        if (R->type != DECK_T_FN) {
            set_err(c, DECK_RT_TYPE_MISMATCH, n->line, n->col,
                    "right side of |> must be a function (got %s)",
                    deck_type_name(R->type));
            deck_release(L); deck_release(R);
            return NULL;
        }
        /* Build a synthetic call invocation reusing invoke_user_fn — but
         * invoke_user_fn evaluates args from an AST. Inline a lightweight
         * dispatch here that uses the already-evaluated L. */
        if (R->as.fn.n_params != 1) {
            set_err(c, DECK_RT_TYPE_MISMATCH, n->line, n->col,
                    "fn '%s' takes %u args, pipe supplies 1",
                    R->as.fn.name ? R->as.fn.name : "<anon>",
                    (unsigned)R->as.fn.n_params);
            deck_release(L); deck_release(R);
            return NULL;
        }
        deck_env_t *call_env = deck_env_new(c->arena, R->as.fn.closure);
        if (!call_env) {
            set_err(c, DECK_RT_NO_MEMORY, n->line, n->col, "pipe env alloc");
            deck_release(L); deck_release(R);
            return NULL;
        }
        deck_env_bind(c->arena, call_env, R->as.fn.params[0], L);
        deck_release(L);
        deck_value_t *result = deck_interp_run(c, call_env, R->as.fn.body);
        deck_env_release(call_env);
        deck_release(R);
        return result;
    }

    deck_value_t *L = deck_interp_run(c, env, n->as.binop.lhs); if (!L) return NULL;
    deck_value_t *R = deck_interp_run(c, env, n->as.binop.rhs); if (!R) { deck_release(L); return NULL; }
    deck_value_t *r = NULL;
    switch (op) {
        case BINOP_ADD: case BINOP_SUB: case BINOP_MUL:
        case BINOP_DIV: case BINOP_MOD: case BINOP_POW:
            r = do_arith(c, op, L, R, n->line, n->col); break;
        case BINOP_LT: case BINOP_LE: case BINOP_GT:
        case BINOP_GE: case BINOP_EQ: case BINOP_NE:
            r = do_compare(c, op, L, R); break;
        case BINOP_CONCAT: r = do_concat(c, L, R, n->line, n->col); break;
        case BINOP_IS:
            /* DL2 F21.9: `is` is value/atom equality with strict typing.
             * Reuse the comparison machinery (which also handles atoms
             * via interned-pointer equality). */
            r = do_compare(c, BINOP_EQ, L, R);
            break;
        default: set_err(c, DECK_RT_INTERNAL, n->line, n->col, "unhandled binop");
    }
    deck_release(L); deck_release(R);
    return r;
}

/* Invoke a user-defined function value.
 *
 * Stack discipline: this is the C-recursion hot path for Deck recursion.
 * We deliberately do NOT spill the evaluated args into a stack buffer —
 * each fib(10)-class call already chains ~10 C frames per Deck level,
 * so a 16-pointer array per frame burns the main task stack quickly. We
 * evaluate each arg and immediately bind it into the callee env. If an
 * arg evaluation fails, partially-bound entries are reachable via the
 * arena env until the arena resets (no extra heap leak vs the existing
 * env semantics). */
/* Clear bindings of an env without releasing it. Used by the trampoline
 * to re-use the call env for self-recursive tail calls. */
static void env_unbind_all(deck_env_t *e)
{
    if (!e) return;
    for (uint32_t i = 0; i < e->count; i++) {
        deck_release(e->bindings[i].val);
        e->bindings[i].val  = NULL;
        e->bindings[i].name = NULL;
    }
    e->count = 0;
}

static deck_value_t *invoke_user_fn(deck_interp_ctx_t *c, deck_env_t *env,
                                    deck_value_t *fnv,
                                    const ast_node_t *call_site)
{
    if (!fnv || fnv->type != DECK_T_FN) {
        set_err(c, DECK_RT_TYPE_MISMATCH, call_site->line, call_site->col,
                "callee is not a function");
        return NULL;
    }
    uint32_t argc = call_site->as.call.args.len;
    if (argc != fnv->as.fn.n_params) {
        set_err(c, DECK_RT_TYPE_MISMATCH, call_site->line, call_site->col,
                "fn '%s': arity mismatch (got %u, want %u)",
                fnv->as.fn.name ? fnv->as.fn.name : "<anon>",
                (unsigned)argc, (unsigned)fnv->as.fn.n_params);
        return NULL;
    }
    deck_value_t *current = deck_retain(fnv);
    deck_env_t   *call_env = deck_env_new(c->arena, current->as.fn.closure);
    if (!call_env) {
        deck_release(current);
        set_err(c, DECK_RT_NO_MEMORY, call_site->line, call_site->col,
                "fn call: env alloc failed");
        return NULL;
    }
    /* Initial arg bindings — args come from the *caller* env. */
    bool save_tail = c->tail_pos;
    c->tail_pos = false;
    for (uint32_t i = 0; i < argc; i++) {
        deck_value_t *v = deck_interp_run(c, env, call_site->as.call.args.items[i]);
        if (!v) {
            c->tail_pos = save_tail;
            deck_env_release(call_env);
            deck_release(current);
            return NULL;
        }
        deck_env_bind(c->arena, call_env, current->as.fn.params[i], v);
        deck_release(v);
    }
    c->tail_pos = save_tail;

    /* Trampoline loop. Body runs with tail_pos = true; if a tail call is
     * trapped into c->pending_tc, we rebind args (re-using the env on a
     * self-recursive call, swapping it for a mutual call) and re-enter
     * without growing the C stack. */
    deck_value_t *result = NULL;
    for (;;) {
        bool prev = c->tail_pos;
        c->tail_pos = true;
        c->pending_tc.active = false;
        result = deck_interp_run(c, call_env, current->as.fn.body);
        c->tail_pos = prev;

        if (!c->pending_tc.active) break;

        /* A tail call was trapped — `result` is a placeholder unit. */
        deck_value_t *next_fn = c->pending_tc.fn;       /* ownership: pending */
        uint32_t      next_argc = c->pending_tc.argc;
        if (result) deck_release(result);
        result = NULL;
        c->pending_tc.active = false;

        if (next_fn->type != DECK_T_FN) {
            set_err(c, DECK_RT_TYPE_MISMATCH, c->pending_tc.line, c->pending_tc.col,
                    "tail-call target is not a function");
            for (uint32_t i = 0; i < next_argc; i++) deck_release(c->pending_tc.args[i]);
            deck_release(next_fn);
            break;
        }
        if (next_argc != next_fn->as.fn.n_params) {
            set_err(c, DECK_RT_TYPE_MISMATCH, c->pending_tc.line, c->pending_tc.col,
                    "fn '%s': arity mismatch (got %u, want %u)",
                    next_fn->as.fn.name ? next_fn->as.fn.name : "<anon>",
                    (unsigned)next_argc, (unsigned)next_fn->as.fn.n_params);
            for (uint32_t i = 0; i < next_argc; i++) deck_release(c->pending_tc.args[i]);
            deck_release(next_fn);
            break;
        }

        if (next_fn == current) {
            /* Self-recursive tail call: keep env + fn; rebind args. */
            env_unbind_all(call_env);
            deck_release(next_fn);          /* duplicate retain from pending */
        } else {
            /* Mutual tail call: swap env to the new fn's closure. */
            deck_env_release(call_env);
            call_env = deck_env_new(c->arena, next_fn->as.fn.closure);
            deck_release(current);
            current = next_fn;              /* transfer pending retain */
            if (!call_env) {
                for (uint32_t i = 0; i < next_argc; i++) deck_release(c->pending_tc.args[i]);
                set_err(c, DECK_RT_NO_MEMORY, c->pending_tc.line, c->pending_tc.col,
                        "tail-call env alloc failed");
                break;
            }
        }
        for (uint32_t i = 0; i < next_argc; i++) {
            deck_env_bind(c->arena, call_env, current->as.fn.params[i],
                          c->pending_tc.args[i]);
            deck_release(c->pending_tc.args[i]);
        }
    }

    deck_env_release(call_env);
    deck_release(current);
    return result;
}

/* DL2 F21.3 — trap a fn-value call into c->pending_tc instead of
 * recursing on the C stack. Caller must check c->pending_tc.active and
 * the returned placeholder is unit. */
static deck_value_t *trap_tail_call(deck_interp_ctx_t *c, deck_env_t *env,
                                    deck_value_t *fnv,
                                    const ast_node_t *call_site)
{
    uint32_t argc = call_site->as.call.args.len;
    if (argc > DECK_INTERP_MAX_TC_ARGS) {
        set_err(c, DECK_RT_TYPE_MISMATCH, call_site->line, call_site->col,
                "tail call: too many args (max %u)",
                (unsigned)DECK_INTERP_MAX_TC_ARGS);
        return NULL;
    }
    bool save_tail = c->tail_pos;
    c->tail_pos = false;        /* args are not in tail position */
    deck_value_t *evaluated[DECK_INTERP_MAX_TC_ARGS] = {0};
    for (uint32_t i = 0; i < argc; i++) {
        evaluated[i] = deck_interp_run(c, env, call_site->as.call.args.items[i]);
        if (!evaluated[i]) {
            for (uint32_t k = 0; k < i; k++) deck_release(evaluated[k]);
            c->tail_pos = save_tail;
            return NULL;
        }
    }
    c->tail_pos = save_tail;

    c->pending_tc.fn   = deck_retain(fnv);
    c->pending_tc.argc = argc;
    c->pending_tc.line = call_site->line;
    c->pending_tc.col  = call_site->col;
    for (uint32_t i = 0; i < argc; i++) c->pending_tc.args[i] = evaluated[i];
    c->pending_tc.active = true;
    return deck_retain(deck_unit());   /* placeholder; trampoline discards it */
}

static deck_value_t *run_call(deck_interp_ctx_t *c, deck_env_t *env, const ast_node_t *n)
{
    const ast_node_t *fn = n->as.call.fn;
    bool tail = c->tail_pos;
    /* Args + callee evaluation are never in tail position. We restore the
     * incoming flag before any potential tail-call dispatch so the trap
     * decision sees the right value. */
    c->tail_pos = false;

    /* Bare ident: prefer user-defined fn binding in env over builtins.
     * (Builtins are namespaced via dot for capabilities; bare builtins are
     * the type conversions, which would never collide with a user fn name
     * in a well-written program — but we check env first to allow shadowing.) */
    if (fn && fn->kind == AST_IDENT) {
        deck_value_t *bound = deck_env_lookup(env, fn->as.s);
        if (bound && bound->type == DECK_T_FN) {
            c->tail_pos = tail;
            if (tail) return trap_tail_call(c, env, bound, n);
            return invoke_user_fn(c, env, bound, n);
        }
        const builtin_t *b = find_bare_builtin(fn->as.s);
        if (b) {
            uint32_t argc = n->as.call.args.len;
            if ((int)argc < b->min_arity || (int)argc > b->max_arity) {
                set_err(c, DECK_RT_TYPE_MISMATCH, n->line, n->col, "%s: arity", b->name);
                return NULL;
            }
            deck_value_t *args[4] = {0};
            for (uint32_t i = 0; i < argc; i++) {
                args[i] = deck_interp_run(c, env, n->as.call.args.items[i]);
                if (!args[i]) { for (uint32_t k = 0; k < i; k++) deck_release(args[k]); return NULL; }
            }
            deck_value_t *r = b->fn(args, argc, c);
            for (uint32_t i = 0; i < argc; i++) deck_release(args[i]);
            return r;
        }
    }
    if (fn && fn->kind == AST_DOT) {
        char full[96];
        if (build_cap_name(fn, full, sizeof(full))) {
            const builtin_t *b = find_builtin(full);
            if (!b) { set_err(c, DECK_RT_INTERNAL, n->line, n->col, "no builtin '%s'", full); return NULL; }
            uint32_t argc = n->as.call.args.len;
            if ((int)argc < b->min_arity || (int)argc > b->max_arity) {
                set_err(c, DECK_RT_TYPE_MISMATCH, n->line, n->col,
                        "%s: arity mismatch (got %u, want %d..%d)",
                        full, (unsigned)argc, b->min_arity, b->max_arity);
                return NULL;
            }
            deck_value_t *args[8] = {0};
            if (argc > 8) { set_err(c, DECK_RT_TYPE_MISMATCH, n->line, n->col, "too many args"); return NULL; }
            for (uint32_t i = 0; i < argc; i++) {
                args[i] = deck_interp_run(c, env, n->as.call.args.items[i]);
                if (!args[i]) { for (uint32_t k = 0; k < i; k++) deck_release(args[k]); return NULL; }
            }
            deck_value_t *r = b->fn(args, argc, c);
            for (uint32_t i = 0; i < argc; i++) deck_release(args[i]);
            return r;
        }
    }
    /* DL2 F21.2: callee is an arbitrary expression (e.g. an inline
     * lambda `(x -> x*2)(5)`, an anonymous fn, or an expression that
     * returns a fn from a higher-order helper). Evaluate it and dispatch
     * if the result is a fn value. */
    if (fn) {
        deck_value_t *callee = deck_interp_run(c, env, fn);
        if (!callee) return NULL;
        if (callee->type != DECK_T_FN) {
            set_err(c, DECK_RT_TYPE_MISMATCH, n->line, n->col,
                    "callee is not a function (got %s)",
                    deck_type_name(callee->type));
            deck_release(callee);
            return NULL;
        }
        c->tail_pos = tail;
        deck_value_t *r;
        if (tail) r = trap_tail_call(c, env, callee, n);
        else      r = invoke_user_fn(c, env, callee, n);
        deck_release(callee);
        return r;
    }
    set_err(c, DECK_RT_INTERNAL, n->line, n->col, "empty call expression");
    return NULL;
}

/* ================================================================
 * Top-level entrypoint
 * ================================================================ */

void deck_interp_init(deck_interp_ctx_t *c, deck_arena_t *arena)
{
    memset(c, 0, sizeof(*c));
    c->arena  = arena;
    c->global = deck_env_new(arena, NULL);
    c->err    = DECK_RT_OK;
}

static const ast_node_t *find_on_launch(const ast_node_t *mod)
{
    if (!mod || mod->kind != AST_MODULE) return NULL;
    for (uint32_t i = 0; i < mod->as.module.items.len; i++) {
        const ast_node_t *it = mod->as.module.items.items[i];
        if (it && it->kind == AST_ON && it->as.on.event &&
            strcmp(it->as.on.event, "launch") == 0) return it;
    }
    return NULL;
}

static const ast_node_t *find_machine(const ast_node_t *mod)
{
    if (!mod || mod->kind != AST_MODULE) return NULL;
    for (uint32_t i = 0; i < mod->as.module.items.len; i++) {
        const ast_node_t *it = mod->as.module.items.items[i];
        if (it && it->kind == AST_MACHINE) return it;
    }
    return NULL;
}

static const ast_node_t *find_state(const ast_node_t *machine, const char *name)
{
    if (!machine || !name) return NULL;
    for (uint32_t i = 0; i < machine->as.machine.states.len; i++) {
        const ast_node_t *st = machine->as.machine.states.items[i];
        if (st && st->kind == AST_STATE && st->as.state.name &&
            strcmp(st->as.state.name, name) == 0) return st;
    }
    return NULL;
}

/* DL1 machine lifecycle: enter initial state, run on enter hook(s),
 * execute transitions sequentially (max 32 to avoid loops), run on
 * leave before switching, terminate when a state has no transition. */
#define DECK_MACHINE_MAX_TRANSITIONS  32

static void run_state_hooks(deck_interp_ctx_t *c, const ast_node_t *state,
                            const char *kind, const char **transition_target)
{
    if (transition_target) *transition_target = NULL;
    for (uint32_t i = 0; i < state->as.state.hooks.len; i++) {
        const ast_node_t *h = state->as.state.hooks.items[i];
        if (!h) continue;
        if (h->kind == AST_STATE_HOOK && kind && h->as.state_hook.kind &&
            strcmp(h->as.state_hook.kind, kind) == 0) {
            deck_value_t *r = deck_interp_run(c, c->global, h->as.state_hook.body);
            if (r) deck_release(r);
            if (c->err != DECK_RT_OK) return;
        }
        if (transition_target && h->kind == AST_TRANSITION) {
            *transition_target = h->as.transition.target;
            /* First transition wins (DL1 simplification). */
            return;
        }
    }
}

static deck_err_t run_machine(const ast_node_t *machine, deck_interp_ctx_t *c)
{
    if (!machine || machine->as.machine.states.len == 0) return DECK_RT_OK;
    const ast_node_t *state = machine->as.machine.states.items[0];
    ESP_LOGI(TAG, "machine '%s' start state :%s",
             machine->as.machine.name, state->as.state.name);

    for (int steps = 0; steps < DECK_MACHINE_MAX_TRANSITIONS; steps++) {
        const char *next = NULL;
        run_state_hooks(c, state, "enter", &next);
        if (c->err != DECK_RT_OK) return c->err;
        if (!next) {
            /* No transition → terminate in this state. */
            run_state_hooks(c, state, "leave", NULL);
            ESP_LOGI(TAG, "machine '%s' terminated in :%s",
                     machine->as.machine.name, state->as.state.name);
            return c->err;
        }
        run_state_hooks(c, state, "leave", NULL);
        if (c->err != DECK_RT_OK) return c->err;

        const ast_node_t *nextst = find_state(machine, next);
        if (!nextst) {
            set_err(c, DECK_RT_INTERNAL, state->line, state->col,
                    "transition target :%s not found", next);
            return c->err;
        }
        ESP_LOGI(TAG, "machine '%s' :%s -> :%s",
                 machine->as.machine.name, state->as.state.name, next);
        state = nextst;
    }
    set_err(c, DECK_RT_INTERNAL, machine->line, machine->col,
            "machine exceeded max transitions (%d)", DECK_MACHINE_MAX_TRANSITIONS);
    return c->err;
}

deck_err_t deck_runtime_run_on_launch(const char *src, uint32_t len)
{
    deck_arena_t  arena;
    deck_loader_t ld;
    deck_arena_init(&arena, 0);
    deck_loader_init(&ld, &arena);

    deck_err_t rc = deck_loader_load(&ld, src, len);
    if (rc != DECK_LOAD_OK) {
        ESP_LOGE(TAG, "load failed @ stage %u: %s (%u:%u)",
                 ld.err_stage, ld.err_msg, ld.err_line, ld.err_col);
        deck_arena_reset(&arena);
        return rc;
    }

    deck_interp_ctx_t c;
    deck_interp_init(&c, &arena);

    /* DL2 F21.1: pre-bind every top-level `fn` so @on launch and @machine
     * bodies can reference them — and so fn bodies can reference each
     * other (mutual recursion) regardless of declaration order. */
    if (ld.module && ld.module->kind == AST_MODULE) {
        for (uint32_t i = 0; i < ld.module->as.module.items.len; i++) {
            const ast_node_t *it = ld.module->as.module.items.items[i];
            if (it && it->kind == AST_FN_DEF) {
                deck_value_t *r = deck_interp_run(&c, c.global, it);
                if (r) deck_release(r);
                if (c.err != DECK_RT_OK) break;
            }
        }
    }

    const ast_node_t *on = (c.err == DECK_RT_OK) ? find_on_launch(ld.module) : NULL;
    if (on) {
        deck_value_t *r = deck_interp_run(&c, c.global, on->as.on.body);
        if (r) deck_release(r);
    }

    if (c.err == DECK_RT_OK) {
        const ast_node_t *machine = find_machine(ld.module);
        if (machine) run_machine(machine, &c);
    }

    deck_err_t run_rc = c.err;
    if (run_rc != DECK_RT_OK) {
        ESP_LOGE(TAG, "runtime error @ %u:%u — %s: %s",
                 c.err_line, c.err_col, deck_err_name(run_rc), c.err_msg);
    }
    /* Release every retained value bound in the global env (top-level
     * fn pre-bindings, top-level let bindings inside @on launch). The
     * arena reset only frees the env struct; without this, refcounted
     * values leak across runs and `memory.no_residual_leak` trips. */
    deck_env_release(c.global);
    deck_arena_reset(&arena);
    return run_rc;
}
