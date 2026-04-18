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

    { NULL, NULL, 0, 0 },
};

/* Bare-ident builtins: str(), int(), float(), bool() type conversions. */
static const builtin_t BARE_BUILTINS[] = {
    { "str",   b_to_str,   1, 1 },
    { "int",   b_to_int,   1, 1 },
    { "float", b_to_float, 1, 1 },
    { "bool",  b_to_bool,  1, 1 },
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
    deck_value_t *r = NULL;

    switch (n->kind) {
        case AST_LIT_INT:    r = deck_new_int(n->as.i); break;
        case AST_LIT_FLOAT:  r = deck_new_float(n->as.f); break;
        case AST_LIT_BOOL:   r = deck_retain(deck_new_bool(n->as.b)); break;
        case AST_LIT_STR:    r = deck_new_str_cstr(n->as.s); break;
        case AST_LIT_ATOM:   r = deck_new_atom(n->as.s); break;
        case AST_LIT_UNIT:   r = deck_retain(deck_unit()); break;
        case AST_LIT_NONE:   r = deck_new_none(); break;

        case AST_IDENT: {
            deck_value_t *v = deck_env_lookup(env, n->as.s);
            if (v) { r = deck_retain(v); break; }
            set_err(c, DECK_RT_INTERNAL, n->line, n->col, "unbound identifier '%s'", n->as.s);
            break;
        }

        case AST_BINOP: r = run_binop(c, env, n); break;

        case AST_UNARY: {
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

        case AST_CALL: r = run_call(c, env, n); break;

        case AST_DOT: {
            char full[96];
            if (build_cap_name(n, full, sizeof(full))) {
                const builtin_t *b = find_builtin(full);
                if (b && b->min_arity == 0) { r = b->fn(NULL, 0, c); break; }
            }
            set_err(c, DECK_RT_INTERNAL, n->line, n->col, "cannot use dot chain as value");
            break;
        }

        case AST_IF: {
            deck_value_t *cond = deck_interp_run(c, env, n->as.if_.cond);
            if (!cond) break;
            bool t = deck_is_truthy(cond);
            deck_release(cond);
            r = deck_interp_run(c, env, t ? n->as.if_.then_ : n->as.if_.else_);
            break;
        }

        case AST_LET: {
            deck_value_t *v = deck_interp_run(c, env, n->as.let.value);
            if (!v) break;
            deck_env_bind(c->arena, env, n->as.let.name, v);
            deck_release(v);
            r = n->as.let.body ? deck_interp_run(c, env, n->as.let.body)
                                : deck_retain(deck_unit());
            break;
        }

        case AST_DO: {
            deck_value_t *last = deck_retain(deck_unit());
            for (uint32_t i = 0; i < n->as.do_.exprs.len; i++) {
                deck_release(last);
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

    c->depth--;
    return r;
}

static deck_value_t *run_match(deck_interp_ctx_t *c, deck_env_t *env, const ast_node_t *n)
{
    deck_value_t *scrut = deck_interp_run(c, env, n->as.match.scrut);
    if (!scrut) return NULL;
    deck_value_t *out = NULL;
    for (uint32_t i = 0; i < n->as.match.n_arms; i++) {
        deck_env_t *arm_env = deck_env_new(c->arena, env);
        if (!match_pattern(c->arena, arm_env, n->as.match.arms[i].pattern, scrut)) {
            deck_env_release(arm_env);
            continue;
        }
        if (n->as.match.arms[i].guard) {
            deck_value_t *g = deck_interp_run(c, arm_env, n->as.match.arms[i].guard);
            if (!g) { deck_env_release(arm_env); deck_release(scrut); return NULL; }
            bool gt = deck_is_truthy(g);
            deck_release(g);
            if (!gt) { deck_env_release(arm_env); continue; }
        }
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
    deck_env_t *call_env = deck_env_new(c->arena, fnv->as.fn.closure);
    if (!call_env) {
        set_err(c, DECK_RT_NO_MEMORY, call_site->line, call_site->col,
                "fn call: env alloc failed");
        return NULL;
    }
    for (uint32_t i = 0; i < argc; i++) {
        deck_value_t *v = deck_interp_run(c, env, call_site->as.call.args.items[i]);
        if (!v) { deck_env_release(call_env); return NULL; }
        deck_env_bind(c->arena, call_env, fnv->as.fn.params[i], v);
        deck_release(v);
    }
    deck_value_t *result = deck_interp_run(c, call_env, fnv->as.fn.body);
    deck_env_release(call_env);
    return result;
}

static deck_value_t *run_call(deck_interp_ctx_t *c, deck_env_t *env, const ast_node_t *n)
{
    const ast_node_t *fn = n->as.call.fn;
    /* Bare ident: prefer user-defined fn binding in env over builtins.
     * (Builtins are namespaced via dot for capabilities; bare builtins are
     * the type conversions, which would never collide with a user fn name
     * in a well-written program — but we check env first to allow shadowing.) */
    if (fn && fn->kind == AST_IDENT) {
        deck_value_t *bound = deck_env_lookup(env, fn->as.s);
        if (bound && bound->type == DECK_T_FN) {
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
        deck_value_t *r = invoke_user_fn(c, env, callee, n);
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
