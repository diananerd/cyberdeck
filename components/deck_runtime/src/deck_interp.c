#include "deck_interp.h"
#include "deck_alloc.h"
#include "deck_intern.h"
#include "deck_loader.h"
#include "deck_dvc.h"
#include "drivers/deck_sdi_time.h"
#include "drivers/deck_sdi_info.h"
#include "drivers/deck_sdi_nvs.h"
#include "drivers/deck_sdi_fs.h"
#include "drivers/deck_sdi_shell.h"
#include "drivers/deck_sdi_bridge_ui.h"

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

/* Forward declarations for helpers defined later in this file. */
static deck_value_t *make_result_tag(const char *tag, deck_value_t *payload);
/* Concept #44 — machine.* builtins defined after struct deck_runtime_app
 * (way below) so they can access the slot array. */
static deck_value_t *b_machine_send (deck_value_t **args, uint32_t n, deck_interp_ctx_t *c);
static deck_value_t *b_machine_state(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c);

/* Concept #47 — intent binding table + app struct moved forward so
 * content_render (concept #46) can access app->intents directly. */
#define DECK_RUNTIME_MAX_INTENTS 64

/* Concept #58 — binding carries a captured action AST + env. At tap time
 * the runtime evaluates `action_ast` in a child of `captured_env`, with
 * `event` optionally injected by concept #59/#60 payload-carrying API.
 * Works for any action shape: `Machine.send(:e[, payload])`, `apps.launch`,
 * `bluesky.post(...)`, composed `do … end` blocks, pipelines, etc.
 *
 * `form_node` — concept #60: when non-NULL, this binding is the submit
 * intent of a form; the dispatcher walks the DVC subtree at tap time to
 * aggregate {field_name: current_value} into `event.values`. Currently
 * the aggregation happens on the bridge side (richer access to widget
 * state); `form_node` is reserved for a future runtime-side walker. */
typedef struct {
    uint32_t      id;              /* 0 = slot empty */
    ast_node_t   *action_ast;      /* expression to evaluate on tap */
    deck_env_t   *captured_env;    /* retained; released when slot is cleared */
} deck_intent_binding_t;

struct deck_runtime_app {
    bool               in_use;
    deck_arena_t       arena;
    deck_loader_t      ld;
    deck_interp_ctx_t  ctx;
    const char        *machine_state;
    deck_intent_binding_t intents[DECK_RUNTIME_MAX_INTENTS];
    uint32_t           next_intent_id;
};

static struct deck_runtime_app *app_from_ctx(deck_interp_ctx_t *c);

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
/* Spec 03-deck-os §3 `@builtin log.debug`. Maps to ESP_LOGD — no-op in
 * production builds (CONFIG_LOG_DEFAULT_LEVEL stops it by default) but
 * visible via menuconfig when the developer opts in. */
static deck_value_t *b_log_debug(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    if (args[0] && args[0]->type == DECK_T_STR)
        ESP_LOGD("deck.app", "%.*s", (int)args[0]->as.s.len, args[0]->as.s.ptr);
    return deck_retain(deck_unit());
}
/* Spec §3: time.now() -> Timestamp (epoch seconds). Uses wall clock when
 * set; falls back to monotonic seconds since boot so ordering still holds
 * on hardware without an RTC and before SNTP sync lands. */
static deck_value_t *b_time_now(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)a; (void)n; (void)c;
    int64_t w = deck_sdi_time_wall_epoch_s();
    if (w <= 0) w = deck_sdi_time_monotonic_us() / 1000000;
    return deck_new_int(w);
}
static deck_value_t *b_info_device_id(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{ (void)a; (void)n; (void)c; const char *d = deck_sdi_info_device_id(); return deck_new_str_cstr(d ? d : ""); }
static deck_value_t *b_info_free_heap(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{ (void)a; (void)n; (void)c; return deck_new_int((int64_t)deck_sdi_info_free_heap()); }
static deck_value_t *b_info_deck_level(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{ (void)a; (void)n; (void)c; return deck_new_int(deck_sdi_info_deck_level()); }

/* Concept #39 — system.info completeness (spec §3).
 * device_model / os_name / os_version are platform-identity constants —
 * hardcoded here until the SDI vtable grows them (future concept). The
 * Waveshare ESP32-S3-Touch-LCD-4.3 is the only supported board today. */
static deck_value_t *b_info_device_model(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{ (void)a; (void)n; (void)c; return deck_new_str_cstr("ESP32-S3-Touch-LCD-4.3"); }
static deck_value_t *b_info_os_name(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{ (void)a; (void)n; (void)c; return deck_new_str_cstr("CyberDeck"); }
static deck_value_t *b_info_os_version(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)a; (void)n; (void)c;
    const char *v = deck_sdi_info_runtime_version();
    return deck_new_str_cstr(v ? v : "0.0.0");
}

/* Walk the current module for @app.<field>. Used by app_id/app_version. */
static const char *info_app_field(deck_interp_ctx_t *c, const char *field)
{
    if (!c || !c->module || c->module->kind != AST_MODULE) return NULL;
    for (uint32_t i = 0; i < c->module->as.module.items.len; i++) {
        const ast_node_t *it = c->module->as.module.items.items[i];
        if (!it || it->kind != AST_APP) continue;
        for (uint32_t f = 0; f < it->as.app.n_fields; f++) {
            const ast_app_field_t *fld = &it->as.app.fields[f];
            if (fld->name && strcmp(fld->name, field) == 0 &&
                fld->value && fld->value->kind == AST_LIT_STR) {
                return fld->value->as.s;
            }
        }
        break;
    }
    return NULL;
}

static deck_value_t *b_info_app_id(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)a; (void)n;
    const char *id = info_app_field(c, "id");
    if (!id) id = deck_sdi_info_current_app_id();
    return deck_new_str_cstr(id ? id : "");
}

static deck_value_t *b_info_app_version(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)a; (void)n;
    const char *v = info_app_field(c, "version");
    return deck_new_str_cstr(v ? v : "0.0.0");
}

static deck_value_t *b_info_uptime(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)a; (void)n; (void)c;
    /* Duration is in seconds per concept #32's canonical unit. */
    return deck_new_int(deck_sdi_time_monotonic_us() / 1000000);
}

static deck_value_t *b_info_cpu_freq(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)a; (void)n; (void)c;
    /* ESP32-S3 default is 240 MHz; runtime menuconfig may lower it to 160/80.
     * Using rtc_clk_cpu_freq_get() would need another include — keep simple. */
    return deck_new_int(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
}

/* Versions record — aggregate envelope per spec §15 @type Versions. */
static deck_value_t *b_info_versions(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)a; (void)n;
    deck_value_t *m = deck_new_map(16);
    if (!m) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "system.info.versions alloc"); return NULL; }
    #define PUT_I(k, v) do { deck_value_t *kk = deck_new_str_cstr(k); deck_value_t *vv = deck_new_int(v); deck_map_put(m, kk, vv); deck_release(kk); deck_release(vv); } while (0)
    #define PUT_S(k, v) do { deck_value_t *kk = deck_new_str_cstr(k); deck_value_t *vv = deck_new_str_cstr(v ? v : ""); deck_map_put(m, kk, vv); deck_release(kk); deck_release(vv); } while (0)
    PUT_I("edition_current", deck_sdi_info_edition());
    PUT_I("deck_os",         deck_sdi_info_deck_os());
    PUT_S("runtime",         deck_sdi_info_runtime_version());
    PUT_S("runtime_build",   "dev");
    PUT_I("sdi_major",       1);
    PUT_I("sdi_minor",       0);
    PUT_S("app_id",          info_app_field(c, "id"));
    PUT_S("app_version",     info_app_field(c, "version"));
    #undef PUT_I
    #undef PUT_S
    return m;
}
static deck_value_t *b_text_upper(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.upper expects str");
        return NULL;
    }
    uint32_t L = args[0]->as.s.len;
    if (L > (1U << 16)) { set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "text.upper: input > 64KB"); return NULL; }
    if (L == 0) return deck_new_str("", 0);
    char *buf = (char *)malloc(L + 1);
    if (!buf) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.upper alloc"); return NULL; }
    for (uint32_t i = 0; i < L; i++) {
        char ch = args[0]->as.s.ptr[i];
        buf[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    }
    deck_value_t *out = deck_new_str(buf, L);
    free(buf);
    return out;
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
    /* Spec §3: round(x, n) - rounds to n decimal places (returns float when n provided). */
    if (!is_num(args[0])) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "math.round needs number"); return NULL; }
    if (n >= 2) {
        if (!args[1] || args[1]->type != DECK_T_INT) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "math.round(x, n:int)"); return NULL; }
        int64_t places = args[1]->as.i;
        if (places < 0) places = 0;
        if (places > 12) places = 12;
        double x = args[0]->type == DECK_T_INT ? (double)args[0]->as.i : args[0]->as.f;
        double mult = 1.0;
        for (int64_t i = 0; i < places; i++) mult *= 10.0;
        return deck_new_float(round(x * mult) / mult);
    }
    if (args[0]->type == DECK_T_INT) return deck_new_int(args[0]->as.i);
    return deck_new_int((int64_t)round(args[0]->as.f));
}

/* ---- math.* completeness (concept #37, spec §3) ---- */

static double numd(deck_value_t *v) { return v->type == DECK_T_INT ? (double)v->as.i : v->as.f; }

#define MATH_UNARY(name, fn) \
    static deck_value_t *b_math_##name(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c) { \
        (void)n; \
        if (!is_num(a[0])) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "math." #name " needs number"); return NULL; } \
        return deck_new_float(fn(numd(a[0]))); \
    }

MATH_UNARY(sqrt, sqrt)
MATH_UNARY(sin,  sin)
MATH_UNARY(cos,  cos)
MATH_UNARY(tan,  tan)
MATH_UNARY(asin, asin)
MATH_UNARY(acos, acos)
MATH_UNARY(atan, atan)
MATH_UNARY(exp,  exp)
MATH_UNARY(ln,   log)
MATH_UNARY(log2, log2)
MATH_UNARY(log10,log10)
#undef MATH_UNARY

static deck_value_t *b_math_pow(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!is_num(a[0]) || !is_num(a[1])) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "math.pow needs numbers"); return NULL; }
    return deck_new_float(pow(numd(a[0]), numd(a[1])));
}

static deck_value_t *b_math_atan2(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!is_num(a[0]) || !is_num(a[1])) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "math.atan2 needs numbers"); return NULL; }
    return deck_new_float(atan2(numd(a[0]), numd(a[1])));
}

static deck_value_t *b_math_clamp(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!is_num(a[0]) || !is_num(a[1]) || !is_num(a[2])) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "math.clamp needs numbers"); return NULL; }
    double x = numd(a[0]), lo = numd(a[1]), hi = numd(a[2]);
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    if (a[0]->type == DECK_T_INT && a[1]->type == DECK_T_INT && a[2]->type == DECK_T_INT)
        return deck_new_int((int64_t)x);
    return deck_new_float(x);
}

static deck_value_t *b_math_lerp(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!is_num(a[0]) || !is_num(a[1]) || !is_num(a[2])) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "math.lerp needs numbers"); return NULL; }
    double A = numd(a[0]), B = numd(a[1]), t = numd(a[2]);
    return deck_new_float(A + (B - A) * t);
}

static deck_value_t *b_math_sign(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!is_num(a[0])) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "math.sign needs number"); return NULL; }
    double x = numd(a[0]);
    return deck_new_float(x > 0 ? 1.0 : x < 0 ? -1.0 : 0.0);
}

static deck_value_t *b_math_is_nan(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    if (!is_num(a[0])) return deck_retain(deck_false());
    if (a[0]->type == DECK_T_INT) return deck_retain(deck_false());
    return deck_retain(isnan(a[0]->as.f) ? deck_true() : deck_false());
}

static deck_value_t *b_math_is_inf(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    if (!is_num(a[0])) return deck_retain(deck_false());
    if (a[0]->type == DECK_T_INT) return deck_retain(deck_false());
    return deck_retain(isinf(a[0]->as.f) ? deck_true() : deck_false());
}

static deck_value_t *b_math_to_radians(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!is_num(a[0])) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "math.to_radians needs number"); return NULL; }
    return deck_new_float(numd(a[0]) * (M_PI / 180.0));
}

static deck_value_t *b_math_to_degrees(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!is_num(a[0])) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "math.to_degrees needs number"); return NULL; }
    return deck_new_float(numd(a[0]) * (180.0 / M_PI));
}

/* ---- int helpers ---- */
static int64_t int_or(deck_value_t *v, deck_interp_ctx_t *c, const char *who)
{
    if (!v || v->type != DECK_T_INT) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "%s needs int", who);
        return 0;
    }
    return v->as.i;
}

static deck_value_t *b_math_abs_int(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    int64_t x = int_or(a[0], c, "math.abs_int");
    if (c->err) return NULL;
    return deck_new_int(x < 0 ? -x : x);
}

static deck_value_t *b_math_min_int(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    int64_t x = int_or(a[0], c, "math.min_int"); if (c->err) return NULL;
    int64_t y = int_or(a[1], c, "math.min_int"); if (c->err) return NULL;
    return deck_new_int(x < y ? x : y);
}

static deck_value_t *b_math_max_int(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    int64_t x = int_or(a[0], c, "math.max_int"); if (c->err) return NULL;
    int64_t y = int_or(a[1], c, "math.max_int"); if (c->err) return NULL;
    return deck_new_int(x > y ? x : y);
}

static deck_value_t *b_math_clamp_int(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    int64_t x  = int_or(a[0], c, "math.clamp_int"); if (c->err) return NULL;
    int64_t lo = int_or(a[1], c, "math.clamp_int"); if (c->err) return NULL;
    int64_t hi = int_or(a[2], c, "math.clamp_int"); if (c->err) return NULL;
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    return deck_new_int(x);
}

static deck_value_t *b_math_gcd(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    int64_t x = int_or(a[0], c, "math.gcd"); if (c->err) return NULL;
    int64_t y = int_or(a[1], c, "math.gcd"); if (c->err) return NULL;
    if (x < 0) x = -x;
    if (y < 0) y = -y;
    while (y) { int64_t t = x % y; x = y; y = t; }
    return deck_new_int(x);
}

static deck_value_t *b_math_lcm(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    int64_t x = int_or(a[0], c, "math.lcm"); if (c->err) return NULL;
    int64_t y = int_or(a[1], c, "math.lcm"); if (c->err) return NULL;
    if (x == 0 || y == 0) return deck_new_int(0);
    int64_t ax = x < 0 ? -x : x;
    int64_t ay = y < 0 ? -y : y;
    int64_t g = ax, b = ay;
    while (b) { int64_t t = g % b; g = b; b = t; }
    return deck_new_int(ax / g * ay);
}

/* Zero-arity constants — picked up by AST_DOT's capability dispatch path
 * (interp.c around the build_cap_name block), which auto-calls 0-arity
 * builtins on bare `math.pi` / `math.e` / `math.tau` accesses. */
static deck_value_t *b_math_pi (deck_value_t **a, uint32_t n, deck_interp_ctx_t *c) { (void)a;(void)n;(void)c; return deck_new_float(M_PI); }
static deck_value_t *b_math_e  (deck_value_t **a, uint32_t n, deck_interp_ctx_t *c) { (void)a;(void)n;(void)c; return deck_new_float(M_E); }
static deck_value_t *b_math_tau(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c) { (void)a;(void)n;(void)c; return deck_new_float(2.0 * M_PI); }

/* ---- text.* ---- */
static deck_value_t *b_text_lower(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.lower expects str"); return NULL; }
    uint32_t L = args[0]->as.s.len;
    if (L > (1U << 16)) { set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "text.lower: input > 64KB"); return NULL; }
    if (L == 0) return deck_new_str("", 0);
    char *buf = (char *)malloc(L + 1);
    if (!buf) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.lower alloc"); return NULL; }
    for (uint32_t i = 0; i < L; i++) {
        char ch = args[0]->as.s.ptr[i];
        buf[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
    }
    deck_value_t *out = deck_new_str(buf, L);
    free(buf);
    return out;
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
        !args[1] || args[1]->type != DECK_T_STR) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.starts expects (str, str)"); return NULL; }
    if (args[1]->as.s.len > args[0]->as.s.len) return deck_retain(deck_false());
    return deck_retain(memcmp(args[0]->as.s.ptr, args[1]->as.s.ptr, args[1]->as.s.len) == 0 ? deck_true() : deck_false());
}
static deck_value_t *b_text_ends_with(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR ||
        !args[1] || args[1]->type != DECK_T_STR) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.ends expects (str, str)"); return NULL; }
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

/* Structural equality — used by list.contains, list.unique, map.has, etc.
 * Strings and atoms are interned, so pointer equality suffices.
 * Numeric mixed (int vs float) compares by promoted double value. */
static bool values_equal(deck_value_t *a, deck_value_t *b)
{
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->type != b->type) {
        if ((a->type == DECK_T_INT || a->type == DECK_T_FLOAT) &&
            (b->type == DECK_T_INT || b->type == DECK_T_FLOAT)) {
            double x = a->type == DECK_T_INT ? (double)a->as.i : a->as.f;
            double y = b->type == DECK_T_INT ? (double)b->as.i : b->as.f;
            return x == y;
        }
        return false;
    }
    switch (a->type) {
        case DECK_T_INT:   return a->as.i == b->as.i;
        case DECK_T_FLOAT: return a->as.f == b->as.f;
        case DECK_T_STR:   return a->as.s.ptr == b->as.s.ptr;
        case DECK_T_ATOM:  return a->as.atom == b->as.atom;
        case DECK_T_BOOL:  return a->as.b == b->as.b;
        case DECK_T_UNIT:  return true;
        case DECK_T_LIST: {
            if (a->as.list.len != b->as.list.len) return false;
            for (uint32_t i = 0; i < a->as.list.len; i++)
                if (!values_equal(a->as.list.items[i], b->as.list.items[i])) return false;
            return true;
        }
        case DECK_T_TUPLE: {
            if (a->as.tuple.arity != b->as.tuple.arity) return false;
            for (uint32_t i = 0; i < a->as.tuple.arity; i++)
                if (!values_equal(a->as.tuple.items[i], b->as.tuple.items[i])) return false;
            return true;
        }
        default: return a == b;
    }
}

/* ---- list.* completeness (concept #41, spec §11.2) ---- */

static deck_value_t *b_list_last(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.last(xs)"); return NULL; }
    if (args[0]->as.list.len == 0) return deck_new_none();
    return deck_new_some(args[0]->as.list.items[args[0]->as.list.len - 1]);
}

static deck_value_t *b_list_append(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.append(xs, item)"); return NULL; }
    uint32_t L = args[0]->as.list.len;
    deck_value_t *out = deck_new_list(L + 1);
    if (!out) return NULL;
    for (uint32_t i = 0; i < L; i++) deck_list_push(out, args[0]->as.list.items[i]);
    deck_list_push(out, args[1]);
    return out;
}

static deck_value_t *b_list_prepend(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[1] || args[1]->type != DECK_T_LIST) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.prepend(item, xs)"); return NULL; }
    uint32_t L = args[1]->as.list.len;
    deck_value_t *out = deck_new_list(L + 1);
    if (!out) return NULL;
    deck_list_push(out, args[0]);
    for (uint32_t i = 0; i < L; i++) deck_list_push(out, args[1]->as.list.items[i]);
    return out;
}

static deck_value_t *b_list_reverse(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.reverse(xs)"); return NULL; }
    uint32_t L = args[0]->as.list.len;
    deck_value_t *out = deck_new_list(L);
    if (!out) return NULL;
    for (uint32_t i = 0; i < L; i++) deck_list_push(out, args[0]->as.list.items[L - 1 - i]);
    return out;
}

static deck_value_t *b_list_take(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST || !args[1] || args[1]->type != DECK_T_INT) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.take(xs, int)"); return NULL;
    }
    int64_t k = args[1]->as.i;
    uint32_t L = args[0]->as.list.len;
    if (k < 0) k = 0;
    if ((uint64_t)k > L) k = L;
    deck_value_t *out = deck_new_list((uint32_t)k);
    if (!out) return NULL;
    for (uint32_t i = 0; i < (uint32_t)k; i++) deck_list_push(out, args[0]->as.list.items[i]);
    return out;
}

static deck_value_t *b_list_drop(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST || !args[1] || args[1]->type != DECK_T_INT) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.drop(xs, int)"); return NULL;
    }
    int64_t k = args[1]->as.i;
    uint32_t L = args[0]->as.list.len;
    if (k < 0) k = 0;
    if ((uint64_t)k > L) k = L;
    deck_value_t *out = deck_new_list(L - (uint32_t)k);
    if (!out) return NULL;
    for (uint32_t i = (uint32_t)k; i < L; i++) deck_list_push(out, args[0]->as.list.items[i]);
    return out;
}

static deck_value_t *b_list_contains(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    if (!args[0] || args[0]->type != DECK_T_LIST) return deck_retain(deck_false());
    for (uint32_t i = 0; i < args[0]->as.list.len; i++) {
        if (values_equal(args[0]->as.list.items[i], args[1])) return deck_retain(deck_true());
    }
    return deck_retain(deck_false());
}

static deck_value_t *b_list_find(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST || !args[1] || args[1]->type != DECK_T_FN) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.find(xs, fn)"); return NULL;
    }
    for (uint32_t i = 0; i < args[0]->as.list.len; i++) {
        deck_value_t *ca[1] = { args[0]->as.list.items[i] };
        deck_value_t *r = call_fn_value_c(c, args[1], ca, 1);
        if (!r) return NULL;
        bool keep = deck_is_truthy(r);
        deck_release(r);
        if (keep) return deck_new_some(args[0]->as.list.items[i]);
    }
    return deck_new_none();
}

static deck_value_t *b_list_find_index(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST || !args[1] || args[1]->type != DECK_T_FN) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.find_index(xs, fn)"); return NULL;
    }
    for (uint32_t i = 0; i < args[0]->as.list.len; i++) {
        deck_value_t *ca[1] = { args[0]->as.list.items[i] };
        deck_value_t *r = call_fn_value_c(c, args[1], ca, 1);
        if (!r) return NULL;
        bool keep = deck_is_truthy(r);
        deck_release(r);
        if (keep) return deck_new_some(deck_new_int((int64_t)i));
    }
    return deck_new_none();
}

static deck_value_t *b_list_count_where(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST || !args[1] || args[1]->type != DECK_T_FN) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.count_where(xs, fn)"); return NULL;
    }
    int64_t cnt = 0;
    for (uint32_t i = 0; i < args[0]->as.list.len; i++) {
        deck_value_t *ca[1] = { args[0]->as.list.items[i] };
        deck_value_t *r = call_fn_value_c(c, args[1], ca, 1);
        if (!r) return NULL;
        if (deck_is_truthy(r)) cnt++;
        deck_release(r);
    }
    return deck_new_int(cnt);
}

static deck_value_t *b_list_any(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST || !args[1] || args[1]->type != DECK_T_FN) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.any(xs, fn)"); return NULL;
    }
    for (uint32_t i = 0; i < args[0]->as.list.len; i++) {
        deck_value_t *ca[1] = { args[0]->as.list.items[i] };
        deck_value_t *r = call_fn_value_c(c, args[1], ca, 1);
        if (!r) return NULL;
        bool keep = deck_is_truthy(r);
        deck_release(r);
        if (keep) return deck_retain(deck_true());
    }
    return deck_retain(deck_false());
}

static deck_value_t *b_list_all(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST || !args[1] || args[1]->type != DECK_T_FN) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.all(xs, fn)"); return NULL;
    }
    for (uint32_t i = 0; i < args[0]->as.list.len; i++) {
        deck_value_t *ca[1] = { args[0]->as.list.items[i] };
        deck_value_t *r = call_fn_value_c(c, args[1], ca, 1);
        if (!r) return NULL;
        bool keep = deck_is_truthy(r);
        deck_release(r);
        if (!keep) return deck_retain(deck_false());
    }
    return deck_retain(deck_true());
}

static deck_value_t *b_list_none(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST || !args[1] || args[1]->type != DECK_T_FN) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.none(xs, fn)"); return NULL;
    }
    for (uint32_t i = 0; i < args[0]->as.list.len; i++) {
        deck_value_t *ca[1] = { args[0]->as.list.items[i] };
        deck_value_t *r = call_fn_value_c(c, args[1], ca, 1);
        if (!r) return NULL;
        bool keep = deck_is_truthy(r);
        deck_release(r);
        if (keep) return deck_retain(deck_false());
    }
    return deck_retain(deck_true());
}

static deck_value_t *b_list_sum(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.sum([int])"); return NULL; }
    int64_t s = 0;
    for (uint32_t i = 0; i < args[0]->as.list.len; i++) {
        deck_value_t *v = args[0]->as.list.items[i];
        if (!v || v->type != DECK_T_INT) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.sum: non-int at %u", (unsigned)i); return NULL; }
        s += v->as.i;
    }
    return deck_new_int(s);
}

static deck_value_t *b_list_sum_f(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.sum_f([float])"); return NULL; }
    double s = 0;
    for (uint32_t i = 0; i < args[0]->as.list.len; i++) {
        deck_value_t *v = args[0]->as.list.items[i];
        if (!v || (v->type != DECK_T_FLOAT && v->type != DECK_T_INT)) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.sum_f: non-number at %u", (unsigned)i); return NULL; }
        s += v->type == DECK_T_INT ? (double)v->as.i : v->as.f;
    }
    return deck_new_float(s);
}

static deck_value_t *b_list_avg(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.avg([float])"); return NULL; }
    uint32_t L = args[0]->as.list.len;
    if (L == 0) return deck_new_none();
    double s = 0;
    for (uint32_t i = 0; i < L; i++) {
        deck_value_t *v = args[0]->as.list.items[i];
        if (!v || (v->type != DECK_T_FLOAT && v->type != DECK_T_INT)) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.avg: non-number at %u", (unsigned)i); return NULL; }
        s += v->type == DECK_T_INT ? (double)v->as.i : v->as.f;
    }
    return deck_new_some(deck_new_float(s / L));
}

/* ---- list.* pass 2 (concept #43) ---- */

/* list.enumerate — returns [(index, value)]. */
static deck_value_t *b_list_enumerate(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.enumerate(xs)"); return NULL; }
    uint32_t L = args[0]->as.list.len;
    deck_value_t *out = deck_new_list(L);
    if (!out) return NULL;
    for (uint32_t i = 0; i < L; i++) {
        deck_value_t *idx = deck_new_int((int64_t)i);
        deck_value_t *items[2] = { idx, args[0]->as.list.items[i] };
        deck_value_t *t = deck_new_tuple(items, 2);
        deck_release(idx);
        if (!t) { deck_release(out); return NULL; }
        deck_list_push(out, t); deck_release(t);
    }
    return out;
}

/* list.zip(a, b) — pairs up elements; truncates to min length. */
static deck_value_t *b_list_zip(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST ||
        !args[1] || args[1]->type != DECK_T_LIST) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.zip(a, b)"); return NULL;
    }
    uint32_t L = args[0]->as.list.len < args[1]->as.list.len ? args[0]->as.list.len : args[1]->as.list.len;
    deck_value_t *out = deck_new_list(L);
    if (!out) return NULL;
    for (uint32_t i = 0; i < L; i++) {
        deck_value_t *items[2] = { args[0]->as.list.items[i], args[1]->as.list.items[i] };
        deck_value_t *t = deck_new_tuple(items, 2);
        if (!t) { deck_release(out); return NULL; }
        deck_list_push(out, t); deck_release(t);
    }
    return out;
}

/* list.zip_with(a, b, fn) — combine pairs via user fn. */
static deck_value_t *b_list_zip_with(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST ||
        !args[1] || args[1]->type != DECK_T_LIST ||
        !args[2] || args[2]->type != DECK_T_FN) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.zip_with(a, b, fn)"); return NULL;
    }
    uint32_t L = args[0]->as.list.len < args[1]->as.list.len ? args[0]->as.list.len : args[1]->as.list.len;
    deck_value_t *out = deck_new_list(L);
    if (!out) return NULL;
    for (uint32_t i = 0; i < L; i++) {
        deck_value_t *ca[2] = { args[0]->as.list.items[i], args[1]->as.list.items[i] };
        deck_value_t *r = call_fn_value_c(c, args[2], ca, 2);
        if (!r) { deck_release(out); return NULL; }
        deck_list_push(out, r); deck_release(r);
    }
    return out;
}

/* list.flat_map(xs, fn) — map then flatten one level. */
static deck_value_t *b_list_flat_map(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST || !args[1] || args[1]->type != DECK_T_FN) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.flat_map(xs, fn)"); return NULL;
    }
    deck_value_t *out = deck_new_list(args[0]->as.list.len * 2);
    if (!out) return NULL;
    for (uint32_t i = 0; i < args[0]->as.list.len; i++) {
        deck_value_t *ca[1] = { args[0]->as.list.items[i] };
        deck_value_t *r = call_fn_value_c(c, args[1], ca, 1);
        if (!r) { deck_release(out); return NULL; }
        if (r->type != DECK_T_LIST) {
            deck_release(r); deck_release(out);
            set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.flat_map: fn must return list");
            return NULL;
        }
        for (uint32_t j = 0; j < r->as.list.len; j++) deck_list_push(out, r->as.list.items[j]);
        deck_release(r);
    }
    return out;
}

/* list.partition(xs, fn) — returns ([keep], [drop]) as a 2-tuple. */
static deck_value_t *b_list_partition(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST || !args[1] || args[1]->type != DECK_T_FN) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.partition(xs, fn)"); return NULL;
    }
    deck_value_t *yes = deck_new_list(4);
    deck_value_t *no  = deck_new_list(4);
    if (!yes || !no) { if (yes) deck_release(yes); if (no) deck_release(no); return NULL; }
    for (uint32_t i = 0; i < args[0]->as.list.len; i++) {
        deck_value_t *ca[1] = { args[0]->as.list.items[i] };
        deck_value_t *r = call_fn_value_c(c, args[1], ca, 1);
        if (!r) { deck_release(yes); deck_release(no); return NULL; }
        bool keep = deck_is_truthy(r);
        deck_release(r);
        deck_list_push(keep ? yes : no, args[0]->as.list.items[i]);
    }
    deck_value_t *items[2] = { yes, no };
    deck_value_t *t = deck_new_tuple(items, 2);
    deck_release(yes); deck_release(no);
    return t;
}

/* list.unique — returns [T] with duplicates removed (first occurrence wins).
 * O(n²) via values_equal; fine for small lists. */
static deck_value_t *b_list_unique(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.unique(xs)"); return NULL; }
    uint32_t L = args[0]->as.list.len;
    deck_value_t *out = deck_new_list(L);
    if (!out) return NULL;
    for (uint32_t i = 0; i < L; i++) {
        deck_value_t *v = args[0]->as.list.items[i];
        bool dup = false;
        for (uint32_t j = 0; j < out->as.list.len; j++) {
            if (values_equal(out->as.list.items[j], v)) { dup = true; break; }
        }
        if (!dup) deck_list_push(out, v);
    }
    return out;
}

/* list.sort — natural ordering on int/float/str. Errors on mixed types.
 * Uses qsort. Since elements are deck_value_t *, sort key extraction is
 * done inside cmp via the first element of the list's type. */
static deck_type_t sort_type;
static int sort_cmp(const void *a, const void *b)
{
    deck_value_t *x = *(deck_value_t * const *)a;
    deck_value_t *y = *(deck_value_t * const *)b;
    switch (sort_type) {
        case DECK_T_INT: return x->as.i < y->as.i ? -1 : x->as.i > y->as.i ? 1 : 0;
        case DECK_T_FLOAT: return x->as.f < y->as.f ? -1 : x->as.f > y->as.f ? 1 : 0;
        case DECK_T_STR: {
            uint32_t m = x->as.s.len < y->as.s.len ? x->as.s.len : y->as.s.len;
            int r = memcmp(x->as.s.ptr, y->as.s.ptr, m);
            if (r != 0) return r;
            return x->as.s.len < y->as.s.len ? -1 : x->as.s.len > y->as.s.len ? 1 : 0;
        }
        default: return 0;
    }
}

static deck_value_t *b_list_sort(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.sort(xs)"); return NULL; }
    uint32_t L = args[0]->as.list.len;
    if (L <= 1) {
        /* Copy to keep immutability semantics. */
        deck_value_t *out = deck_new_list(L);
        if (!out) return NULL;
        for (uint32_t i = 0; i < L; i++) deck_list_push(out, args[0]->as.list.items[i]);
        return out;
    }
    sort_type = args[0]->as.list.items[0]->type;
    if (sort_type != DECK_T_INT && sort_type != DECK_T_FLOAT && sort_type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.sort: only [int] / [float] / [str] supported");
        return NULL;
    }
    for (uint32_t i = 1; i < L; i++) {
        if (args[0]->as.list.items[i]->type != sort_type) {
            set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.sort: mixed element types at index %u", (unsigned)i);
            return NULL;
        }
    }
    deck_value_t *out = deck_new_list(L);
    if (!out) return NULL;
    deck_value_t **buf = malloc(sizeof(deck_value_t *) * L);
    if (!buf) { deck_release(out); set_err(c, DECK_RT_NO_MEMORY, 0, 0, "list.sort alloc"); return NULL; }
    for (uint32_t i = 0; i < L; i++) buf[i] = args[0]->as.list.items[i];
    qsort(buf, L, sizeof(deck_value_t *), sort_cmp);
    for (uint32_t i = 0; i < L; i++) deck_list_push(out, buf[i]);
    free(buf);
    return out;
}

/* list.min_by / list.max_by — fold returning the element with min/max fn(elem). */
static deck_value_t *list_extrema_by(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c, bool want_max)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST || !args[1] || args[1]->type != DECK_T_FN) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.min_by/max_by(xs, fn)"); return NULL;
    }
    if (args[0]->as.list.len == 0) return deck_new_none();
    double best_k = 0;
    deck_value_t *best_v = NULL;
    for (uint32_t i = 0; i < args[0]->as.list.len; i++) {
        deck_value_t *ca[1] = { args[0]->as.list.items[i] };
        deck_value_t *r = call_fn_value_c(c, args[1], ca, 1);
        if (!r) return NULL;
        double k;
        if (r->type == DECK_T_INT) k = (double)r->as.i;
        else if (r->type == DECK_T_FLOAT) k = r->as.f;
        else { deck_release(r); set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.min_by/max_by: fn must return number"); return NULL; }
        deck_release(r);
        if (!best_v || (want_max ? k > best_k : k < best_k)) {
            best_k = k;
            best_v = args[0]->as.list.items[i];
        }
    }
    return deck_new_some(best_v);
}
static deck_value_t *b_list_min_by(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c) { return list_extrema_by(a, n, c, false); }
static deck_value_t *b_list_max_by(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c) { return list_extrema_by(a, n, c, true); }

static deck_value_t *b_list_flatten(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.flatten([[T]])"); return NULL; }
    uint32_t total = 0;
    for (uint32_t i = 0; i < args[0]->as.list.len; i++) {
        deck_value_t *v = args[0]->as.list.items[i];
        if (!v || v->type != DECK_T_LIST) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "list.flatten: non-list at %u", (unsigned)i); return NULL; }
        total += v->as.list.len;
    }
    deck_value_t *out = deck_new_list(total);
    if (!out) return NULL;
    for (uint32_t i = 0; i < args[0]->as.list.len; i++) {
        deck_value_t *sub = args[0]->as.list.items[i];
        for (uint32_t j = 0; j < sub->as.list.len; j++) deck_list_push(out, sub->as.list.items[j]);
    }
    return out;
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

/* ---- map.* completeness (concept #42, spec §11.3) ---- */

static deck_value_t *b_map_delete(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_MAP) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "map.delete(m, k)"); return NULL; }
    deck_value_t *m = args[0];
    deck_value_t *out = deck_new_map(m->as.map.cap > 0 ? m->as.map.cap : 4);
    if (!out) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "map.delete alloc"); return NULL; }
    for (uint32_t i = 0; i < m->as.map.len; i++) {
        if (!m->as.map.entries[i].used) continue;
        deck_value_t *k = m->as.map.entries[i].key;
        if (values_equal(k, args[1])) continue;
        deck_map_put(out, k, m->as.map.entries[i].val);
    }
    return out;
}

static deck_value_t *b_map_has(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    if (!args[0] || args[0]->type != DECK_T_MAP) return deck_retain(deck_false());
    deck_value_t *v = deck_map_get(args[0], args[1]);
    return deck_retain(v ? deck_true() : deck_false());
}

static deck_value_t *b_map_merge(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_MAP ||
        !args[1] || args[1]->type != DECK_T_MAP) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "map.merge(a, b)"); return NULL;
    }
    uint32_t cap = args[0]->as.map.cap + args[1]->as.map.cap;
    if (cap < 4) cap = 4;
    deck_value_t *out = deck_new_map(cap);
    if (!out) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "map.merge alloc"); return NULL; }
    for (uint32_t i = 0; i < args[0]->as.map.len; i++) {
        if (args[0]->as.map.entries[i].used)
            deck_map_put(out, args[0]->as.map.entries[i].key, args[0]->as.map.entries[i].val);
    }
    /* Right-biased: b's values overwrite a's on key conflict. */
    for (uint32_t i = 0; i < args[1]->as.map.len; i++) {
        if (args[1]->as.map.entries[i].used)
            deck_map_put(out, args[1]->as.map.entries[i].key, args[1]->as.map.entries[i].val);
    }
    return out;
}

static deck_value_t *b_map_is_empty(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    if (!args[0] || args[0]->type != DECK_T_MAP) return deck_retain(deck_true());
    for (uint32_t i = 0; i < args[0]->as.map.len; i++)
        if (args[0]->as.map.entries[i].used) return deck_retain(deck_false());
    return deck_retain(deck_true());
}

static deck_value_t *b_map_map_values(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_MAP || !args[1] || args[1]->type != DECK_T_FN) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "map.map_values(m, fn)"); return NULL;
    }
    deck_value_t *out = deck_new_map(args[0]->as.map.cap > 0 ? args[0]->as.map.cap : 4);
    if (!out) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "map.map_values alloc"); return NULL; }
    for (uint32_t i = 0; i < args[0]->as.map.len; i++) {
        if (!args[0]->as.map.entries[i].used) continue;
        deck_value_t *ca[1] = { args[0]->as.map.entries[i].val };
        deck_value_t *r = call_fn_value_c(c, args[1], ca, 1);
        if (!r) { deck_release(out); return NULL; }
        deck_map_put(out, args[0]->as.map.entries[i].key, r);
        deck_release(r);
    }
    return out;
}

static deck_value_t *b_map_filter(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_MAP || !args[1] || args[1]->type != DECK_T_FN) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "map.filter(m, fn)"); return NULL;
    }
    deck_value_t *out = deck_new_map(args[0]->as.map.cap > 0 ? args[0]->as.map.cap : 4);
    if (!out) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "map.filter alloc"); return NULL; }
    for (uint32_t i = 0; i < args[0]->as.map.len; i++) {
        if (!args[0]->as.map.entries[i].used) continue;
        deck_value_t *ca[2] = { args[0]->as.map.entries[i].key, args[0]->as.map.entries[i].val };
        deck_value_t *r = call_fn_value_c(c, args[1], ca, 2);
        if (!r) { deck_release(out); return NULL; }
        bool keep = deck_is_truthy(r);
        deck_release(r);
        if (keep)
            deck_map_put(out, args[0]->as.map.entries[i].key, args[0]->as.map.entries[i].val);
    }
    return out;
}

/* map.to_list: emit [(k, v)] tuples. */
static deck_value_t *b_map_to_list(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_MAP) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "map.to_list(m)"); return NULL; }
    deck_value_t *out = deck_new_list(args[0]->as.map.len);
    if (!out) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "map.to_list alloc"); return NULL; }
    for (uint32_t i = 0; i < args[0]->as.map.len; i++) {
        if (!args[0]->as.map.entries[i].used) continue;
        deck_value_t *items[2] = { args[0]->as.map.entries[i].key, args[0]->as.map.entries[i].val };
        deck_value_t *t = deck_new_tuple(items, 2);
        if (!t) { deck_release(out); return NULL; }
        deck_list_push(out, t); deck_release(t);
    }
    return out;
}

/* map.from_list: accept [(k, v)] and build a map. */
static deck_value_t *b_map_from_list(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "map.from_list([(k,v)])"); return NULL; }
    deck_value_t *out = deck_new_map(args[0]->as.list.len + 4);
    if (!out) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "map.from_list alloc"); return NULL; }
    for (uint32_t i = 0; i < args[0]->as.list.len; i++) {
        deck_value_t *t = args[0]->as.list.items[i];
        if (!t || t->type != DECK_T_TUPLE || t->as.tuple.arity != 2) {
            deck_release(out);
            set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "map.from_list: element %u not (k, v)", (unsigned)i);
            return NULL;
        }
        deck_map_put(out, t->as.tuple.items[0], t->as.tuple.items[1]);
    }
    return out;
}

/* ---- tup.* (concept #42, spec §11.4) ---- */

static deck_value_t *b_tup_fst(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_TUPLE || args[0]->as.tuple.arity < 2) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "tup.fst((A, B))"); return NULL;
    }
    return deck_retain(args[0]->as.tuple.items[0]);
}

static deck_value_t *b_tup_snd(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_TUPLE || args[0]->as.tuple.arity < 2) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "tup.snd((A, B))"); return NULL;
    }
    return deck_retain(args[0]->as.tuple.items[1]);
}

static deck_value_t *b_tup_third(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_TUPLE || args[0]->as.tuple.arity < 3) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "tup.third((A, B, C))"); return NULL;
    }
    return deck_retain(args[0]->as.tuple.items[2]);
}

static deck_value_t *b_tup_swap(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_TUPLE || args[0]->as.tuple.arity != 2) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "tup.swap((A, B))"); return NULL;
    }
    deck_value_t *items[2] = { args[0]->as.tuple.items[1], args[0]->as.tuple.items[0] };
    return deck_new_tuple(items, 2);
}

static deck_value_t *b_tup_map_fst(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_TUPLE || args[0]->as.tuple.arity != 2 ||
        !args[1] || args[1]->type != DECK_T_FN) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "tup.map_fst((A, B), fn)"); return NULL;
    }
    deck_value_t *ca[1] = { args[0]->as.tuple.items[0] };
    deck_value_t *mapped = call_fn_value_c(c, args[1], ca, 1);
    if (!mapped) return NULL;
    deck_value_t *items[2] = { mapped, args[0]->as.tuple.items[1] };
    deck_value_t *t = deck_new_tuple(items, 2);
    deck_release(mapped);
    return t;
}

static deck_value_t *b_tup_map_snd(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_TUPLE || args[0]->as.tuple.arity != 2 ||
        !args[1] || args[1]->type != DECK_T_FN) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "tup.map_snd((A, B), fn)"); return NULL;
    }
    deck_value_t *ca[1] = { args[0]->as.tuple.items[1] };
    deck_value_t *mapped = call_fn_value_c(c, args[1], ca, 1);
    if (!mapped) return NULL;
    deck_value_t *items[2] = { args[0]->as.tuple.items[0], mapped };
    deck_value_t *t = deck_new_tuple(items, 2);
    deck_release(mapped);
    return t;
}

/* ---- bytes.* (concept #40 — spec §3 @builtin bytes) ----
 * Accepts DECK_T_BYTES (the dedicated buffer value) or DECK_T_LIST of ints
 * (the byte-literal shape `[0xDE, 0xAD]`). Outputs are emitted as DECK_T_LIST
 * for interop with text.hex_* / fs.read_bytes — unifies the byte
 * representation at this layer. */

/* Materialize a [byte]-shaped value into a heap buffer. Returns false with
 * a set error on type mismatch. Caller frees `*out_buf`. */
static bool bytes_materialize(deck_value_t *v, uint8_t **out_buf, uint32_t *out_len,
                              deck_interp_ctx_t *c, const char *who)
{
    if (!v) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "%s expects [byte] or bytes", who); return false; }
    if (v->type == DECK_T_BYTES) {
        uint32_t L = v->as.bytes.len;
        uint8_t *b = L > 0 ? (uint8_t *)malloc(L) : NULL;
        if (L > 0 && !b) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "%s alloc", who); return false; }
        if (L > 0) memcpy(b, v->as.bytes.buf, L);
        *out_buf = b; *out_len = L;
        return true;
    }
    if (v->type == DECK_T_LIST) {
        uint32_t L = v->as.list.len;
        uint8_t *b = L > 0 ? (uint8_t *)malloc(L) : NULL;
        if (L > 0 && !b) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "%s alloc", who); return false; }
        for (uint32_t i = 0; i < L; i++) {
            deck_value_t *e = v->as.list.items[i];
            if (!e || e->type != DECK_T_INT) { free(b); set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "%s: list[%u] not int", who, (unsigned)i); return false; }
            int64_t x = e->as.i;
            if (x < 0 || x > 255) { free(b); set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "%s: byte out of range", who); return false; }
            b[i] = (uint8_t)x;
        }
        *out_buf = b; *out_len = L;
        return true;
    }
    set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "%s expects [byte] or bytes", who);
    return false;
}

/* Wrap a heap buffer into a DECK_T_LIST of ints. Takes ownership of `buf`
 * via copying — caller still frees on the way out. */
static deck_value_t *bytes_to_list(const uint8_t *buf, uint32_t L, deck_interp_ctx_t *c)
{
    deck_value_t *out = deck_new_list(L);
    if (!out) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "bytes output alloc"); return NULL; }
    for (uint32_t i = 0; i < L; i++) {
        deck_value_t *b = deck_new_int(buf[i]);
        deck_list_push(out, b); deck_release(b);
    }
    return out;
}

static deck_value_t *b_bytes_len(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0]) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "bytes.len expects [byte] or bytes"); return NULL; }
    if (args[0]->type == DECK_T_BYTES) return deck_new_int((int64_t)args[0]->as.bytes.len);
    if (args[0]->type == DECK_T_LIST)  return deck_new_int((int64_t)args[0]->as.list.len);
    set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "bytes.len expects [byte] or bytes"); return NULL;
}

static deck_value_t *b_bytes_concat(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    uint8_t *a = NULL, *b = NULL; uint32_t aL = 0, bL = 0;
    if (!bytes_materialize(args[0], &a, &aL, c, "bytes.concat")) return NULL;
    if (!bytes_materialize(args[1], &b, &bL, c, "bytes.concat")) { free(a); return NULL; }
    uint64_t total = (uint64_t)aL + bL;
    if (total > (1U << 15)) { free(a); free(b); set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "bytes.concat: > 32KB"); return NULL; }
    uint8_t *merged = total > 0 ? (uint8_t *)malloc((size_t)total) : NULL;
    if (total > 0 && !merged) { free(a); free(b); set_err(c, DECK_RT_NO_MEMORY, 0, 0, "bytes.concat alloc"); return NULL; }
    if (aL > 0) memcpy(merged, a, aL);
    if (bL > 0) memcpy(merged + aL, b, bL);
    deck_value_t *out = bytes_to_list(merged, (uint32_t)total, c);
    free(a); free(b); free(merged);
    return out;
}

static deck_value_t *b_bytes_slice(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[1] || args[1]->type != DECK_T_INT || !args[2] || args[2]->type != DECK_T_INT) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "bytes.slice(b, int, int)"); return NULL;
    }
    uint8_t *b = NULL; uint32_t L = 0;
    if (!bytes_materialize(args[0], &b, &L, c, "bytes.slice")) return NULL;
    int64_t s = args[1]->as.i, e = args[2]->as.i;
    if (s < 0) s += L;
    if (e < 0) e += L;
    if (s < 0) s = 0;
    if (e > (int64_t)L) e = L;
    if (s >= e) { free(b); return deck_new_list(0); }
    deck_value_t *out = bytes_to_list(b + s, (uint32_t)(e - s), c);
    free(b);
    return out;
}

static deck_value_t *b_bytes_to_int_be(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    uint8_t *b = NULL; uint32_t L = 0;
    if (!bytes_materialize(args[0], &b, &L, c, "bytes.to_int_be")) return NULL;
    if (L > 8) { free(b); set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "bytes.to_int_be: > 8 bytes"); return NULL; }
    int64_t acc = 0;
    for (uint32_t i = 0; i < L; i++) acc = (acc << 8) | b[i];
    free(b);
    return deck_new_int(acc);
}

static deck_value_t *b_bytes_to_int_le(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    uint8_t *b = NULL; uint32_t L = 0;
    if (!bytes_materialize(args[0], &b, &L, c, "bytes.to_int_le")) return NULL;
    if (L > 8) { free(b); set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "bytes.to_int_le: > 8 bytes"); return NULL; }
    int64_t acc = 0;
    for (int32_t i = (int32_t)L - 1; i >= 0; i--) acc = (acc << 8) | b[i];
    free(b);
    return deck_new_int(acc);
}

/* bytes.from_int(n, size, endian:atom). endian = :be | :le. */
static deck_value_t *b_bytes_from_int(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_INT ||
        !args[1] || args[1]->type != DECK_T_INT ||
        !args[2] || args[2]->type != DECK_T_ATOM) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "bytes.from_int(int, size:int, :be|:le)"); return NULL;
    }
    int64_t v = args[0]->as.i;
    int64_t size = args[1]->as.i;
    if (size < 1 || size > 8) { set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "bytes.from_int: size 1..8"); return NULL; }
    bool be;
    if (strcmp(args[2]->as.atom, "be") == 0) be = true;
    else if (strcmp(args[2]->as.atom, "le") == 0) be = false;
    else { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "bytes.from_int: endian must be :be or :le"); return NULL; }
    uint8_t buf[8];
    uint64_t u = (uint64_t)v;
    if (be) {
        for (int64_t i = size - 1; i >= 0; i--) { buf[i] = (uint8_t)(u & 0xFF); u >>= 8; }
    } else {
        for (int64_t i = 0; i < size; i++) { buf[i] = (uint8_t)(u & 0xFF); u >>= 8; }
    }
    return bytes_to_list(buf, (uint32_t)size, c);
}

/* XOR: paired-byte XOR of equal-length inputs; shorter input cycles.
 * Spec §3 implies equal length — return empty when shapes mismatch badly,
 * caller's responsibility to align. */
static deck_value_t *b_bytes_xor(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    uint8_t *a = NULL, *b = NULL; uint32_t aL = 0, bL = 0;
    if (!bytes_materialize(args[0], &a, &aL, c, "bytes.xor")) return NULL;
    if (!bytes_materialize(args[1], &b, &bL, c, "bytes.xor")) { free(a); return NULL; }
    if (bL == 0) { free(a); free(b); return deck_new_list(0); }
    uint8_t *out = aL > 0 ? (uint8_t *)malloc(aL) : NULL;
    if (aL > 0 && !out) { free(a); free(b); set_err(c, DECK_RT_NO_MEMORY, 0, 0, "bytes.xor alloc"); return NULL; }
    for (uint32_t i = 0; i < aL; i++) out[i] = a[i] ^ b[i % bL];
    deck_value_t *r = bytes_to_list(out, aL, c);
    free(a); free(b); free(out);
    return r;
}

static deck_value_t *b_bytes_fill(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_INT ||
        !args[1] || args[1]->type != DECK_T_INT) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "bytes.fill(byte:int, count:int)"); return NULL;
    }
    int64_t val = args[0]->as.i;
    int64_t count = args[1]->as.i;
    if (val < 0 || val > 255) { set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "bytes.fill: byte 0..255"); return NULL; }
    if (count < 0) count = 0;
    if (count > (1 << 15)) { set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "bytes.fill: count > 32KB"); return NULL; }
    deck_value_t *out = deck_new_list((uint32_t)count);
    if (!out) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "bytes.fill alloc"); return NULL; }
    for (int64_t i = 0; i < count; i++) {
        deck_value_t *b = deck_new_int(val);
        deck_list_push(out, b); deck_release(b);
    }
    return out;
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

/* ---- time.* completeness (concept #32, spec §3) ----
 * Canonical units: Timestamp = epoch seconds (int), Duration = seconds (int).
 * Duration literals (5s, 2m, 1h, 1d) are lowered to seconds by the lexer;
 * `ms` suffix is truncated since the canonical precision is seconds. */

static int64_t ts_or_err(deck_value_t *v, deck_interp_ctx_t *c, const char *who)
{
    if (!is_num(v)) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "%s needs Timestamp", who); return INT64_MIN; }
    return v->type == DECK_T_INT ? v->as.i : (int64_t)v->as.f;
}

static deck_value_t *b_time_since(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    int64_t t = ts_or_err(a[0], c, "time.since");
    if (c->err) return NULL;
    int64_t now_s = deck_sdi_time_wall_epoch_s();
    if (now_s <= 0) now_s = deck_sdi_time_monotonic_us() / 1000000;
    return deck_new_int(now_s - t);
}

static deck_value_t *b_time_until(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    int64_t t = ts_or_err(a[0], c, "time.until");
    if (c->err) return NULL;
    int64_t now_s = deck_sdi_time_wall_epoch_s();
    if (now_s <= 0) now_s = deck_sdi_time_monotonic_us() / 1000000;
    return deck_new_int(t - now_s);
}

static deck_value_t *b_time_add(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    int64_t t = ts_or_err(a[0], c, "time.add");
    if (c->err) return NULL;
    int64_t d = ts_or_err(a[1], c, "time.add");
    if (c->err) return NULL;
    return deck_new_int(t + d);
}

static deck_value_t *b_time_sub(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    int64_t t = ts_or_err(a[0], c, "time.sub");
    if (c->err) return NULL;
    int64_t d = ts_or_err(a[1], c, "time.sub");
    if (c->err) return NULL;
    return deck_new_int(t - d);
}

static deck_value_t *b_time_before(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    int64_t x = ts_or_err(a[0], c, "time.before");
    if (c->err) return NULL;
    int64_t y = ts_or_err(a[1], c, "time.before");
    if (c->err) return NULL;
    return deck_retain(x < y ? deck_true() : deck_false());
}

static deck_value_t *b_time_after(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    int64_t x = ts_or_err(a[0], c, "time.after");
    if (c->err) return NULL;
    int64_t y = ts_or_err(a[1], c, "time.after");
    if (c->err) return NULL;
    return deck_retain(x > y ? deck_true() : deck_false());
}

static deck_value_t *b_time_epoch(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)a; (void)n; (void)c;
    return deck_new_int(0);
}

/* Format a Timestamp using strftime-compatible template. Cap 128 bytes out. */
static deck_value_t *b_time_format(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    int64_t t = ts_or_err(a[0], c, "time.format");
    if (c->err) return NULL;
    if (!a[1] || a[1]->type != DECK_T_STR) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "time.format(t, fmt:str)"); return NULL; }
    char fmt[64]; uint32_t L = a[1]->as.s.len < 63 ? a[1]->as.s.len : 63;
    memcpy(fmt, a[1]->as.s.ptr, L); fmt[L] = 0;
    time_t tt = (time_t)t;
    struct tm tm; gmtime_r(&tt, &tm);
    char buf[128];
    size_t k = strftime(buf, sizeof(buf), fmt, &tm);
    return deck_new_str(buf, (uint32_t)k);
}

/* Parse a Timestamp from a formatted string using strptime. :none on failure. */
static deck_value_t *b_time_parse(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    if (!a[0] || a[0]->type != DECK_T_STR ||
        !a[1] || a[1]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "time.parse(s:str, fmt:str)"); return NULL;
    }
    char s[96];   uint32_t sl = a[0]->as.s.len < 95 ? a[0]->as.s.len : 95;
    memcpy(s, a[0]->as.s.ptr, sl); s[sl] = 0;
    char fmt[64]; uint32_t fl = a[1]->as.s.len < 63 ? a[1]->as.s.len : 63;
    memcpy(fmt, a[1]->as.s.ptr, fl); fmt[fl] = 0;
    struct tm tm; memset(&tm, 0, sizeof(tm));
    extern char *strptime(const char *s, const char *format, struct tm *tm);
    char *end = strptime(s, fmt, &tm);
    if (!end) return deck_new_none();
    /* timegm is non-portable; construct epoch manually from tm fields as UTC. */
    static const int16_t mdays[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int year = tm.tm_year + 1900;
    int month = tm.tm_mon;
    int day = tm.tm_mday;
    if (year < 1970 || month < 0 || month > 11) return deck_new_none();
    int64_t days = (year - 1970) * 365 + (year - 1969) / 4 - (year - 1901) / 100 + (year - 1601) / 400;
    days += mdays[month] + (day - 1);
    bool is_leap = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
    if (month >= 2 && is_leap) days += 1;
    int64_t epoch = days * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    return deck_new_some(deck_new_int(epoch));
}

/* from_iso is just parse with the fixed ISO 8601 Z format. Returns :none on bad input. */
static deck_value_t *b_time_from_iso(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    if (!a[0] || a[0]->type != DECK_T_STR) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "time.from_iso(str)"); return NULL; }
    char s[40]; uint32_t L = a[0]->as.s.len;
    if (L > 39) return deck_new_none();
    memcpy(s, a[0]->as.s.ptr, L); s[L] = 0;
    int Y, Mo, D, h, m, sec;
    char tail = 0;
    int got = sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%c", &Y, &Mo, &D, &h, &m, &sec, &tail);
    if (got < 6) return deck_new_none();
    if (got >= 7 && tail != 'Z' && tail != '+' && tail != '-') return deck_new_none();
    if (Y < 1970 || Mo < 1 || Mo > 12 || D < 1 || D > 31) return deck_new_none();
    static const int16_t mdays[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int64_t days = (Y - 1970) * 365 + (Y - 1969) / 4 - (Y - 1901) / 100 + (Y - 1601) / 400;
    days += mdays[Mo - 1] + (D - 1);
    bool is_leap = ((Y % 4 == 0) && (Y % 100 != 0)) || (Y % 400 == 0);
    if (Mo >= 3 && is_leap) days += 1;
    int64_t epoch = days * 86400 + h * 3600 + m * 60 + sec;
    return deck_new_some(deck_new_int(epoch));
}

/* time.date_parts(t) -> {str: int?} with year/month/day/hour/minute/second.
 * Returning int? (i.e. :some N) matches the fixture's `match parts.year | :some y`
 * pattern — map.get already produces Option. So the map values are plain int
 * and the Option wraps at map.get time. Wait — fixture does `parts.year` not
 * `map.get(parts, "year")`. That's the future map.field accessor; for now the
 * fixture won't work regardless. Keep the shape spec-correct: {str: int}. */
static deck_value_t *b_time_date_parts(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    int64_t t = ts_or_err(a[0], c, "time.date_parts");
    if (c->err) return NULL;
    time_t tt = (time_t)t;
    struct tm tm; gmtime_r(&tt, &tm);
    deck_value_t *m = deck_new_map(8);
    if (!m) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "time.date_parts alloc"); return NULL; }
    #define PUT_I(k, v) do { deck_value_t *kk = deck_new_str_cstr(k); deck_value_t *vv = deck_new_int(v); deck_map_put(m, kk, vv); deck_release(kk); deck_release(vv); } while (0)
    PUT_I("year",   tm.tm_year + 1900);
    PUT_I("month",  tm.tm_mon + 1);
    PUT_I("day",    tm.tm_mday);
    PUT_I("hour",   tm.tm_hour);
    PUT_I("minute", tm.tm_min);
    PUT_I("second", tm.tm_sec);
    #undef PUT_I
    return m;
}

static deck_value_t *b_time_day_of_week(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    int64_t t = ts_or_err(a[0], c, "time.day_of_week");
    if (c->err) return NULL;
    time_t tt = (time_t)t;
    struct tm tm; gmtime_r(&tt, &tm);
    return deck_new_int(tm.tm_wday);    /* 0=Sunday .. 6=Saturday */
}

static deck_value_t *b_time_start_of_day(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    int64_t t = ts_or_err(a[0], c, "time.start_of_day");
    if (c->err) return NULL;
    return deck_new_int((t / 86400) * 86400);
}

static deck_value_t *b_time_duration_parts(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    int64_t d = ts_or_err(a[0], c, "time.duration_parts");
    if (c->err) return NULL;
    if (d < 0) d = -d;
    deck_value_t *m = deck_new_map(8);
    if (!m) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "time.duration_parts alloc"); return NULL; }
    int64_t days    = d / 86400; d %= 86400;
    int64_t hours   = d / 3600;  d %= 3600;
    int64_t minutes = d / 60;    d %= 60;
    int64_t seconds = d;
    #define PUT_I(k, v) do { deck_value_t *kk = deck_new_str_cstr(k); deck_value_t *vv = deck_new_int(v); deck_map_put(m, kk, vv); deck_release(kk); deck_release(vv); } while (0)
    PUT_I("days",    days);
    PUT_I("hours",   hours);
    PUT_I("minutes", minutes);
    PUT_I("seconds", seconds);
    #undef PUT_I
    return m;
}

static deck_value_t *b_time_duration_str(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    int64_t d = ts_or_err(a[0], c, "time.duration_str");
    if (c->err) return NULL;
    bool neg = d < 0;
    if (neg) d = -d;
    char buf[64];
    int k;
    if (d >= 86400)      k = snprintf(buf, sizeof(buf), "%s%lldd %lldh", neg ? "-" : "", (long long)(d / 86400), (long long)((d % 86400) / 3600));
    else if (d >= 3600)  k = snprintf(buf, sizeof(buf), "%s%lldh %lldm", neg ? "-" : "", (long long)(d / 3600),  (long long)((d % 3600) / 60));
    else if (d >= 60)    k = snprintf(buf, sizeof(buf), "%s%lldm %llds", neg ? "-" : "", (long long)(d / 60),    (long long)(d % 60));
    else                 k = snprintf(buf, sizeof(buf), "%s%llds",       neg ? "-" : "", (long long)d);
    return deck_new_str(buf, (uint32_t)k);
}

static deck_value_t *b_time_ago(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    int64_t t = ts_or_err(a[0], c, "time.ago");
    if (c->err) return NULL;
    int64_t now_s = deck_sdi_time_wall_epoch_s();
    if (now_s <= 0) now_s = deck_sdi_time_monotonic_us() / 1000000;
    int64_t d = now_s - t;
    char buf[64];
    int k;
    if (d < 0)            k = snprintf(buf, sizeof(buf), "in the future");
    else if (d < 60)      k = snprintf(buf, sizeof(buf), "%llds ago", (long long)d);
    else if (d < 3600)    k = snprintf(buf, sizeof(buf), "%lldm ago", (long long)(d / 60));
    else if (d < 86400)   k = snprintf(buf, sizeof(buf), "%lldh ago", (long long)(d / 3600));
    else                  k = snprintf(buf, sizeof(buf), "%lldd ago", (long long)(d / 86400));
    return deck_new_str(buf, (uint32_t)k);
}

/* ---- nvs.* (concept #35, spec §3 / §05 §3) ----
 * Spec signature: `nvs.get(key)` / `nvs.set(key, value)` — no explicit
 * namespace. The namespace is the currently-executing app's `@app.id`.
 * Without an app context (test harness, scratch eval) a fallback ns is used
 * so operations still work deterministically. ESP32 NVS keys are limited to
 * 15 chars; we validate and return `:err :invalid_key` for too-long keys. */

#define NVS_KEY_MAX 15
#define NVS_FALLBACK_NS "deck.app"

static void nvs_app_ns(deck_interp_ctx_t *c, char *out, size_t cap)
{
    const char *id = NULL;
    if (c && c->module && c->module->kind == AST_MODULE) {
        for (uint32_t i = 0; i < c->module->as.module.items.len; i++) {
            const ast_node_t *it = c->module->as.module.items.items[i];
            if (!it || it->kind != AST_APP) continue;
            for (uint32_t f = 0; f < it->as.app.n_fields; f++) {
                const ast_app_field_t *fld = &it->as.app.fields[f];
                if (fld->name && strcmp(fld->name, "id") == 0 &&
                    fld->value && fld->value->kind == AST_LIT_STR) {
                    id = fld->value->as.s;
                    break;
                }
            }
            break;
        }
    }
    if (!id || !*id) id = NVS_FALLBACK_NS;
    /* ESP-IDF NVS namespaces are limited to 15 characters; truncate. */
    size_t L = strlen(id);
    if (L >= cap) L = cap - 1;
    if (L > 15) L = 15;
    memcpy(out, id, L);
    out[L] = 0;
}

static deck_value_t *nvs_err_result(deck_sdi_err_t rc)
{
    const char *name;
    switch (rc) {
        case DECK_SDI_ERR_NOT_FOUND:    name = "not_found";    break;
        case DECK_SDI_ERR_INVALID_ARG:  name = "invalid_key";  break;
        case DECK_SDI_ERR_NO_MEMORY:    name = "full";         break;
        case DECK_SDI_ERR_NOT_SUPPORTED:name = "write_fail";   break;
        default:                        name = "write_fail";   break;
    }
    deck_value_t *atom = deck_new_atom(name);
    if (!atom) return NULL;
    deck_value_t *r = make_result_tag("err", atom);
    deck_release(atom);
    return r;
}

/* Copy + null-terminate a key value into `dst`. Returns false on type error
 * or key too long (the latter surfaces as a :err :invalid_key Result, so the
 * caller distinguishes via `*out_key_too_long`). */
static bool nvs_copy_key(deck_value_t *v, char *dst, bool *out_too_long)
{
    *out_too_long = false;
    if (!v || v->type != DECK_T_STR) return false;
    if (v->as.s.len > NVS_KEY_MAX) { *out_too_long = true; return false; }
    memcpy(dst, v->as.s.ptr, v->as.s.len);
    dst[v->as.s.len] = 0;
    return true;
}

/* String get: returns :some str or :none (spec §3 sig `get(key) -> str?`). */
static deck_value_t *b_nvs_get(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    char key[NVS_KEY_MAX + 1];
    bool too_long;
    if (!nvs_copy_key(args[0], key, &too_long)) {
        if (too_long) return deck_new_none();
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "nvs.get(key:str)"); return NULL;
    }
    char ns[32]; nvs_app_ns(c, ns, sizeof(ns));
    char val[512];
    deck_sdi_err_t rc = deck_sdi_nvs_get_str(ns, key, val, sizeof(val));
    if (rc == DECK_SDI_ERR_NOT_FOUND) return deck_new_none();
    if (rc != DECK_SDI_OK) return deck_new_none();
    return deck_new_some(deck_new_str_cstr(val));
}

/* String set: returns :ok unit / :err :atom. */
static deck_value_t *b_nvs_set(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    char key[NVS_KEY_MAX + 1];
    bool too_long;
    if (!nvs_copy_key(args[0], key, &too_long)) {
        if (too_long) return nvs_err_result(DECK_SDI_ERR_INVALID_ARG);
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "nvs.set(key:str, val:str)"); return NULL;
    }
    if (!args[1] || args[1]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "nvs.set(key:str, val:str)"); return NULL;
    }
    char ns[32]; nvs_app_ns(c, ns, sizeof(ns));
    char val[512];
    uint32_t L = args[1]->as.s.len < 511 ? args[1]->as.s.len : 511;
    memcpy(val, args[1]->as.s.ptr, L); val[L] = 0;
    deck_sdi_err_t rc = deck_sdi_nvs_set_str(ns, key, val);
    if (rc != DECK_SDI_OK) return nvs_err_result(rc);
    return make_result_tag("ok", deck_unit());
}

/* Delete: returns :ok unit or :err :not_found. */
static deck_value_t *b_nvs_delete(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    char key[NVS_KEY_MAX + 1];
    bool too_long;
    if (!nvs_copy_key(args[0], key, &too_long)) {
        if (too_long) return nvs_err_result(DECK_SDI_ERR_INVALID_ARG);
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "nvs.delete(key:str)"); return NULL;
    }
    char ns[32]; nvs_app_ns(c, ns, sizeof(ns));
    deck_sdi_err_t rc = deck_sdi_nvs_del(ns, key);
    if (rc != DECK_SDI_OK) return nvs_err_result(rc);
    return make_result_tag("ok", deck_unit());
}

/* Int round-trip (SDI i64). */
static deck_value_t *b_nvs_get_int(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    char key[NVS_KEY_MAX + 1];
    bool too_long;
    if (!nvs_copy_key(args[0], key, &too_long)) { if (too_long) return deck_new_none(); set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "nvs.get_int"); return NULL; }
    char ns[32]; nvs_app_ns(c, ns, sizeof(ns));
    int64_t v = 0;
    if (deck_sdi_nvs_get_i64(ns, key, &v) != DECK_SDI_OK) return deck_new_none();
    return deck_new_some(deck_new_int(v));
}

static deck_value_t *b_nvs_set_int(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    char key[NVS_KEY_MAX + 1];
    bool too_long;
    if (!nvs_copy_key(args[0], key, &too_long)) { if (too_long) return nvs_err_result(DECK_SDI_ERR_INVALID_ARG); set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "nvs.set_int"); return NULL; }
    if (!args[1] || args[1]->type != DECK_T_INT) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "nvs.set_int(key, int)"); return NULL; }
    char ns[32]; nvs_app_ns(c, ns, sizeof(ns));
    deck_sdi_err_t rc = deck_sdi_nvs_set_i64(ns, key, args[1]->as.i);
    if (rc != DECK_SDI_OK) return nvs_err_result(rc);
    return make_result_tag("ok", deck_unit());
}

/* Bool piggybacks on i64 (0/1). */
static deck_value_t *b_nvs_get_bool(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    char key[NVS_KEY_MAX + 1];
    bool too_long;
    if (!nvs_copy_key(args[0], key, &too_long)) { if (too_long) return deck_new_none(); set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "nvs.get_bool"); return NULL; }
    char ns[32]; nvs_app_ns(c, ns, sizeof(ns));
    int64_t v = 0;
    if (deck_sdi_nvs_get_i64(ns, key, &v) != DECK_SDI_OK) return deck_new_none();
    return deck_new_some(deck_retain(v ? deck_true() : deck_false()));
}

static deck_value_t *b_nvs_set_bool(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    char key[NVS_KEY_MAX + 1];
    bool too_long;
    if (!nvs_copy_key(args[0], key, &too_long)) { if (too_long) return nvs_err_result(DECK_SDI_ERR_INVALID_ARG); set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "nvs.set_bool"); return NULL; }
    if (!args[1] || args[1]->type != DECK_T_BOOL) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "nvs.set_bool(key, bool)"); return NULL; }
    char ns[32]; nvs_app_ns(c, ns, sizeof(ns));
    deck_sdi_err_t rc = deck_sdi_nvs_set_i64(ns, key, args[1]->as.b ? 1 : 0);
    if (rc != DECK_SDI_OK) return nvs_err_result(rc);
    return make_result_tag("ok", deck_unit());
}

/* Float piggybacks on i64 with bit-pattern preservation. */
static deck_value_t *b_nvs_get_float(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    char key[NVS_KEY_MAX + 1];
    bool too_long;
    if (!nvs_copy_key(args[0], key, &too_long)) { if (too_long) return deck_new_none(); set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "nvs.get_float"); return NULL; }
    char ns[32]; nvs_app_ns(c, ns, sizeof(ns));
    int64_t bits = 0;
    if (deck_sdi_nvs_get_i64(ns, key, &bits) != DECK_SDI_OK) return deck_new_none();
    double d;
    memcpy(&d, &bits, sizeof(d));
    return deck_new_some(deck_new_float(d));
}

static deck_value_t *b_nvs_set_float(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    char key[NVS_KEY_MAX + 1];
    bool too_long;
    if (!nvs_copy_key(args[0], key, &too_long)) { if (too_long) return nvs_err_result(DECK_SDI_ERR_INVALID_ARG); set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "nvs.set_float"); return NULL; }
    if (!args[1] || args[1]->type != DECK_T_FLOAT) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "nvs.set_float(key, float)"); return NULL; }
    char ns[32]; nvs_app_ns(c, ns, sizeof(ns));
    double d = args[1]->as.f;
    int64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    deck_sdi_err_t rc = deck_sdi_nvs_set_i64(ns, key, bits);
    if (rc != DECK_SDI_OK) return nvs_err_result(rc);
    return make_result_tag("ok", deck_unit());
}

/* Bytes ↔ blob. Values surface as [int] (each 0..255). */
static deck_value_t *b_nvs_get_bytes(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    char key[NVS_KEY_MAX + 1];
    bool too_long;
    if (!nvs_copy_key(args[0], key, &too_long)) { if (too_long) return deck_new_none(); set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "nvs.get_bytes"); return NULL; }
    char ns[32]; nvs_app_ns(c, ns, sizeof(ns));
    uint8_t buf[512];
    size_t sz = sizeof(buf);
    if (deck_sdi_nvs_get_blob(ns, key, buf, &sz) != DECK_SDI_OK) return deck_new_none();
    deck_value_t *out = deck_new_list((uint32_t)sz);
    if (!out) return NULL;
    for (size_t i = 0; i < sz; i++) {
        deck_value_t *b = deck_new_int(buf[i]);
        deck_list_push(out, b); deck_release(b);
    }
    deck_value_t *some = deck_new_some(out);
    deck_release(out);
    return some;
}

static deck_value_t *b_nvs_set_bytes(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    char key[NVS_KEY_MAX + 1];
    bool too_long;
    if (!nvs_copy_key(args[0], key, &too_long)) { if (too_long) return nvs_err_result(DECK_SDI_ERR_INVALID_ARG); set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "nvs.set_bytes"); return NULL; }
    if (!args[1] || args[1]->type != DECK_T_LIST) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "nvs.set_bytes(key, [int])"); return NULL; }
    uint32_t L = args[1]->as.list.len;
    if (L > 1024) { set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "nvs.set_bytes: payload > 1KB"); return NULL; }
    uint8_t buf[1024];
    for (uint32_t i = 0; i < L; i++) {
        deck_value_t *v = args[1]->as.list.items[i];
        if (!v || v->type != DECK_T_INT) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "nvs.set_bytes: element %u not int", (unsigned)i); return NULL; }
        int64_t x = v->as.i;
        if (x < 0 || x > 255) { set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "nvs.set_bytes: byte out of range"); return NULL; }
        buf[i] = (uint8_t)x;
    }
    char ns[32]; nvs_app_ns(c, ns, sizeof(ns));
    deck_sdi_err_t rc = deck_sdi_nvs_set_blob(ns, key, buf, L);
    if (rc != DECK_SDI_OK) return nvs_err_result(rc);
    return make_result_tag("ok", deck_unit());
}

/* keys() → Result [str] nvs.Error. */
typedef struct { deck_value_t *list; bool alloc_fail; } nvs_keys_ctx_t;
static bool nvs_keys_cb(const char *key, void *user)
{
    nvs_keys_ctx_t *ctx = (nvs_keys_ctx_t *)user;
    if (ctx->alloc_fail) return false;
    deck_value_t *v = deck_new_str_cstr(key);
    if (!v) { ctx->alloc_fail = true; return false; }
    deck_list_push(ctx->list, v);
    deck_release(v);
    return true;
}

static deck_value_t *b_nvs_keys(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)args; (void)n;
    char ns[32]; nvs_app_ns(c, ns, sizeof(ns));
    nvs_keys_ctx_t ctx = { deck_new_list(8), false };
    if (!ctx.list) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "nvs.keys alloc"); return NULL; }
    deck_sdi_err_t rc = deck_sdi_nvs_keys(ns, nvs_keys_cb, &ctx);
    if (ctx.alloc_fail) { deck_release(ctx.list); set_err(c, DECK_RT_NO_MEMORY, 0, 0, "nvs.keys alloc"); return NULL; }
    if (rc != DECK_SDI_OK) { deck_release(ctx.list); return nvs_err_result(rc); }
    deck_value_t *r = make_result_tag("ok", ctx.list);
    deck_release(ctx.list);
    return r;
}

static deck_value_t *b_nvs_clear(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)args; (void)n;
    char ns[32]; nvs_app_ns(c, ns, sizeof(ns));
    deck_sdi_err_t rc = deck_sdi_nvs_clear(ns);
    if (rc != DECK_SDI_OK) return nvs_err_result(rc);
    return make_result_tag("ok", deck_unit());
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
/* Map SDI error code to the spec-canonical fs.Error atom (§3). */
static deck_value_t *fs_err_atom(deck_sdi_err_t rc)
{
    const char *name;
    switch (rc) {
        case DECK_SDI_ERR_NOT_FOUND:       name = "not_found"; break;
        case DECK_SDI_ERR_ALREADY_EXISTS:  name = "exists";    break;
        case DECK_SDI_ERR_NOT_SUPPORTED:   name = "io";        break;
        case DECK_SDI_ERR_INVALID_ARG:     name = "io";        break;
        case DECK_SDI_ERR_NO_MEMORY:       name = "full";      break;
        default:                           name = "io";        break;
    }
    return deck_new_atom(name);
}

/* Build a Result :err with the fs.Error atom payload. */
static deck_value_t *fs_err_result(deck_sdi_err_t rc)
{
    deck_value_t *atom = fs_err_atom(rc);
    if (!atom) return NULL;
    deck_value_t *r = make_result_tag("err", atom);
    deck_release(atom);
    return r;
}

/* Shared path-copy helper: validates + null-terminates into a 192-byte buffer.
 * Returns false + sets err on failure. */
static bool fs_copy_path(deck_value_t *v, char *dst, size_t cap, deck_interp_ctx_t *c, const char *who)
{
    if (!v || v->type != DECK_T_STR) { set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "%s: path must be str", who); return false; }
    if (v->as.s.len >= cap) { set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "%s: path too long", who); return false; }
    memcpy(dst, v->as.s.ptr, v->as.s.len);
    dst[v->as.s.len] = 0;
    return true;
}

/* fs.read → Result str fs.Error (spec §3). Content capped at 8 KB per read. */
static deck_value_t *b_fs_read(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    char path[192];
    if (!fs_copy_path(args[0], path, sizeof(path), c, "fs.read")) return NULL;
    static const size_t READ_CAP = 8192;
    char *buf = (char *)malloc(READ_CAP);
    if (!buf) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "fs.read alloc"); return NULL; }
    size_t sz = READ_CAP;
    deck_sdi_err_t rc = deck_sdi_fs_read(path, buf, &sz);
    if (rc != DECK_SDI_OK) { free(buf); return fs_err_result(rc); }
    deck_value_t *inner = deck_new_str(buf, (uint32_t)sz);
    free(buf);
    if (!inner) return NULL;
    deck_value_t *ok = make_result_tag("ok", inner);
    deck_release(inner);
    return ok;
}

/* fs.write(path, content) → Result unit fs.Error. Truncates. */
static deck_value_t *b_fs_write(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    char path[192];
    if (!fs_copy_path(args[0], path, sizeof(path), c, "fs.write")) return NULL;
    if (!args[1] || args[1]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "fs.write(path, str)"); return NULL;
    }
    deck_sdi_err_t rc = deck_sdi_fs_write(path, args[1]->as.s.ptr, args[1]->as.s.len);
    if (rc != DECK_SDI_OK) return fs_err_result(rc);
    return make_result_tag("ok", deck_unit());
}

/* fs.append(path, content). SDI has no seek+write; we read existing + concat +
 * write. If path doesn't exist yet, append creates it (same as write). */
static deck_value_t *b_fs_append(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    char path[192];
    if (!fs_copy_path(args[0], path, sizeof(path), c, "fs.append")) return NULL;
    if (!args[1] || args[1]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "fs.append(path, str)"); return NULL;
    }
    /* Read existing (if any) into a heap buffer. */
    static const size_t APPEND_CAP = 16384;
    char *buf = (char *)malloc(APPEND_CAP);
    if (!buf) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "fs.append alloc"); return NULL; }
    size_t existing_sz = APPEND_CAP;
    deck_sdi_err_t rc = deck_sdi_fs_read(path, buf, &existing_sz);
    if (rc == DECK_SDI_ERR_NOT_FOUND) { existing_sz = 0; rc = DECK_SDI_OK; }
    if (rc != DECK_SDI_OK) { free(buf); return fs_err_result(rc); }
    uint32_t add_len = args[1]->as.s.len;
    if (existing_sz + add_len > APPEND_CAP) {
        free(buf);
        set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "fs.append: combined size > 16KB");
        return NULL;
    }
    memcpy(buf + existing_sz, args[1]->as.s.ptr, add_len);
    rc = deck_sdi_fs_write(path, buf, existing_sz + add_len);
    free(buf);
    if (rc != DECK_SDI_OK) return fs_err_result(rc);
    return make_result_tag("ok", deck_unit());
}

/* fs.delete → Result unit fs.Error. */
static deck_value_t *b_fs_delete(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    char path[192];
    if (!fs_copy_path(args[0], path, sizeof(path), c, "fs.delete")) return NULL;
    deck_sdi_err_t rc = deck_sdi_fs_remove(path);
    if (rc != DECK_SDI_OK) return fs_err_result(rc);
    return make_result_tag("ok", deck_unit());
}

/* fs.mkdir → Result unit fs.Error. Parent must already exist (SDI contract). */
static deck_value_t *b_fs_mkdir(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    char path[192];
    if (!fs_copy_path(args[0], path, sizeof(path), c, "fs.mkdir")) return NULL;
    deck_sdi_err_t rc = deck_sdi_fs_mkdir(path);
    if (rc != DECK_SDI_OK) return fs_err_result(rc);
    return make_result_tag("ok", deck_unit());
}

/* fs.move(from, to). SDI has no rename primitive, so implement as
 * read-existing + write-new + delete-old. Atomicity is best-effort: if the
 * write succeeds but delete fails, the file exists at both paths. */
static deck_value_t *b_fs_move(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    char from[192], to[192];
    if (!fs_copy_path(args[0], from, sizeof(from), c, "fs.move")) return NULL;
    if (!fs_copy_path(args[1], to,   sizeof(to),   c, "fs.move")) return NULL;
    static const size_t MOVE_CAP = 16384;
    char *buf = (char *)malloc(MOVE_CAP);
    if (!buf) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "fs.move alloc"); return NULL; }
    size_t sz = MOVE_CAP;
    deck_sdi_err_t rc = deck_sdi_fs_read(from, buf, &sz);
    if (rc != DECK_SDI_OK) { free(buf); return fs_err_result(rc); }
    rc = deck_sdi_fs_write(to, buf, sz);
    free(buf);
    if (rc != DECK_SDI_OK) return fs_err_result(rc);
    rc = deck_sdi_fs_remove(from);
    if (rc != DECK_SDI_OK) return fs_err_result(rc);
    return make_result_tag("ok", deck_unit());
}

/* Concept #36 — fs.list returns Result [FsEntry] fs.Error per spec §3.
 * FsEntry is a map with string keys: name / is_dir / size / modified.
 * SDI exposes only name + is_dir at the callback; size / modified default
 * to 0 until the SDI vtable grows those fields. Map field access then
 * works via concept #33's dual-key lookup (both atom and string keys). */
typedef struct {
    deck_value_t *list;
    bool alloc_fail;
} fs_list_rec_ctx_t;

static bool fs_list_record_cb(const char *name, bool is_dir, void *user)
{
    fs_list_rec_ctx_t *ctx = user;
    if (ctx->alloc_fail || !name) return true;
    deck_value_t *entry = deck_new_map(8);
    if (!entry) { ctx->alloc_fail = true; return false; }
    #define PUT(k, v) do { deck_value_t *kk = deck_new_str_cstr(k); deck_value_t *vv = (v); deck_map_put(entry, kk, vv); deck_release(kk); deck_release(vv); } while (0)
    PUT("name",     deck_new_str_cstr(name));
    PUT("is_dir",   deck_retain(is_dir ? deck_true() : deck_false()));
    PUT("size",     deck_new_int(0));          /* SDI doesn't surface size yet */
    PUT("modified", deck_new_int(0));          /* SDI doesn't surface mtime yet */
    #undef PUT
    deck_list_push(ctx->list, entry);
    deck_release(entry);
    return true;
}

/* fs.read_bytes(path) -> Result [int] fs.Error. */
static deck_value_t *b_fs_read_bytes(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    char path[192];
    if (!fs_copy_path(args[0], path, sizeof(path), c, "fs.read_bytes")) return NULL;
    static const size_t READ_CAP = 8192;
    uint8_t *buf = (uint8_t *)malloc(READ_CAP);
    if (!buf) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "fs.read_bytes alloc"); return NULL; }
    size_t sz = READ_CAP;
    deck_sdi_err_t rc = deck_sdi_fs_read(path, buf, &sz);
    if (rc != DECK_SDI_OK) { free(buf); return fs_err_result(rc); }
    deck_value_t *out = deck_new_list((uint32_t)sz);
    if (!out) { free(buf); set_err(c, DECK_RT_NO_MEMORY, 0, 0, "fs.read_bytes list alloc"); return NULL; }
    for (size_t i = 0; i < sz; i++) {
        deck_value_t *b = deck_new_int(buf[i]);
        deck_list_push(out, b); deck_release(b);
    }
    free(buf);
    deck_value_t *r = make_result_tag("ok", out);
    deck_release(out);
    return r;
}

/* fs.write_bytes(path, [int]) -> Result unit fs.Error. */
static deck_value_t *b_fs_write_bytes(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    char path[192];
    if (!fs_copy_path(args[0], path, sizeof(path), c, "fs.write_bytes")) return NULL;
    if (!args[1] || args[1]->type != DECK_T_LIST) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "fs.write_bytes(path, [int])"); return NULL;
    }
    uint32_t L = args[1]->as.list.len;
    if (L > 8192) { set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "fs.write_bytes: payload > 8KB"); return NULL; }
    uint8_t *buf = L > 0 ? (uint8_t *)malloc(L) : NULL;
    if (L > 0 && !buf) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "fs.write_bytes alloc"); return NULL; }
    for (uint32_t i = 0; i < L; i++) {
        deck_value_t *v = args[1]->as.list.items[i];
        if (!v || v->type != DECK_T_INT) { free(buf); set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "fs.write_bytes: element %u not int", (unsigned)i); return NULL; }
        int64_t x = v->as.i;
        if (x < 0 || x > 255) { free(buf); set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "fs.write_bytes: byte out of range"); return NULL; }
        buf[i] = (uint8_t)x;
    }
    deck_sdi_err_t rc = deck_sdi_fs_write(path, buf, L);
    free(buf);
    if (rc != DECK_SDI_OK) return fs_err_result(rc);
    return make_result_tag("ok", deck_unit());
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
    char path[192];
    if (!fs_copy_path(args[0], path, sizeof(path), c, "fs.list")) return NULL;
    fs_list_rec_ctx_t ctx = { deck_new_list(8), false };
    if (!ctx.list) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "fs.list alloc"); return NULL; }
    deck_sdi_err_t rc = deck_sdi_fs_list(path, fs_list_record_cb, &ctx);
    if (ctx.alloc_fail) { deck_release(ctx.list); set_err(c, DECK_RT_NO_MEMORY, 0, 0, "fs.list entry alloc"); return NULL; }
    if (rc != DECK_SDI_OK) { deck_release(ctx.list); return fs_err_result(rc); }
    deck_value_t *r = make_result_tag("ok", ctx.list);
    deck_release(ctx.list);
    return r;
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
    if (total > (1U << 16)) {
        set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "text.repeat: result > 64KB");
        return NULL;
    }
    if (total == 0) return deck_new_str("", 0);
    char *buf = (char *)malloc((size_t)total + 1);
    if (!buf) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.repeat alloc"); return NULL; }
    uint32_t off = 0;
    for (int64_t i = 0; i < k; i++) {
        memcpy(buf + off, args[0]->as.s.ptr, single);
        off += single;
    }
    deck_value_t *out = deck_new_str(buf, off);
    free(buf);
    return out;
}

/* ---- text.* (continued — spec §3 completeness, concept #26) ---- */

static bool is_blank_ch(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
}

static deck_value_t *b_text_trim(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.trim expects str"); return NULL;
    }
    const char *s = args[0]->as.s.ptr;
    uint32_t L = args[0]->as.s.len;
    uint32_t start = 0, end = L;
    while (start < end && is_blank_ch(s[start])) start++;
    while (end > start && is_blank_ch(s[end - 1])) end--;
    return deck_new_str(s + start, end - start);
}

static deck_value_t *b_text_is_empty(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.is_empty expects str"); return NULL;
    }
    return deck_retain(args[0]->as.s.len == 0 ? deck_true() : deck_false());
}

static deck_value_t *b_text_is_blank(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.is_blank expects str"); return NULL;
    }
    const char *s = args[0]->as.s.ptr;
    for (uint32_t i = 0; i < args[0]->as.s.len; i++)
        if (!is_blank_ch(s[i])) return deck_retain(deck_false());
    return deck_retain(deck_true());
}

static deck_value_t *b_text_join(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST ||
        !args[1] || args[1]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.join([str], sep)"); return NULL;
    }
    uint32_t parts_n = args[0]->as.list.len;
    if (parts_n == 0) return deck_new_str("", 0);
    uint32_t sep_len = args[1]->as.s.len;
    uint64_t total = 0;
    for (uint32_t i = 0; i < parts_n; i++) {
        deck_value_t *p = args[0]->as.list.items[i];
        if (!p || p->type != DECK_T_STR) {
            set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.join: list element %u is not str", (unsigned)i);
            return NULL;
        }
        total += p->as.s.len;
        if (i + 1 < parts_n) total += sep_len;
    }
    if (total > (1U << 16)) {
        set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "text.join: result > 64KB"); return NULL;
    }
    char *buf = (char *)malloc((size_t)total + 1);
    if (!buf) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.join alloc"); return NULL; }
    uint32_t off = 0;
    for (uint32_t i = 0; i < parts_n; i++) {
        deck_value_t *p = args[0]->as.list.items[i];
        memcpy(buf + off, p->as.s.ptr, p->as.s.len); off += p->as.s.len;
        if (i + 1 < parts_n && sep_len > 0) {
            memcpy(buf + off, args[1]->as.s.ptr, sep_len); off += sep_len;
        }
    }
    deck_value_t *out = deck_new_str(buf, off);
    free(buf);
    return out;
}

/* Find first index of needle in haystack starting at `from`. -1 if not found. */
static int64_t find_sub(const char *h, uint32_t hl, const char *n, uint32_t nl, uint32_t from)
{
    if (nl == 0) return (int64_t)from;
    if (nl > hl || from > hl - nl) return -1;
    for (uint32_t i = from; i + nl <= hl; i++) {
        if (memcmp(h + i, n, nl) == 0) return (int64_t)i;
    }
    return -1;
}

static deck_value_t *b_text_index_of(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR ||
        !args[1] || args[1]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.index_of(str, str)"); return NULL;
    }
    int64_t idx = find_sub(args[0]->as.s.ptr, args[0]->as.s.len,
                           args[1]->as.s.ptr, args[1]->as.s.len, 0);
    if (idx < 0) return deck_new_none();
    return deck_new_some(deck_new_int(idx));
}

static deck_value_t *b_text_count(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR ||
        !args[1] || args[1]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.count(str, str)"); return NULL;
    }
    const char *h = args[0]->as.s.ptr;
    uint32_t hl = args[0]->as.s.len;
    const char *nd = args[1]->as.s.ptr;
    uint32_t nl = args[1]->as.s.len;
    if (nl == 0) return deck_new_int(0);
    int64_t count = 0;
    uint32_t i = 0;
    while (i + nl <= hl) {
        if (memcmp(h + i, nd, nl) == 0) { count++; i += nl; } else i++;
    }
    return deck_new_int(count);
}

static deck_value_t *b_text_slice(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR ||
        !args[1] || args[1]->type != DECK_T_INT ||
        !args[2] || args[2]->type != DECK_T_INT) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.slice(str, int, int)"); return NULL;
    }
    int64_t L = (int64_t)args[0]->as.s.len;
    int64_t s = args[1]->as.i;
    int64_t e = args[2]->as.i;
    if (s < 0) s += L;
    if (e < 0) e += L;
    if (s < 0) s = 0;
    if (e > L) e = L;
    if (s >= e) return deck_new_str("", 0);
    return deck_new_str(args[0]->as.s.ptr + s, (uint32_t)(e - s));
}

/* Shared engine for replace(n=1) / replace_all(n=-1 for "unlimited"). */
static deck_value_t *text_replace_impl(deck_interp_ctx_t *c, deck_value_t *sv,
                                       deck_value_t *fv, deck_value_t *tv, int64_t max_replacements)
{
    const char *s = sv->as.s.ptr;
    uint32_t sl = sv->as.s.len;
    const char *f = fv->as.s.ptr;
    uint32_t fl = fv->as.s.len;
    const char *t = tv->as.s.ptr;
    uint32_t tl = tv->as.s.len;
    if (fl == 0) return deck_new_str(s, sl);   /* spec: empty needle = identity */
    /* Upper bound for output size. */
    uint64_t max_out = (uint64_t)sl;
    if (tl > fl) {
        uint64_t extra = (uint64_t)(tl - fl) * (uint64_t)sl;
        max_out += extra;
    }
    if (max_out > (1U << 16)) {
        set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "text.replace: result > 64KB"); return NULL;
    }
    char *buf = (char *)malloc((size_t)max_out + 1);
    if (!buf) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.replace alloc"); return NULL; }
    uint32_t off = 0, i = 0;
    int64_t done = 0;
    while (i + fl <= sl) {
        if ((max_replacements < 0 || done < max_replacements) &&
            memcmp(s + i, f, fl) == 0) {
            memcpy(buf + off, t, tl); off += tl;
            i += fl;
            done++;
        } else {
            buf[off++] = s[i++];
        }
    }
    while (i < sl) buf[off++] = s[i++];
    deck_value_t *out = deck_new_str(buf, off);
    free(buf);
    return out;
}

static deck_value_t *b_text_replace(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR ||
        !args[1] || args[1]->type != DECK_T_STR ||
        !args[2] || args[2]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.replace(str, from, to)"); return NULL;
    }
    return text_replace_impl(c, args[0], args[1], args[2], 1);
}

static deck_value_t *b_text_replace_all(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR ||
        !args[1] || args[1]->type != DECK_T_STR ||
        !args[2] || args[2]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.replace_all(str, from, to)"); return NULL;
    }
    return text_replace_impl(c, args[0], args[1], args[2], -1);
}

/* Split on any run of newlines (LF / CRLF / CR). */
static deck_value_t *b_text_lines(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    if (!args[0] || args[0]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.lines expects str"); return NULL;
    }
    const char *s = args[0]->as.s.ptr;
    uint32_t L = args[0]->as.s.len;
    deck_value_t *out = deck_new_list(4);
    if (!out) return NULL;
    uint32_t start = 0;
    for (uint32_t i = 0; i < L; i++) {
        if (s[i] == '\n' || s[i] == '\r') {
            deck_value_t *frag = deck_new_str(s + start, i - start);
            deck_list_push(out, frag); deck_release(frag);
            if (s[i] == '\r' && i + 1 < L && s[i + 1] == '\n') i++;
            start = i + 1;
        }
    }
    if (start <= L) {
        deck_value_t *frag = deck_new_str(s + start, L - start);
        deck_list_push(out, frag); deck_release(frag);
    }
    return out;
}

/* Split on any run of whitespace; empty runs are skipped. */
static deck_value_t *b_text_words(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    if (!args[0] || args[0]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.words expects str"); return NULL;
    }
    const char *s = args[0]->as.s.ptr;
    uint32_t L = args[0]->as.s.len;
    deck_value_t *out = deck_new_list(4);
    if (!out) return NULL;
    uint32_t i = 0;
    while (i < L) {
        while (i < L && is_blank_ch(s[i])) i++;
        uint32_t start = i;
        while (i < L && !is_blank_ch(s[i])) i++;
        if (i > start) {
            deck_value_t *w = deck_new_str(s + start, i - start);
            deck_list_push(out, w); deck_release(w);
        }
    }
    return out;
}

static deck_value_t *b_text_truncate(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    if (!args[0] || args[0]->type != DECK_T_STR ||
        !args[1] || args[1]->type != DECK_T_INT) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.truncate(str, int [, suffix])"); return NULL;
    }
    int64_t max = args[1]->as.i;
    if (max < 0) max = 0;
    uint32_t L = args[0]->as.s.len;
    if ((uint64_t)L <= (uint64_t)max) return deck_new_str(args[0]->as.s.ptr, L);
    if (n < 3) return deck_new_str(args[0]->as.s.ptr, (uint32_t)max);
    /* With suffix: the output is max chars total, ending in suffix. */
    if (args[2]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.truncate suffix must be str"); return NULL;
    }
    uint32_t sl = args[2]->as.s.len;
    if ((uint64_t)sl >= (uint64_t)max) return deck_new_str(args[2]->as.s.ptr, sl);
    uint32_t keep = (uint32_t)max - sl;
    uint32_t need = keep + sl;
    char *buf = (char *)malloc(need + 1);
    if (!buf) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.truncate alloc"); return NULL; }
    memcpy(buf, args[0]->as.s.ptr, keep);
    memcpy(buf + keep, args[2]->as.s.ptr, sl);
    deck_value_t *out = deck_new_str(buf, need);
    free(buf);
    return out;
}

/* pad_side: -1 left, 0 center, +1 right */
static deck_value_t *text_pad_impl(deck_interp_ctx_t *c, deck_value_t **args, int pad_side)
{
    if (!args[0] || args[0]->type != DECK_T_STR ||
        !args[1] || args[1]->type != DECK_T_INT ||
        !args[2] || args[2]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "pad(str, int, ch)"); return NULL;
    }
    uint32_t L = args[0]->as.s.len;
    int64_t want = args[1]->as.i;
    if (want < 0) want = 0;
    if ((uint64_t)L >= (uint64_t)want) return deck_new_str(args[0]->as.s.ptr, L);
    uint32_t ch_len = args[2]->as.s.len;
    if (ch_len == 0) return deck_new_str(args[0]->as.s.ptr, L);
    uint32_t pad_total = (uint32_t)want - L;
    if ((uint64_t)want > (1U << 16)) {
        set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "pad: width > 64KB"); return NULL;
    }
    char *buf = (char *)malloc((size_t)want + 1);
    if (!buf) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "pad alloc"); return NULL; }
    uint32_t pad_left = 0, pad_right = 0;
    if (pad_side < 0)      { pad_right = 0; pad_left = pad_total; }
    else if (pad_side > 0) { pad_left = 0;  pad_right = pad_total; }
    else                   { pad_left = pad_total / 2; pad_right = pad_total - pad_left; }
    uint32_t off = 0;
    for (uint32_t i = 0; i < pad_left; i++) buf[off++] = args[2]->as.s.ptr[i % ch_len];
    memcpy(buf + off, args[0]->as.s.ptr, L); off += L;
    for (uint32_t i = 0; i < pad_right; i++) buf[off++] = args[2]->as.s.ptr[i % ch_len];
    deck_value_t *out = deck_new_str(buf, off);
    free(buf);
    return out;
}

static deck_value_t *b_text_pad_left  (deck_value_t **a, uint32_t n, deck_interp_ctx_t *c) { (void)n; return text_pad_impl(c, a, -1); }
static deck_value_t *b_text_pad_right (deck_value_t **a, uint32_t n, deck_interp_ctx_t *c) { (void)n; return text_pad_impl(c, a, +1); }
static deck_value_t *b_text_pad_center(deck_value_t **a, uint32_t n, deck_interp_ctx_t *c) { (void)n; return text_pad_impl(c, a,  0); }

/* ---- text.* codecs (spec §3, concept #28) ---- */

static const char B64_ENC[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static deck_value_t *b_text_base64_encode(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.base64_encode expects str"); return NULL;
    }
    const unsigned char *s = (const unsigned char *)args[0]->as.s.ptr;
    uint32_t L = args[0]->as.s.len;
    if (L > (1U << 15)) { set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "text.base64_encode: input > 32KB"); return NULL; }
    uint32_t out_len = 4 * ((L + 2) / 3);
    char *buf = (char *)malloc((size_t)out_len + 1);
    if (!buf) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.base64_encode alloc"); return NULL; }
    uint32_t o = 0;
    for (uint32_t i = 0; i < L; i += 3) {
        uint32_t b0 = s[i];
        uint32_t b1 = (i + 1 < L) ? s[i + 1] : 0;
        uint32_t b2 = (i + 2 < L) ? s[i + 2] : 0;
        uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        buf[o++] = B64_ENC[(triple >> 18) & 0x3F];
        buf[o++] = B64_ENC[(triple >> 12) & 0x3F];
        buf[o++] = (i + 1 < L) ? B64_ENC[(triple >> 6) & 0x3F] : '=';
        buf[o++] = (i + 2 < L) ? B64_ENC[triple & 0x3F] : '=';
    }
    deck_value_t *out = deck_new_str(buf, o);
    free(buf);
    return out;
}

/* Base64 decode table: -1 = invalid, -2 = padding '=', -3 = whitespace (skip). */
static int b64_val(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    if (ch == '=') return -2;
    if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') return -3;
    return -1;
}

static deck_value_t *b_text_base64_decode(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.base64_decode expects str"); return NULL;
    }
    const char *s = args[0]->as.s.ptr;
    uint32_t L = args[0]->as.s.len;
    if (L > (1U << 16)) { set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "text.base64_decode: input > 64KB"); return NULL; }
    char *buf = (char *)malloc((size_t)(L * 3 / 4) + 3);
    if (!buf) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.base64_decode alloc"); return NULL; }
    uint32_t o = 0;
    int quad[4]; uint32_t qn = 0;
    int pad = 0;
    for (uint32_t i = 0; i < L; i++) {
        int v = b64_val(s[i]);
        if (v == -3) continue;       /* whitespace ignored */
        if (v == -1) { free(buf); return deck_new_none(); }
        if (v == -2) { pad++; quad[qn++] = 0; }
        else {
            if (pad) { free(buf); return deck_new_none(); }   /* non-pad after pad */
            quad[qn++] = v;
        }
        if (qn == 4) {
            uint32_t triple = ((uint32_t)quad[0] << 18) | ((uint32_t)quad[1] << 12) |
                              ((uint32_t)quad[2] << 6)  |  (uint32_t)quad[3];
            if (pad <= 2) buf[o++] = (char)((triple >> 16) & 0xFF);
            if (pad <= 1) buf[o++] = (char)((triple >> 8)  & 0xFF);
            if (pad == 0) buf[o++] = (char)(triple         & 0xFF);
            qn = 0;
        }
    }
    if (qn != 0) { free(buf); return deck_new_none(); }   /* incomplete quad */
    deck_value_t *inner = deck_new_str(buf, o);
    free(buf);
    if (!inner) return NULL;
    deck_value_t *some = deck_new_some(inner);
    deck_release(inner);
    return some;
}

static bool url_unreserved(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
           ch == '.' || ch == '~';
}

static deck_value_t *b_text_url_encode(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.url_encode expects str"); return NULL;
    }
    const unsigned char *s = (const unsigned char *)args[0]->as.s.ptr;
    uint32_t L = args[0]->as.s.len;
    if ((uint64_t)L * 3 > (1U << 16)) { set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "text.url_encode: input too large"); return NULL; }
    char *buf = (char *)malloc((size_t)L * 3 + 1);
    if (!buf) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.url_encode alloc"); return NULL; }
    static const char HEX[] = "0123456789ABCDEF";
    uint32_t o = 0;
    for (uint32_t i = 0; i < L; i++) {
        unsigned char ch = s[i];
        if (url_unreserved(ch)) {
            buf[o++] = (char)ch;
        } else {
            buf[o++] = '%';
            buf[o++] = HEX[(ch >> 4) & 0xF];
            buf[o++] = HEX[ch & 0xF];
        }
    }
    deck_value_t *out = deck_new_str(buf, o);
    free(buf);
    return out;
}

static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* url_decode: %-decodes; also treats `+` as space (form-encoding variant). */
static deck_value_t *b_text_url_decode(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.url_decode expects str"); return NULL;
    }
    const char *s = args[0]->as.s.ptr;
    uint32_t L = args[0]->as.s.len;
    char *buf = (char *)malloc((size_t)L + 1);
    if (!buf) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.url_decode alloc"); return NULL; }
    uint32_t o = 0;
    for (uint32_t i = 0; i < L; i++) {
        char ch = s[i];
        if (ch == '%' && i + 2 < L) {
            int hi = hex_nibble(s[i + 1]);
            int lo = hex_nibble(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                buf[o++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        buf[o++] = ch;
    }
    deck_value_t *out = deck_new_str(buf, o);
    free(buf);
    return out;
}

/* hex_encode takes a list-of-int (each 0-255) or a DECK_T_BYTES. Spec §3
 * calls it `[byte]`; the runtime materializes byte lists as int lists. */
static deck_value_t *b_text_hex_encode(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    const uint8_t *src = NULL;
    uint32_t src_len = 0;
    uint8_t *heap_src = NULL;   /* for list conversion */
    if (args[0] && args[0]->type == DECK_T_BYTES) {
        src = args[0]->as.bytes.buf;
        src_len = args[0]->as.bytes.len;
    } else if (args[0] && args[0]->type == DECK_T_LIST) {
        src_len = args[0]->as.list.len;
        if (src_len > 0) {
            heap_src = (uint8_t *)malloc(src_len);
            if (!heap_src) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.hex_encode alloc"); return NULL; }
            for (uint32_t i = 0; i < src_len; i++) {
                deck_value_t *v = args[0]->as.list.items[i];
                if (!v || v->type != DECK_T_INT) {
                    free(heap_src);
                    set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.hex_encode: list element %u not int", (unsigned)i);
                    return NULL;
                }
                int64_t x = v->as.i;
                if (x < 0 || x > 255) {
                    free(heap_src);
                    set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "text.hex_encode: byte out of range");
                    return NULL;
                }
                heap_src[i] = (uint8_t)x;
            }
            src = heap_src;
        }
    } else {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.hex_encode expects [byte] or bytes"); return NULL;
    }
    if (src_len > (1U << 15)) { if (heap_src) free(heap_src); set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "text.hex_encode: input > 32KB"); return NULL; }
    static const char HEX[] = "0123456789abcdef";
    char *buf = (char *)malloc((size_t)src_len * 2 + 1);
    if (!buf) { if (heap_src) free(heap_src); set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.hex_encode alloc"); return NULL; }
    for (uint32_t i = 0; i < src_len; i++) {
        buf[2 * i]     = HEX[(src[i] >> 4) & 0xF];
        buf[2 * i + 1] = HEX[src[i] & 0xF];
    }
    deck_value_t *out = deck_new_str(buf, src_len * 2);
    free(buf);
    if (heap_src) free(heap_src);
    return out;
}

/* Forward declaration — b_to_str lives further down (bare-builtin area). */
static deck_value_t *b_to_str(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c);
/* Forward declaration — cmp_str is defined below alongside query_build. */
static int cmp_str(const void *a, const void *b);

/* ---- text.json / text.from_json (concept #31, spec §3) ----
 * Minimal JSON — RFC 8259 subset: no exponent on ints (floats OK),
 * standard escapes (\n \t \r \" \\ \/ \b \f \uXXXX), strict grammar.
 * Value ↔ JSON mapping:
 *   null    ↔ unit
 *   true    ↔ bool true
 *   false   ↔ bool false
 *   integer ↔ int
 *   number  ↔ float (has '.' or 'e'/'E')
 *   string  ↔ str
 *   array   ↔ list
 *   object  ↔ map (keys lex-sorted on emit for determinism)
 *   atom/bytes/fn/tuple ↔ unsupported; serializer errors, parser never emits. */

typedef struct {
    char *buf;
    uint32_t len;
    uint32_t cap;
    bool oom;
} js_out_t;

static void js_reserve(js_out_t *o, uint32_t extra)
{
    if (o->oom) return;
    uint64_t need = (uint64_t)o->len + extra + 1;
    if (need > o->cap) {
        uint64_t newcap = o->cap ? o->cap : 64;
        while (newcap < need) newcap *= 2;
        if (newcap > (1U << 17)) { o->oom = true; return; }   /* 128 KB cap */
        char *nb = (char *)realloc(o->buf, (size_t)newcap);
        if (!nb) { o->oom = true; return; }
        o->buf = nb;
        o->cap = (uint32_t)newcap;
    }
}

static void js_putc(js_out_t *o, char ch)
{
    js_reserve(o, 1);
    if (o->oom) return;
    o->buf[o->len++] = ch;
}

static void js_puts(js_out_t *o, const char *s, uint32_t n)
{
    js_reserve(o, n);
    if (o->oom) return;
    memcpy(o->buf + o->len, s, n);
    o->len += n;
}

static void js_emit_str(js_out_t *o, const char *s, uint32_t n)
{
    js_putc(o, '"');
    for (uint32_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        switch (ch) {
            case '"':  js_puts(o, "\\\"", 2); break;
            case '\\': js_puts(o, "\\\\", 2); break;
            case '\n': js_puts(o, "\\n",  2); break;
            case '\r': js_puts(o, "\\r",  2); break;
            case '\t': js_puts(o, "\\t",  2); break;
            case '\b': js_puts(o, "\\b",  2); break;
            case '\f': js_puts(o, "\\f",  2); break;
            default:
                if (ch < 0x20) {
                    static const char HEX[] = "0123456789abcdef";
                    char esc[6] = { '\\', 'u', '0', '0', HEX[(ch >> 4) & 0xF], HEX[ch & 0xF] };
                    js_puts(o, esc, 6);
                } else {
                    js_putc(o, (char)ch);
                }
                break;
        }
    }
    js_putc(o, '"');
}

static bool json_emit_value(js_out_t *o, deck_value_t *v);

static int cmp_strkey(const void *a, const void *b)
{
    return cmp_str(a, b);
}

static bool json_emit_value(js_out_t *o, deck_value_t *v)
{
    if (!v) { js_puts(o, "null", 4); return true; }
    char scratch[40];
    switch (v->type) {
        case DECK_T_UNIT:   js_puts(o, "null", 4);  return true;
        case DECK_T_BOOL:   js_puts(o, v->as.b ? "true" : "false", v->as.b ? 4 : 5); return true;
        case DECK_T_INT: {
            int k = snprintf(scratch, sizeof(scratch), "%lld", (long long)v->as.i);
            js_puts(o, scratch, (uint32_t)k); return true;
        }
        case DECK_T_FLOAT: {
            double x = v->as.f;
            if (isnan(x) || isinf(x)) { js_puts(o, "null", 4); return true; }
            int k = snprintf(scratch, sizeof(scratch), "%g", x);
            js_puts(o, scratch, (uint32_t)k); return true;
        }
        case DECK_T_STR: js_emit_str(o, v->as.s.ptr, v->as.s.len); return true;
        case DECK_T_LIST: {
            js_putc(o, '[');
            for (uint32_t i = 0; i < v->as.list.len; i++) {
                if (i) js_putc(o, ',');
                if (!json_emit_value(o, v->as.list.items[i])) return false;
            }
            js_putc(o, ']'); return true;
        }
        case DECK_T_MAP: {
            js_putc(o, '{');
            uint32_t used = 0;
            for (uint32_t i = 0; i < v->as.map.len; i++) if (v->as.map.entries[i].used) used++;
            if (used == 0) { js_putc(o, '}'); return true; }
            deck_value_t **keys = (deck_value_t **)malloc(sizeof(deck_value_t *) * used);
            if (!keys) { o->oom = true; return false; }
            uint32_t ki = 0;
            for (uint32_t i = 0; i < v->as.map.len; i++) {
                if (!v->as.map.entries[i].used) continue;
                deck_value_t *k = v->as.map.entries[i].key;
                if (!k || k->type != DECK_T_STR) { free(keys); return false; }
                keys[ki++] = k;
            }
            qsort(keys, used, sizeof(deck_value_t *), cmp_strkey);
            for (uint32_t i = 0; i < used; i++) {
                if (i) js_putc(o, ',');
                js_emit_str(o, keys[i]->as.s.ptr, keys[i]->as.s.len);
                js_putc(o, ':');
                deck_value_t *val = deck_map_get(v, keys[i]);
                if (!json_emit_value(o, val)) { free(keys); return false; }
            }
            free(keys);
            js_putc(o, '}');
            return true;
        }
        default: return false;   /* atom / bytes / fn / tuple — unsupported */
    }
}

static deck_value_t *b_text_json(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    js_out_t o = { NULL, 0, 0, false };
    bool ok = json_emit_value(&o, args[0]);
    if (!ok || o.oom) {
        free(o.buf);
        if (o.oom) { set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "text.json: output too large or unsupported type"); return NULL; }
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.json: value type not representable in JSON");
        return NULL;
    }
    deck_value_t *result = deck_new_str(o.buf ? o.buf : "", o.len);
    free(o.buf);
    return result;
}

/* ---- text.from_json (parser) ---- */
typedef struct { const char *s; uint32_t i, L; bool err; } js_in_t;

static deck_value_t *json_parse_value(js_in_t *p);

static void js_skip_ws(js_in_t *p)
{
    while (p->i < p->L) {
        char c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->i++;
        else break;
    }
}

static deck_value_t *json_parse_string(js_in_t *p)
{
    if (p->i >= p->L || p->s[p->i] != '"') { p->err = true; return NULL; }
    p->i++;
    char *buf = (char *)malloc(p->L - p->i + 1);
    if (!buf) { p->err = true; return NULL; }
    uint32_t o = 0;
    while (p->i < p->L) {
        char ch = p->s[p->i++];
        if (ch == '"') {
            deck_value_t *r = deck_new_str(buf, o);
            free(buf);
            return r;
        }
        if (ch == '\\') {
            if (p->i >= p->L) { free(buf); p->err = true; return NULL; }
            char esc = p->s[p->i++];
            switch (esc) {
                case '"':  buf[o++] = '"'; break;
                case '\\': buf[o++] = '\\'; break;
                case '/':  buf[o++] = '/'; break;
                case 'n':  buf[o++] = '\n'; break;
                case 'r':  buf[o++] = '\r'; break;
                case 't':  buf[o++] = '\t'; break;
                case 'b':  buf[o++] = '\b'; break;
                case 'f':  buf[o++] = '\f'; break;
                case 'u': {
                    if (p->i + 4 > p->L) { free(buf); p->err = true; return NULL; }
                    int cp = 0;
                    for (int k = 0; k < 4; k++) {
                        int nib = hex_nibble(p->s[p->i + k]);
                        if (nib < 0) { free(buf); p->err = true; return NULL; }
                        cp = (cp << 4) | nib;
                    }
                    p->i += 4;
                    /* Simple UTF-8 encoding up to BMP (0..0xFFFF). */
                    if (cp < 0x80) buf[o++] = (char)cp;
                    else if (cp < 0x800) {
                        buf[o++] = (char)(0xC0 | ((cp >> 6) & 0x1F));
                        buf[o++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        buf[o++] = (char)(0xE0 | ((cp >> 12) & 0x0F));
                        buf[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[o++] = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: free(buf); p->err = true; return NULL;
            }
        } else if ((unsigned char)ch < 0x20) {
            /* raw control char — invalid per RFC 8259 */
            free(buf); p->err = true; return NULL;
        } else {
            buf[o++] = ch;
        }
    }
    free(buf); p->err = true; return NULL;   /* unterminated */
}

static deck_value_t *json_parse_number(js_in_t *p)
{
    uint32_t start = p->i;
    bool is_float = false;
    if (p->i < p->L && p->s[p->i] == '-') p->i++;
    while (p->i < p->L && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
    if (p->i < p->L && p->s[p->i] == '.') {
        is_float = true; p->i++;
        while (p->i < p->L && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
    }
    if (p->i < p->L && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        is_float = true; p->i++;
        if (p->i < p->L && (p->s[p->i] == '+' || p->s[p->i] == '-')) p->i++;
        while (p->i < p->L && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
    }
    uint32_t end = p->i;
    if (end <= start || (end - start) > 32) { p->err = true; return NULL; }
    char scratch[40]; memcpy(scratch, p->s + start, end - start); scratch[end - start] = 0;
    if (is_float) return deck_new_float(strtod(scratch, NULL));
    return deck_new_int(strtoll(scratch, NULL, 10));
}

static deck_value_t *json_parse_array(js_in_t *p)
{
    if (p->s[p->i] != '[') { p->err = true; return NULL; }
    p->i++;
    js_skip_ws(p);
    deck_value_t *out = deck_new_list(4);
    if (!out) { p->err = true; return NULL; }
    if (p->i < p->L && p->s[p->i] == ']') { p->i++; return out; }
    while (p->i < p->L) {
        deck_value_t *v = json_parse_value(p);
        if (p->err) { deck_release(out); return NULL; }
        deck_list_push(out, v); deck_release(v);
        js_skip_ws(p);
        if (p->i < p->L && p->s[p->i] == ',') { p->i++; js_skip_ws(p); continue; }
        if (p->i < p->L && p->s[p->i] == ']') { p->i++; return out; }
        deck_release(out); p->err = true; return NULL;
    }
    deck_release(out); p->err = true; return NULL;
}

static deck_value_t *json_parse_object(js_in_t *p)
{
    if (p->s[p->i] != '{') { p->err = true; return NULL; }
    p->i++;
    js_skip_ws(p);
    deck_value_t *out = deck_new_map(8);
    if (!out) { p->err = true; return NULL; }
    if (p->i < p->L && p->s[p->i] == '}') { p->i++; return out; }
    while (p->i < p->L) {
        js_skip_ws(p);
        deck_value_t *k = json_parse_string(p);
        if (p->err) { deck_release(out); return NULL; }
        js_skip_ws(p);
        if (p->i >= p->L || p->s[p->i] != ':') { deck_release(k); deck_release(out); p->err = true; return NULL; }
        p->i++;
        js_skip_ws(p);
        deck_value_t *v = json_parse_value(p);
        if (p->err) { deck_release(k); deck_release(out); return NULL; }
        deck_map_put(out, k, v);
        deck_release(k); deck_release(v);
        js_skip_ws(p);
        if (p->i < p->L && p->s[p->i] == ',') { p->i++; continue; }
        if (p->i < p->L && p->s[p->i] == '}') { p->i++; return out; }
        deck_release(out); p->err = true; return NULL;
    }
    deck_release(out); p->err = true; return NULL;
}

static deck_value_t *json_parse_value(js_in_t *p)
{
    js_skip_ws(p);
    if (p->i >= p->L) { p->err = true; return NULL; }
    char c = p->s[p->i];
    if (c == '"') return json_parse_string(p);
    if (c == '{') return json_parse_object(p);
    if (c == '[') return json_parse_array(p);
    if (c == 't' && p->i + 4 <= p->L && memcmp(p->s + p->i, "true", 4) == 0) { p->i += 4; return deck_retain(deck_true()); }
    if (c == 'f' && p->i + 5 <= p->L && memcmp(p->s + p->i, "false", 5) == 0) { p->i += 5; return deck_retain(deck_false()); }
    if (c == 'n' && p->i + 4 <= p->L && memcmp(p->s + p->i, "null", 4) == 0) { p->i += 4; return deck_retain(deck_unit()); }
    if (c == '-' || (c >= '0' && c <= '9')) return json_parse_number(p);
    p->err = true;
    return NULL;
}

static deck_value_t *b_text_from_json(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    if (!args[0] || args[0]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.from_json expects str"); return NULL;
    }
    js_in_t p = { args[0]->as.s.ptr, 0, args[0]->as.s.len, false };
    deck_value_t *v = json_parse_value(&p);
    if (p.err || !v) { if (v) deck_release(v); return deck_new_none(); }
    js_skip_ws(&p);
    if (p.i != p.L) { deck_release(v); return deck_new_none(); }   /* trailing junk */
    deck_value_t *some = deck_new_some(v);
    deck_release(v);
    return some;
}

/* text.format(tmpl, {name: value}) -> str — replaces {name} placeholders
 * with map-lookup values stringified via b_to_str. Missing keys keep the
 * literal placeholder. `{{` escapes to literal `{`. */
static deck_value_t *b_text_format(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR ||
        !args[1] || args[1]->type != DECK_T_MAP) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.format(tmpl, {str: any})"); return NULL;
    }
    const char *s = args[0]->as.s.ptr;
    uint32_t L = args[0]->as.s.len;
    uint32_t cap = L + 32;
    char *out = (char *)malloc(cap);
    if (!out) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.format alloc"); return NULL; }
    uint32_t off = 0;
    uint32_t i = 0;
    while (i < L) {
        char ch = s[i];
        if (ch == '{' && i + 1 < L && s[i + 1] == '{') {
            /* {{ -> { */
            if (off + 1 >= cap) { cap *= 2; char *t = realloc(out, cap); if (!t) { free(out); set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.format realloc"); return NULL; } out = t; }
            out[off++] = '{';
            i += 2;
            continue;
        }
        if (ch == '{') {
            /* find matching } */
            uint32_t end = i + 1;
            while (end < L && s[end] != '}') end++;
            if (end >= L) {
                /* unmatched — emit literal */
                if (off + 1 >= cap) { cap *= 2; char *t = realloc(out, cap); if (!t) { free(out); set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.format realloc"); return NULL; } out = t; }
                out[off++] = ch;
                i++;
                continue;
            }
            uint32_t name_len = end - i - 1;
            deck_value_t *key = deck_new_str(s + i + 1, name_len);
            deck_value_t *val = key ? deck_map_get(args[1], key) : NULL;
            if (key) deck_release(key);
            if (!val) {
                /* missing -> literal {name} */
                uint32_t lit_len = (end - i + 1);
                if (off + lit_len >= cap) { while (cap <= off + lit_len) cap *= 2; char *t = realloc(out, cap); if (!t) { free(out); set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.format realloc"); return NULL; } out = t; }
                memcpy(out + off, s + i, lit_len); off += lit_len;
            } else {
                deck_value_t *stringified;
                if (val->type == DECK_T_STR) {
                    stringified = deck_retain(val);
                } else {
                    deck_value_t *sargs[1] = { val };
                    stringified = b_to_str(sargs, 1, c);
                    if (!stringified) { free(out); return NULL; }
                }
                uint32_t vl = stringified->as.s.len;
                if (off + vl >= cap) { while (cap <= off + vl) cap *= 2; char *t = realloc(out, cap); if (!t) { deck_release(stringified); free(out); set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.format realloc"); return NULL; } out = t; }
                memcpy(out + off, stringified->as.s.ptr, vl); off += vl;
                deck_release(stringified);
            }
            i = end + 1;
            continue;
        }
        if (off + 1 >= cap) { cap *= 2; char *t = realloc(out, cap); if (!t) { free(out); set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.format realloc"); return NULL; } out = t; }
        out[off++] = ch;
        i++;
    }
    deck_value_t *result = deck_new_str(out, off);
    free(out);
    return result;
}

/* text.bytes(s) -> [int] — convert string to list of byte values (0-255). */
static deck_value_t *b_text_bytes(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.bytes expects str"); return NULL;
    }
    const unsigned char *s = (const unsigned char *)args[0]->as.s.ptr;
    uint32_t L = args[0]->as.s.len;
    deck_value_t *out = deck_new_list(L);
    if (!out) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.bytes alloc"); return NULL; }
    for (uint32_t i = 0; i < L; i++) {
        deck_value_t *b = deck_new_int(s[i]);
        if (!b) { deck_release(out); return NULL; }
        deck_list_push(out, b);
        deck_release(b);
    }
    return out;
}

/* text.from_bytes([int]) -> str? — reverse of text.bytes. Null byte (0x00)
 * yields :none since it can't appear in a valid Deck string (runtime ptrs
 * are len-prefixed, but null is still a sentinel for C interop). Any other
 * 0x01..0xFF byte is accepted as the owning string is len-prefixed. */
static deck_value_t *b_text_from_bytes(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_LIST) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.from_bytes expects [int]"); return NULL;
    }
    uint32_t L = args[0]->as.list.len;
    if (L == 0) {
        deck_value_t *empty = deck_new_str("", 0);
        deck_value_t *some = deck_new_some(empty);
        deck_release(empty);
        return some;
    }
    if (L > (1U << 16)) {
        set_err(c, DECK_RT_OUT_OF_RANGE, 0, 0, "text.from_bytes: list > 64KB"); return NULL;
    }
    char *buf = (char *)malloc(L + 1);
    if (!buf) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.from_bytes alloc"); return NULL; }
    for (uint32_t i = 0; i < L; i++) {
        deck_value_t *v = args[0]->as.list.items[i];
        if (!v || v->type != DECK_T_INT) { free(buf); return deck_new_none(); }
        int64_t x = v->as.i;
        if (x < 0 || x > 255 || x == 0) { free(buf); return deck_new_none(); }
        buf[i] = (char)x;
    }
    deck_value_t *inner = deck_new_str(buf, L);
    free(buf);
    if (!inner) return NULL;
    deck_value_t *some = deck_new_some(inner);
    deck_release(inner);
    return some;
}

/* text.query_build({k: v}) -> str — URL-encoded k=v pairs joined with '&'.
 * Keys are emitted in lexicographic order so output is deterministic. */
static int cmp_str(const void *a, const void *b)
{
    deck_value_t *const *pa = (deck_value_t *const *)a;
    deck_value_t *const *pb = (deck_value_t *const *)b;
    const char *sa = (*pa)->as.s.ptr;
    const char *sb = (*pb)->as.s.ptr;
    uint32_t la = (*pa)->as.s.len, lb = (*pb)->as.s.len;
    uint32_t m = la < lb ? la : lb;
    int r = memcmp(sa, sb, m);
    if (r != 0) return r;
    return (la < lb) ? -1 : (la > lb) ? 1 : 0;
}

/* RFC 3986 percent-encode into a heap buf — shared by query_build + a future
 * caller. Caller owns the returned pointer, uses *out_len for its size. */
static char *url_pct_encode(const char *s, uint32_t L, uint32_t *out_len)
{
    static const char HEX[] = "0123456789ABCDEF";
    char *buf = (char *)malloc((size_t)L * 3 + 1);
    if (!buf) return NULL;
    uint32_t o = 0;
    for (uint32_t i = 0; i < L; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (url_unreserved(ch)) buf[o++] = (char)ch;
        else {
            buf[o++] = '%';
            buf[o++] = HEX[(ch >> 4) & 0xF];
            buf[o++] = HEX[ch & 0xF];
        }
    }
    *out_len = o;
    return buf;
}

static deck_value_t *b_text_query_build(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_MAP) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.query_build({str: str})"); return NULL;
    }
    /* Gather used keys. */
    deck_map_t *m = &args[0]->as.map;
    uint32_t n_entries = 0;
    for (uint32_t i = 0; i < m->len; i++) if (m->entries[i].used) n_entries++;
    if (n_entries == 0) return deck_new_str("", 0);
    deck_value_t **keys = (deck_value_t **)malloc(sizeof(deck_value_t *) * n_entries);
    if (!keys) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.query_build alloc"); return NULL; }
    uint32_t ki = 0;
    for (uint32_t i = 0; i < m->len; i++) {
        if (!m->entries[i].used) continue;
        deck_value_t *k = m->entries[i].key;
        deck_value_t *v = m->entries[i].val;
        if (!k || k->type != DECK_T_STR || !v || v->type != DECK_T_STR) {
            free(keys);
            set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.query_build: keys and values must be str");
            return NULL;
        }
        keys[ki++] = k;
    }
    qsort(keys, n_entries, sizeof(deck_value_t *), cmp_str);
    /* Assemble output. */
    uint32_t cap = 256;
    char *out = (char *)malloc(cap);
    if (!out) { free(keys); set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.query_build alloc"); return NULL; }
    uint32_t off = 0;
    for (uint32_t i = 0; i < n_entries; i++) {
        deck_value_t *k = keys[i];
        deck_value_t *v = deck_map_get(args[0], k);
        uint32_t klen = 0, vlen = 0;
        char *ke = url_pct_encode(k->as.s.ptr, k->as.s.len, &klen);
        char *ve = url_pct_encode(v->as.s.ptr, v->as.s.len, &vlen);
        if (!ke || !ve) { free(ke); free(ve); free(out); free(keys); set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.query_build alloc"); return NULL; }
        uint32_t need = off + klen + 1 + vlen + (i + 1 < n_entries ? 1 : 0);
        if (need >= cap) {
            while (cap <= need) cap *= 2;
            char *tmp = (char *)realloc(out, cap);
            if (!tmp) { free(ke); free(ve); free(out); free(keys); set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.query_build realloc"); return NULL; }
            out = tmp;
        }
        memcpy(out + off, ke, klen); off += klen;
        out[off++] = '=';
        memcpy(out + off, ve, vlen); off += vlen;
        if (i + 1 < n_entries) out[off++] = '&';
        free(ke); free(ve);
    }
    free(keys);
    deck_value_t *result = deck_new_str(out, off);
    free(out);
    return result;
}

/* text.query_parse("a=1&b=two%20words") -> {"a": "1", "b": "two words"}.
 * Invalid input (missing '=' in a pair, bad percent-escapes) is best-effort:
 * missing '=' pair -> value = "". Bad %XX -> literal passthrough (same as
 * url_decode). */
static deck_value_t *b_text_query_parse(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.query_parse expects str"); return NULL;
    }
    deck_value_t *out = deck_new_map(8);
    if (!out) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.query_parse alloc"); return NULL; }
    const char *s = args[0]->as.s.ptr;
    uint32_t L = args[0]->as.s.len;
    uint32_t i = 0;
    while (i < L) {
        uint32_t k_start = i;
        while (i < L && s[i] != '=' && s[i] != '&') i++;
        uint32_t k_end = i;
        uint32_t v_start = k_end, v_end = k_end;
        if (i < L && s[i] == '=') {
            i++;
            v_start = i;
            while (i < L && s[i] != '&') i++;
            v_end = i;
        }
        if (i < L && s[i] == '&') i++;
        /* Inline URL-decode directly into malloc'd buffers. */
        uint32_t klen = k_end - k_start;
        char *kbuf = (char *)malloc((size_t)klen + 1);
        if (!kbuf) { deck_release(out); set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.query_parse alloc"); return NULL; }
        uint32_t kl = 0;
        for (uint32_t p = k_start; p < k_end; p++) {
            char ch = s[p];
            if (ch == '%' && p + 2 < k_end) {
                int hi = hex_nibble(s[p + 1]), lo = hex_nibble(s[p + 2]);
                if (hi >= 0 && lo >= 0) { kbuf[kl++] = (char)((hi << 4) | lo); p += 2; continue; }
            }
            kbuf[kl++] = ch;
        }
        uint32_t vlen = v_end - v_start;
        char *vbuf = (char *)malloc((size_t)vlen + 1);
        if (!vbuf) { free(kbuf); deck_release(out); set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.query_parse alloc"); return NULL; }
        uint32_t vl = 0;
        for (uint32_t p = v_start; p < v_end; p++) {
            char ch = s[p];
            if (ch == '%' && p + 2 < v_end) {
                int hi = hex_nibble(s[p + 1]), lo = hex_nibble(s[p + 2]);
                if (hi >= 0 && lo >= 0) { vbuf[vl++] = (char)((hi << 4) | lo); p += 2; continue; }
            }
            vbuf[vl++] = ch;
        }
        deck_value_t *kv = deck_new_str(kbuf, kl);
        deck_value_t *vv = deck_new_str(vbuf, vl);
        free(kbuf); free(vbuf);
        if (!kv || !vv) { if (kv) deck_release(kv); if (vv) deck_release(vv); deck_release(out); return NULL; }
        deck_map_put(out, kv, vv);
        deck_release(kv); deck_release(vv);
    }
    return out;
}

/* hex_decode returns a list-of-int (each 0-255) wrapped in :some, or :none. */
static deck_value_t *b_text_hex_decode(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "text.hex_decode expects str"); return NULL;
    }
    const char *s = args[0]->as.s.ptr;
    uint32_t L = args[0]->as.s.len;
    if (L % 2 != 0) return deck_new_none();
    uint32_t out_n = L / 2;
    deck_value_t *out = deck_new_list(out_n);
    if (!out) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "text.hex_decode alloc"); return NULL; }
    for (uint32_t i = 0; i < out_n; i++) {
        int hi = hex_nibble(s[2 * i]);
        int lo = hex_nibble(s[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            deck_release(out);
            return deck_new_none();
        }
        deck_value_t *byte_v = deck_new_int((hi << 4) | lo);
        if (!byte_v) { deck_release(out); return NULL; }
        deck_list_push(out, byte_v);
        deck_release(byte_v);
    }
    deck_value_t *some = deck_new_some(out);
    deck_release(out);
    return some;
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

/* ================================================================
 * bridge.ui.* — DVC tree builders (DL2 F28 Phase 2)
 * ================================================================
 *
 * .deck apps build a UI tree by calling these builder builtins and then
 * pushing the root with `bridge.ui.render(root)`. Each builder returns an
 * integer "handle" (an index into a module-local node table) rather than
 * exposing the native `deck_dvc_node_t*` as a first-class Deck value — the
 * runtime doesn't have a generic opaque-pointer type and we don't need
 * one just to thread a few tree-building calls.
 *
 * Flow:
 *   let title = bridge.ui.label("Hello world")
 *   let btn   = bridge.ui.trigger("OK", 0)
 *   let col   = bridge.ui.column([title, btn])
 *   bridge.ui.render(col)
 *
 * Lifetime: s_bui_arena + s_bui_nodes live module-local (static). Each
 * call to `render()` encodes + pushes + resets the arena — so handles
 * from a previous build session become invalid. Only one build session
 * at a time (single-threaded interpreter; bridge UI runs on the LVGL
 * task but push_snapshot serialises via its own mutex).
 *
 * Error model: handles out of range, non-matching types, or table-full
 * errors all set DECK_RT_TYPE_MISMATCH or DECK_RT_NO_MEMORY on the ctx.
 * The caller sees a runtime error at the exact offending call site.
 */
#define DECK_BUI_MAX_NODES   128

static deck_arena_t      s_bui_arena  = {0};
static deck_dvc_node_t  *s_bui_nodes[DECK_BUI_MAX_NODES];
static uint32_t          s_bui_n      = 0;
static bool              s_bui_ready  = false;

static void bui_ensure_init(void)
{
    if (s_bui_ready) return;
    deck_arena_init(&s_bui_arena, 2048);
    s_bui_ready = true;
}

static void bui_reset(void)
{
    deck_arena_reset(&s_bui_arena);
    s_bui_n = 0;
}

static int64_t bui_register(deck_dvc_node_t *n)
{
    if (!n) return -1;
    if (s_bui_n >= DECK_BUI_MAX_NODES) return -1;
    s_bui_nodes[s_bui_n] = n;
    return (int64_t)s_bui_n++;
}

static deck_dvc_node_t *bui_lookup(int64_t h)
{
    if (h < 0 || (uint32_t)h >= s_bui_n) return NULL;
    return s_bui_nodes[(uint32_t)h];
}

/* Copy a Deck string (which is not NUL-terminated) into the render arena
 * with a NUL byte so deck_dvc_set_str can handle it as a C string. */
static const char *bui_strdup_from_value(deck_value_t *v)
{
    if (!v || v->type != DECK_T_STR) return NULL;
    char *copy = deck_arena_alloc(&s_bui_arena, v->as.s.len + 1);
    if (!copy) return NULL;
    memcpy(copy, v->as.s.ptr, v->as.s.len);
    copy[v->as.s.len] = '\0';
    return copy;
}

/* bridge.ui.label(text) — DVC_LABEL with :value=text.  */
static deck_value_t *b_bui_label(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "bridge.ui.label(text:str)"); return NULL;
    }
    bui_ensure_init();
    deck_dvc_node_t *node = deck_dvc_node_new(&s_bui_arena, DVC_LABEL);
    if (!node) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "bridge.ui.label oom"); return NULL; }
    const char *text = bui_strdup_from_value(args[0]);
    if (text) deck_dvc_set_str(&s_bui_arena, node, "value", text);
    int64_t h = bui_register(node);
    if (h < 0) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "bridge.ui: node table full"); return NULL; }
    return deck_new_int(h);
}

/* bridge.ui.trigger(text, intent_id) — DVC_TRIGGER; intent_id is an int
 * that the shell can resolve to an intent. Pass 0 for decorative buttons. */
static deck_value_t *b_bui_trigger(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR ||
        !args[1] || args[1]->type != DECK_T_INT) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "bridge.ui.trigger(text:str, intent:int)"); return NULL;
    }
    bui_ensure_init();
    deck_dvc_node_t *node = deck_dvc_node_new(&s_bui_arena, DVC_TRIGGER);
    if (!node) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "bridge.ui.trigger oom"); return NULL; }
    /* DVC schema for TRIGGER names the display string "label" (spec §18
     * + render_trigger in deck_bridge_ui_decode reads attr_str(n,
     * "label", ...)). Setting it under "text" silently produces a
     * "BUTTON" default — bug caught on hardware, 2026-04-17. */
    const char *text = bui_strdup_from_value(args[0]);
    if (text) deck_dvc_set_str(&s_bui_arena, node, "label", text);
    node->intent_id = (uint32_t)args[1]->as.i;
    int64_t h = bui_register(node);
    if (h < 0) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "bridge.ui: node table full"); return NULL; }
    return deck_new_int(h);
}

/* Shared helper: build a container node of `kind`, attach each handle in
 * the list as a child. Used for column/row/group. */
static deck_value_t *bui_container(deck_dvc_type_t kind, const char *debug,
                                   deck_value_t **args, deck_interp_ctx_t *c,
                                   const char *title_attr)
{
    /* Args shape depends on title_attr:
     *   NULL  → args[0] is children list
     *   set   → args[0] is title (str), args[1] is children list */
    uint32_t ai = 0;
    const char *title = NULL;
    if (title_attr) {
        if (!args[0] || args[0]->type != DECK_T_STR) {
            set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0,
                    "bridge.ui.%s(title:str, children:list)", debug);
            return NULL;
        }
        title = bui_strdup_from_value(args[0]);
        ai = 1;
    }
    if (!args[ai] || args[ai]->type != DECK_T_LIST) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0,
                "bridge.ui.%s: children must be a list", debug);
        return NULL;
    }
    bui_ensure_init();
    deck_dvc_node_t *node = deck_dvc_node_new(&s_bui_arena, kind);
    if (!node) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "bridge.ui.%s oom", debug); return NULL; }
    if (title && title_attr) deck_dvc_set_str(&s_bui_arena, node, title_attr, title);

    deck_value_t *list = args[ai];
    for (uint32_t i = 0; i < list->as.list.len; i++) {
        deck_value_t *item = list->as.list.items[i];
        if (!item || item->type != DECK_T_INT) {
            set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0,
                    "bridge.ui.%s: child %u not a handle", debug, (unsigned)i);
            return NULL;
        }
        deck_dvc_node_t *child = bui_lookup(item->as.i);
        if (!child) {
            set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0,
                    "bridge.ui.%s: child %u handle=%lld invalid",
                    debug, (unsigned)i, (long long)item->as.i);
            return NULL;
        }
        deck_dvc_add_child(&s_bui_arena, node, child);
    }
    int64_t h = bui_register(node);
    if (h < 0) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "bridge.ui: node table full"); return NULL; }
    return deck_new_int(h);
}

static deck_value_t *b_bui_column(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{ (void)n; return bui_container(DVC_COLUMN, "column", args, c, NULL); }

static deck_value_t *b_bui_row(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{ (void)n; return bui_container(DVC_ROW, "row", args, c, NULL); }

static deck_value_t *b_bui_group(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{ (void)n; return bui_container(DVC_GROUP, "group", args, c, "title"); }

/* bridge.ui.data_row(label, value) — dim-label-over-primary-value row. */
static deck_value_t *b_bui_data_row(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR ||
        !args[1] || args[1]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "bridge.ui.data_row(label:str, value:str)"); return NULL;
    }
    bui_ensure_init();
    deck_dvc_node_t *node = deck_dvc_node_new(&s_bui_arena, DVC_DATA_ROW);
    if (!node) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "bridge.ui.data_row oom"); return NULL; }
    const char *lbl = bui_strdup_from_value(args[0]);
    const char *val = bui_strdup_from_value(args[1]);
    if (lbl) deck_dvc_set_str(&s_bui_arena, node, "label", lbl);
    if (val) deck_dvc_set_str(&s_bui_arena, node, "value", val);
    int64_t h = bui_register(node);
    if (h < 0) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "bridge.ui: node table full"); return NULL; }
    return deck_new_int(h);
}

/* bridge.ui.divider() / bridge.ui.spacer() — no args, decorative layout. */
static deck_value_t *b_bui_divider(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)args; (void)n;
    bui_ensure_init();
    deck_dvc_node_t *node = deck_dvc_node_new(&s_bui_arena, DVC_DIVIDER);
    if (!node) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "bridge.ui.divider oom"); return NULL; }
    int64_t h = bui_register(node);
    if (h < 0) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "bridge.ui: node table full"); return NULL; }
    return deck_new_int(h);
}
static deck_value_t *b_bui_spacer(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)args; (void)n;
    bui_ensure_init();
    deck_dvc_node_t *node = deck_dvc_node_new(&s_bui_arena, DVC_SPACER);
    if (!node) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "bridge.ui.spacer oom"); return NULL; }
    int64_t h = bui_register(node);
    if (h < 0) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "bridge.ui: node table full"); return NULL; }
    return deck_new_int(h);
}

/* asset.path(name:str) — look up a registered @assets entry on the
 * current module and return its path as a string. If the name is not
 * registered (or no @assets block exists), returns :none so callers can
 * pattern-match / default with `|>?`. The lookup is linear over the
 * parsed pairs; typical app declares ≤ 10 entries so this is cheap.
 *
 * Paths are returned verbatim — interpretation (relative to /assets/,
 * strip leading slash, etc.) is the responsibility of downstream
 * consumers (fs.read, media widgets). */
static deck_value_t *b_asset_path(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_STR) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "asset.path(name:str)");
        return NULL;
    }
    if (!c->module || c->module->kind != AST_MODULE) {
        /* Called outside a loaded module (e.g. raw expr via run_expr). */
        return deck_new_none();
    }
    for (uint32_t i = 0; i < c->module->as.module.items.len; i++) {
        const ast_node_t *it = c->module->as.module.items.items[i];
        if (!it || it->kind != AST_ASSETS) continue;
        for (uint32_t e = 0; e < it->as.assets.n_entries; e++) {
            const char *name = it->as.assets.names[e];
            if (!name) continue;
            size_t nlen = strlen(name);
            if (nlen == args[0]->as.s.len &&
                memcmp(name, args[0]->as.s.ptr, nlen) == 0) {
                const char *path = it->as.assets.paths[e];
                if (!path) return deck_new_none();
                return deck_new_some(deck_new_str_cstr(path));
            }
        }
    }
    return deck_new_none();
}

/* ---- Concept #46 — declarative content evaluator ----
 *
 * Walks an AST_CONTENT_BLOCK emitted by the parser (concept #45) and
 * builds a DVC tree in s_bui_arena, then encodes + pushes to the bridge.
 * Called at state entry (initial state in app_load, then on every
 * machine.send). This is the event-driven mirror of bridge.ui.* — apps
 * declare intent in `content = …` and the runtime produces the DVC tree
 * on every transition.
 *
 * Currently handled item kinds:
 *   trigger "label" [-> action_expr]        → DVC_TRIGGER + :label attr
 *   navigate "label" [-> action_expr]       → DVC_NAVIGATE + :label attr
 *   label <expr>                            → DVC_LABEL + :value attr (str)
 *   rich_text <expr>                        → DVC_RICH_TEXT + :value
 *   loading                                 → DVC_LOADING
 *   error <expr>                            → DVC_TEXT  (error message)
 *   raw <expr>                              → DVC_LABEL from expr.to_str
 *   other kinds                             → silently skipped
 *
 * Intent binding is deferred (concept #47): triggers need an intent_id
 * that the bridge sends back on tap, which maps to Machine.send(:event).
 * Today the triggers render but the user can't interact with them.  */

static deck_value_t *content_eval_expr(deck_interp_ctx_t *c, deck_env_t *env, ast_node_t *expr)
{
    if (!expr) return NULL;
    return deck_interp_run(c, env, expr);
}

static const char *content_value_as_str(deck_value_t *v, char *buf, size_t cap)
{
    if (!v) return "";
    switch (v->type) {
        case DECK_T_STR: {
            uint32_t L = v->as.s.len < cap - 1 ? v->as.s.len : (uint32_t)(cap - 1);
            memcpy(buf, v->as.s.ptr, L);
            buf[L] = 0;
            return buf;
        }
        case DECK_T_INT:   snprintf(buf, cap, "%lld", (long long)v->as.i); return buf;
        case DECK_T_FLOAT: snprintf(buf, cap, "%g", v->as.f); return buf;
        case DECK_T_BOOL:  return v->as.b ? "true" : "false";
        case DECK_T_ATOM:  snprintf(buf, cap, ":%s", v->as.atom); return buf;
        case DECK_T_UNIT:  return "unit";
        default:           return "";
    }
}

/* Concept #47 — extract the event atom from an action expression shaped like
 *   Machine.send(:event)  or  Machine.send(:event, payload_expr)
 *   machine.send(:event)  (lowercase form also accepted)
 * Returns NULL for any other shape (including bare values, fn calls with
 * non-atom first arg, nested expressions). The atom text is intern-owned. */
static const char *content_extract_event(const ast_node_t *action)
{
    if (!action || action->kind != AST_CALL) return NULL;
    const ast_node_t *fn = action->as.call.fn;
    if (!fn || fn->kind != AST_DOT) return NULL;
    const char *field = fn->as.dot.field;
    if (!field || strcmp(field, "send") != 0) return NULL;
    if (action->as.call.args.len < 1) return NULL;
    const ast_node_t *arg0 = action->as.call.args.items[0];
    if (!arg0 || arg0->kind != AST_LIT_ATOM) return NULL;
    return arg0->as.s;
}

/* Concept #58 — capture any action expression + its render-time env so
 * the tap dispatcher can evaluate it later (Machine.send, apps.launch,
 * arbitrary composition — anything). Returns the allocated intent_id, or
 * 0 when no action is present or the table is full. Retains env so it
 * survives past the walker's stack frame. Caller stamps the returned id
 * onto the DVC node. */
static uint32_t content_bind_intent(struct deck_runtime_app *app,
                                     ast_node_t *action,
                                     deck_env_t *env)
{
    if (!app || !action) return 0;
    if (app->next_intent_id >= DECK_RUNTIME_MAX_INTENTS) return 0;
    uint32_t id = app->next_intent_id++;
    app->intents[id].id = id;
    app->intents[id].action_ast = action;
    app->intents[id].captured_env = deck_env_retain(env);
    return id;
}

/* Concept #57 — set a DVC attribute on `node` from a Deck runtime value,
 * dispatching by value type. Keeps the walker's per-kind code terse: each
 * option's atom name maps 1:1 to the attr key, and the value's runtime
 * type picks the setter. Unknown types are dropped (no attr set). */
static void content_apply_value_as_attr(deck_dvc_node_t *node,
                                         const char *attr,
                                         deck_value_t *v)
{
    if (!node || !attr || !v) return;
    switch (v->type) {
        case DECK_T_BOOL:
            deck_dvc_set_bool(&s_bui_arena, node, attr, v->as.b);
            break;
        case DECK_T_INT:
            deck_dvc_set_i64(&s_bui_arena, node, attr, v->as.i);
            break;
        case DECK_T_FLOAT:
            deck_dvc_set_f64(&s_bui_arena, node, attr, v->as.f);
            break;
        case DECK_T_STR: {
            /* set_str expects a NUL-terminated string; copy into a small
             * local buffer (dvc setter re-duplicates into the arena). */
            char buf[256];
            uint32_t L = v->as.s.len < (uint32_t)(sizeof(buf) - 1)
                         ? v->as.s.len : (uint32_t)(sizeof(buf) - 1);
            memcpy(buf, v->as.s.ptr, L);
            buf[L] = 0;
            deck_dvc_set_str(&s_bui_arena, node, attr, buf);
            break;
        }
        case DECK_T_ATOM:
            if (v->as.atom)
                deck_dvc_set_atom(&s_bui_arena, node, attr, v->as.atom);
            break;
        case DECK_T_LIST: {
            /* LIST_STR — accepts lists of strings and/or atoms.
             * Non-string/atom elements are skipped silently. */
            uint32_t n = v->as.list.len;
            if (n == 0) {
                deck_dvc_set_list_str(&s_bui_arena, node, attr, NULL, 0);
                break;
            }
            const char **items = deck_arena_alloc(
                &s_bui_arena, sizeof(const char *) * n);
            if (!items) break;
            uint16_t count = 0;
            for (uint32_t i = 0; i < n; i++) {
                deck_value_t *e = v->as.list.items[i];
                if (!e) continue;
                if (e->type == DECK_T_STR) {
                    char *buf = deck_arena_alloc(&s_bui_arena, e->as.s.len + 1);
                    if (!buf) continue;
                    memcpy(buf, e->as.s.ptr, e->as.s.len);
                    buf[e->as.s.len] = 0;
                    items[count++] = buf;
                } else if (e->type == DECK_T_ATOM && e->as.atom) {
                    items[count++] = e->as.atom;
                }
            }
            deck_dvc_set_list_str(&s_bui_arena, node, attr, items, count);
            break;
        }
        default:
            break;
    }
}

/* Concept #57 — evaluate every option on a content item in `env` and
 * copy each onto `node` as a DVC attribute using the option's key as
 * the attribute name. Called once per rendered node; option names
 * therefore live in the DVC attr namespace 1:1 (`badge`, `min`, `max`,
 * `options`, `placeholder`, `value`, `state`, `mask`, `length`,
 * `prompt`, `alt`, `role`, `has_more`, etc.). Options supplied on a
 * kind that doesn't consume them still ride through — the bridge
 * decides what to do with unknown attrs on each node type. */
static void content_apply_options(deck_interp_ctx_t *c, deck_env_t *env,
                                   deck_dvc_node_t *node,
                                   const ast_node_t *ci)
{
    if (!node || !ci || ci->kind != AST_CONTENT_ITEM) return;
    for (uint32_t i = 0; i < ci->as.content_item.n_options; i++) {
        const ast_content_option_t *opt = &ci->as.content_item.options[i];
        if (!opt->key || !opt->value) continue;
        deck_value_t *v = content_eval_expr(c, env, opt->value);
        if (!v) continue;
        content_apply_value_as_attr(node, opt->key, v);
        deck_release(v);
    }
}

static void content_render(deck_interp_ctx_t *c, deck_env_t *env, const ast_node_t *block)
{
    if (!block || block->kind != AST_CONTENT_BLOCK) return;
    bui_ensure_init();
    /* Reset intent table — stale ids from previous state must not persist.
     * Concept #58 — release any retained captured env (with all its
     * bindings) from the prior render before zeroing. */
    struct deck_runtime_app *app = app_from_ctx(c);
    if (app) {
        for (uint32_t j = 0; j < DECK_RUNTIME_MAX_INTENTS; j++) {
            if (app->intents[j].captured_env)
                deck_env_release(app->intents[j].captured_env);
        }
        memset(app->intents, 0, sizeof(app->intents));
        app->next_intent_id = 1;   /* 0 reserved for "no intent" */
    }
    deck_dvc_node_t *root = deck_dvc_node_new(&s_bui_arena, DVC_FLOW);
    if (!root) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "content: root alloc"); return; }

    for (uint32_t i = 0; i < block->as.content_block.items.len; i++) {
        const ast_node_t *ci = block->as.content_block.items.items[i];
        if (!ci || ci->kind != AST_CONTENT_ITEM) continue;
        const char *kind = ci->as.content_item.kind;
        const char *label = ci->as.content_item.label;
        ast_node_t *action = ci->as.content_item.action_expr;
        ast_node_t *data   = ci->as.content_item.data_expr;
        deck_dvc_node_t *node = NULL;

        if (kind && strcmp(kind, "trigger") == 0) {
            node = deck_dvc_node_new(&s_bui_arena, DVC_TRIGGER);
            if (node && label) deck_dvc_set_str(&s_bui_arena, node, "label", label);
            /* Concept #57 — when the parser captured a label expression in
             * data_expr (multi-line `trigger expr \n -> action` form), evaluate
             * it and surface as the "label" attr. */
            if (node && !label && data) {
                deck_value_t *lv = content_eval_expr(c, env, data);
                if (lv) {
                    char buf[128];
                    const char *s = content_value_as_str(lv, buf, sizeof(buf));
                    if (s && *s) deck_dvc_set_str(&s_bui_arena, node, "label", s);
                    deck_release(lv);
                }
            }
            /* Concept #58 — capture the action expression + render env. At
             * tap time `deck_runtime_app_intent` evaluates it in a child
             * of that env, so any action shape works (Machine.send,
             * apps.launch, bluesky.post(…), composed do-blocks, …). */
            if (node && app && action) {
                uint32_t id = content_bind_intent(app, action, env);
                if (id) node->intent_id = id;
            }
        } else if (kind && strcmp(kind, "navigate") == 0) {
            node = deck_dvc_node_new(&s_bui_arena, DVC_NAVIGATE);
            if (node && label) deck_dvc_set_str(&s_bui_arena, node, "label", label);
            /* Concept #57 — data_expr-as-label for multi-line form. */
            if (node && !label && data) {
                deck_value_t *lv = content_eval_expr(c, env, data);
                if (lv) {
                    char buf[128];
                    const char *s = content_value_as_str(lv, buf, sizeof(buf));
                    if (s && *s) deck_dvc_set_str(&s_bui_arena, node, "label", s);
                    deck_release(lv);
                }
            }
            if (node && app && action) {
                uint32_t id = content_bind_intent(app, action, env);
                if (id) node->intent_id = id;
            }
        } else if (kind && strcmp(kind, "list") == 0) {
            /* Concept #48+#49 — evaluate data_expr. If a per-item template
             * `item x -> body` was parsed (concept #49), bind each list
             * element to the binder name and render the body once per
             * element as a DVC_LIST_ITEM group. Otherwise fall back to
             * the scalar-label pass-1 form. */
            node = deck_dvc_node_new(&s_bui_arena, DVC_LIST);
            if (node && (data || action)) {
                ast_node_t *expr = data ? data : action;
                deck_value_t *v = content_eval_expr(c, env, expr);
                if (v && v->type == DECK_T_LIST) {
                    bool has_template = ci->as.content_item.item_binder != NULL &&
                                        ci->as.content_item.item_body.len > 0;
                    /* Concept #52 — if the list is empty and an `empty ->`
                     * fallback body was parsed, render each fallback item as
                     * a child of the list node. */
                    if (v->as.list.len == 0 && ci->as.content_item.empty_body.len > 0) {
                        for (uint32_t k = 0; k < ci->as.content_item.empty_body.len; k++) {
                            const ast_node_t *sub = ci->as.content_item.empty_body.items[k];
                            if (!sub || sub->kind != AST_CONTENT_ITEM || !sub->as.content_item.action_expr) continue;
                            deck_value_t *lv = content_eval_expr(c, env, sub->as.content_item.action_expr);
                            if (lv) {
                                deck_dvc_node_t *emptylbl = deck_dvc_node_new(&s_bui_arena, DVC_LABEL);
                                if (emptylbl) {
                                    char buf[128];
                                    const char *s = content_value_as_str(lv, buf, sizeof(buf));
                                    if (s) deck_dvc_set_str(&s_bui_arena, emptylbl, "value", s);
                                    deck_dvc_add_child(&s_bui_arena, node, emptylbl);
                                }
                                deck_release(lv);
                            }
                        }
                    }
                    for (uint32_t j = 0; j < v->as.list.len; j++) {
                        deck_value_t *elem = v->as.list.items[j];
                        if (!elem) continue;
                        if (has_template) {
                            /* Per-item template — create a child env with the
                             * binder bound to this element, then recursively
                             * emit each body item as children of a DVC_LIST_ITEM. */
                            struct deck_runtime_app *cur_app = app_from_ctx(c);
                            deck_env_t *item_env = deck_env_new(cur_app ? &cur_app->arena : c->arena, env);
                            if (!item_env) continue;
                            deck_env_bind(cur_app ? &cur_app->arena : c->arena,
                                          item_env, ci->as.content_item.item_binder, elem);
                            deck_dvc_node_t *item_row = deck_dvc_node_new(&s_bui_arena, DVC_LIST_ITEM);
                            if (!item_row) { deck_env_release(item_env); break; }
                            /* Render each body item into item_row. Reuse the same
                             * kind-dispatch logic by recursing through a helper —
                             * but we don't have one. Inline a minimal subset
                             * (trigger / navigate / label / raw) for now; other
                             * kinds in the body are future work. */
                            for (uint32_t k = 0; k < ci->as.content_item.item_body.len; k++) {
                                const ast_node_t *sub = ci->as.content_item.item_body.items[k];
                                if (!sub || sub->kind != AST_CONTENT_ITEM) continue;
                                const char *sk = sub->as.content_item.kind;
                                const char *slabel = sub->as.content_item.label;
                                ast_node_t *sact = sub->as.content_item.action_expr;
                                deck_dvc_node_t *sn = NULL;
                                /* For trigger / navigate, evaluate the label
                                 * expression (usually a binder-field access like
                                 * `x.name`) into a string. */
                                char lbuf[128];
                                const char *lstr = slabel;
                                if (sact && !sk) sact = NULL;   /* safety */
                                if (sk && (strcmp(sk, "trigger") == 0 || strcmp(sk, "navigate") == 0)) {
                                    sn = deck_dvc_node_new(&s_bui_arena,
                                        strcmp(sk, "trigger") == 0 ? DVC_TRIGGER : DVC_NAVIGATE);
                                    /* Per-item labels typically reference the
                                     * binder (`trigger x.name`). The parser
                                     * may store that expression in either
                                     * action_expr (no tail arrow) or data_expr
                                     * (concept #57 tail-arrow form:
                                     * `trigger app.name\n  -> action`).
                                     * Priority: if data_expr is set it is
                                     * always the label; otherwise fall back
                                     * to the legacy Machine.send heuristic. */
                                    ast_node_t *label_expr = NULL;
                                    ast_node_t *action_expr2 = NULL;
                                    if (sub->as.content_item.data_expr) {
                                        label_expr = sub->as.content_item.data_expr;
                                        action_expr2 = sact;
                                    } else if (sact) {
                                        const char *ev_try = content_extract_event(sact);
                                        if (ev_try) {
                                            action_expr2 = sact;
                                        } else {
                                            label_expr = sact;
                                        }
                                    }
                                    if (sn) {
                                        if (slabel) {
                                            deck_dvc_set_str(&s_bui_arena, sn, "label", slabel);
                                        } else if (label_expr) {
                                            deck_value_t *lv = content_eval_expr(c, item_env, label_expr);
                                            if (lv) {
                                                lstr = content_value_as_str(lv, lbuf, sizeof(lbuf));
                                                if (lstr) deck_dvc_set_str(&s_bui_arena, sn, "label", lstr);
                                                deck_release(lv);
                                            }
                                        }
                                        /* Concept #58 — per-item intent binding
                                         * captures the action AST + per-item
                                         * env (which has the binder in scope
                                         * via item_env), so a tap evaluates
                                         * `Machine.send(:open, app.id)` /
                                         * `apps.launch(app.id)` / anything
                                         * else against the element's values. */
                                        if (action_expr2 && cur_app) {
                                            uint32_t id = content_bind_intent(
                                                cur_app, action_expr2, item_env);
                                            if (id) sn->intent_id = id;
                                        }
                                    }
                                } else if (sk && strcmp(sk, "label") == 0) {
                                    sn = deck_dvc_node_new(&s_bui_arena, DVC_LABEL);
                                    if (sn) {
                                        if (sact) {
                                            deck_value_t *lv = content_eval_expr(c, item_env, sact);
                                            if (lv) {
                                                lstr = content_value_as_str(lv, lbuf, sizeof(lbuf));
                                                if (lstr) deck_dvc_set_str(&s_bui_arena, sn, "value", lstr);
                                                deck_release(lv);
                                            }
                                        } else if (slabel) {
                                            deck_dvc_set_str(&s_bui_arena, sn, "value", slabel);
                                        }
                                    }
                                } else if (sk && strcmp(sk, "raw") == 0 && sact) {
                                    deck_value_t *lv = content_eval_expr(c, item_env, sact);
                                    if (lv) {
                                        sn = deck_dvc_node_new(&s_bui_arena, DVC_LABEL);
                                        if (sn) {
                                            lstr = content_value_as_str(lv, lbuf, sizeof(lbuf));
                                            if (lstr) deck_dvc_set_str(&s_bui_arena, sn, "value", lstr);
                                        }
                                        deck_release(lv);
                                    }
                                }
                                /* Concept #57 — per-item sub-node options
                                 * evaluated in the per-element env so
                                 * references like `badge: x.count` bind. */
                                if (sn) content_apply_options(c, item_env, sn, sub);
                                if (sn) deck_dvc_add_child(&s_bui_arena, item_row, sn);
                            }
                            deck_dvc_add_child(&s_bui_arena, node, item_row);
                            deck_env_release(item_env);
                        } else {
                            deck_dvc_node_t *row = deck_dvc_node_new(&s_bui_arena, DVC_LABEL);
                            if (!row) break;
                            char buf[128];
                            const char *s = content_value_as_str(elem, buf, sizeof(buf));
                            if (s) deck_dvc_set_str(&s_bui_arena, row, "value", s);
                            deck_dvc_add_child(&s_bui_arena, node, row);
                        }
                    }
                }
                if (v) deck_release(v);
            }
        } else if (kind && strcmp(kind, "group") == 0) {
            /* Concept #48 — group with label. Nested items not yet
             * unpacked from the absorbed indent block; pass-1 renders an
             * empty group header. */
            node = deck_dvc_node_new(&s_bui_arena, DVC_GROUP);
            if (node && label) deck_dvc_set_str(&s_bui_arena, node, "label", label);
        } else if (kind && strcmp(kind, "form") == 0) {
            /* Concept #60 — `form on submit -> action`. The intent is
             * captured on the DVC_FORM itself; the bridge walks the form's
             * subtree at submit tap time and aggregates every field's
             * current value by its `:name` attr into a `{name: value}`
             * map that the runtime exposes as `event.values`. */
            node = deck_dvc_node_new(&s_bui_arena, DVC_FORM);
            if (node && label) deck_dvc_set_str(&s_bui_arena, node, "label", label);
            if (node && app && action) {
                uint32_t id = content_bind_intent(app, action, env);
                if (id) node->intent_id = id;
            }
        } else if (kind && strcmp(kind, "markdown") == 0) {
            node = deck_dvc_node_new(&s_bui_arena, DVC_MARKDOWN);
            if (node && (data || action)) {
                ast_node_t *expr = data ? data : action;
                deck_value_t *lv = content_eval_expr(c, env, expr);
                if (lv) {
                    char buf[512];
                    const char *s = content_value_as_str(lv, buf, sizeof(buf));
                    if (s && *s) deck_dvc_set_str(&s_bui_arena, node, "value", s);
                    deck_release(lv);
                }
            }
        } else if (kind && strcmp(kind, "media") == 0) {
            node = deck_dvc_node_new(&s_bui_arena, DVC_MEDIA);
            if (node && (data || action)) {
                ast_node_t *expr = data ? data : action;
                deck_value_t *lv = content_eval_expr(c, env, expr);
                if (lv) {
                    char buf[256];
                    const char *s = content_value_as_str(lv, buf, sizeof(buf));
                    if (s) deck_dvc_set_str(&s_bui_arena, node, "src", s);
                    deck_release(lv);
                }
            }
        } else if (kind && strcmp(kind, "progress") == 0) {
            node = deck_dvc_node_new(&s_bui_arena, DVC_PROGRESS);
            if (node && (data || action)) {
                ast_node_t *expr = data ? data : action;
                deck_value_t *lv = content_eval_expr(c, env, expr);
                if (lv) {
                    if (lv->type == DECK_T_FLOAT)
                        deck_dvc_set_f64(&s_bui_arena, node, "value", lv->as.f);
                    else if (lv->type == DECK_T_INT)
                        deck_dvc_set_i64(&s_bui_arena, node, "value", lv->as.i);
                    deck_release(lv);
                }
            }
        } else if (kind && strcmp(kind, "status") == 0) {
            node = deck_dvc_node_new(&s_bui_arena, DVC_LABEL);
            if (node && label) deck_dvc_set_str(&s_bui_arena, node, "label", label);
            if (node && (data || action)) {
                ast_node_t *expr = data ? data : action;
                deck_value_t *lv = content_eval_expr(c, env, expr);
                if (lv) {
                    char buf[128];
                    const char *s = content_value_as_str(lv, buf, sizeof(buf));
                    if (s) deck_dvc_set_str(&s_bui_arena, node, "value", s);
                    deck_release(lv);
                }
            }
        } else if (kind && strcmp(kind, "divider") == 0) {
            node = deck_dvc_node_new(&s_bui_arena, DVC_DIVIDER);
        } else if (kind && strcmp(kind, "spacer") == 0) {
            node = deck_dvc_node_new(&s_bui_arena, DVC_SPACER);
        } else if (kind && (strcmp(kind, "toggle") == 0 ||
                             strcmp(kind, "range") == 0 ||
                             strcmp(kind, "choice") == 0 ||
                             strcmp(kind, "multiselect") == 0 ||
                             strcmp(kind, "text") == 0 ||
                             strcmp(kind, "password") == 0 ||
                             strcmp(kind, "pin") == 0 ||
                             strcmp(kind, "date") == 0 ||
                             strcmp(kind, "search") == 0 ||
                             strcmp(kind, "confirm") == 0 ||
                             strcmp(kind, "share") == 0 ||
                             strcmp(kind, "create") == 0)) {
            /* Concept #55 — input intents (§12.4). Minimal pass-1 renderers:
             * map to the spec-canonical DVC type with a label string and, if
             * the action is Machine.send(:evt), bind an intent. Detailed
             * per-widget config (options/min/max/placeholder/etc.) is a
             * future concept — the bridge will see the right node type and
             * can infer a default-styled widget. */
            deck_dvc_type_t t = DVC_LABEL;
            if      (strcmp(kind, "toggle")      == 0) t = DVC_TOGGLE;
            else if (strcmp(kind, "range")       == 0) t = DVC_SLIDER;
            else if (strcmp(kind, "choice")      == 0) t = DVC_CHOICE;
            else if (strcmp(kind, "multiselect") == 0) t = DVC_CHOICE;
            else if (strcmp(kind, "text")        == 0) t = DVC_TEXT;
            else if (strcmp(kind, "password")    == 0) t = DVC_PASSWORD;
            else if (strcmp(kind, "pin")         == 0) t = DVC_PIN;
            else if (strcmp(kind, "date")        == 0) t = DVC_DATE_PICKER;
            else if (strcmp(kind, "search")      == 0) t = DVC_TEXT;
            else if (strcmp(kind, "confirm")     == 0) t = DVC_CONFIRM;
            else if (strcmp(kind, "share")       == 0) t = DVC_SHARE;
            else if (strcmp(kind, "create")      == 0) t = DVC_TRIGGER;
            node = deck_dvc_node_new(&s_bui_arena, t);
            if (node && label) deck_dvc_set_str(&s_bui_arena, node, "label", label);
            /* Concept #60 — mark each input intent inside a form with its
             * `:name` atom so the form's submit-time aggregation can key
             * by it. The name comes from the item's data_expr/action_expr
             * if it's a bare atom literal (e.g. `toggle :lights …`). */
            if (node) {
                const ast_node_t *probe = action ? action : data;
                if (probe && probe->kind == AST_LIT_ATOM && probe->as.s)
                    deck_dvc_set_atom(&s_bui_arena, node, "name", probe->as.s);
            }
            if (node && app && action) {
                uint32_t id = content_bind_intent(app, action, env);
                if (id) node->intent_id = id;
            }
        } else if (kind && strcmp(kind, "loading") == 0) {
            node = deck_dvc_node_new(&s_bui_arena, DVC_LOADING);
        } else if (kind && strcmp(kind, "label") == 0) {
            node = deck_dvc_node_new(&s_bui_arena, DVC_LABEL);
            if (node) {
                char buf[128];
                const char *s = NULL;
                if (action) {
                    deck_value_t *v = content_eval_expr(c, env, action);
                    if (v) { s = content_value_as_str(v, buf, sizeof(buf)); deck_release(v); }
                }
                if (!s && label) s = label;
                if (s) deck_dvc_set_str(&s_bui_arena, node, "value", s);
            }
        } else if (kind && strcmp(kind, "rich_text") == 0) {
            node = deck_dvc_node_new(&s_bui_arena, DVC_RICH_TEXT);
            if (node && data) {
                deck_value_t *v = content_eval_expr(c, env, data);
                if (v) {
                    char buf[256];
                    const char *s = content_value_as_str(v, buf, sizeof(buf));
                    if (s && *s) deck_dvc_set_str(&s_bui_arena, node, "value", s);
                    deck_release(v);
                }
            }
        } else if (kind && strcmp(kind, "error") == 0) {
            node = deck_dvc_node_new(&s_bui_arena, DVC_LABEL);
            if (node && action) {
                deck_value_t *v = content_eval_expr(c, env, action);
                if (v) {
                    char buf[256];
                    const char *s = content_value_as_str(v, buf, sizeof(buf));
                    if (s && *s) deck_dvc_set_str(&s_bui_arena, node, "value", s);
                    deck_release(v);
                }
            }
        } else if (kind && strcmp(kind, "raw") == 0) {
            /* Bare expression — render as DVC_LABEL from its string form. */
            if (action) {
                deck_value_t *v = content_eval_expr(c, env, action);
                if (v) {
                    node = deck_dvc_node_new(&s_bui_arena, DVC_LABEL);
                    if (node) {
                        char buf[128];
                        const char *s = content_value_as_str(v, buf, sizeof(buf));
                        if (s) deck_dvc_set_str(&s_bui_arena, node, "value", s);
                    }
                    deck_release(v);
                }
            }
        }
        /* Other kinds (list/group/form/media/markdown…) skipped in pass 1. */

        /* Concept #57 — apply per-widget options (`badge: 3`,
         * `placeholder: "…"`, `min: 0`, `options: [:a, :b]`, …) onto the
         * DVC node. Done once per top-level item; per-item template
         * sub-nodes get their own call inside the list branch. */
        if (node) content_apply_options(c, env, node, ci);

        if (node) deck_dvc_add_child(&s_bui_arena, root, node);
    }

    /* Encode + push. Mirrors bridge.ui.render. */
    size_t need = 0;
    (void)deck_dvc_encode(root, NULL, 0, &need);
    if (need == 0) { bui_reset(); return; }
    uint8_t *buf = deck_arena_alloc(&s_bui_arena, need);
    if (!buf) { bui_reset(); return; }
    size_t wrote = 0;
    if (deck_dvc_encode(root, buf, need, &wrote) != DECK_RT_OK) { bui_reset(); return; }
    deck_sdi_bridge_ui_push_snapshot(buf, wrote);
    bui_reset();
}

/* Helper — locate the content block in a state, render it with the given env. */
static void content_render_state(deck_interp_ctx_t *c, deck_env_t *env, const ast_node_t *state)
{
    if (!state || state->kind != AST_STATE) return;
    for (uint32_t i = 0; i < state->as.state.hooks.len; i++) {
        const ast_node_t *h = state->as.state.hooks.items[i];
        if (h && h->kind == AST_CONTENT_BLOCK) {
            content_render(c, env, h);
            return;
        }
    }
}

/* bridge.ui.render(root_handle) — encode the tree rooted at `root_handle`,
 * push it via the SDI, then reset the render arena. Returns unit. */
static deck_value_t *b_bui_render(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n;
    if (!args[0] || args[0]->type != DECK_T_INT) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "bridge.ui.render(root:handle)"); return NULL;
    }
    deck_dvc_node_t *root = bui_lookup(args[0]->as.i);
    if (!root) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0,
                "bridge.ui.render: root handle=%lld invalid", (long long)args[0]->as.i);
        return NULL;
    }

    /* deck_dvc_encode in sizing mode (buf=NULL) returns DECK_RT_NO_MEMORY
     * by design — it walks the tree and updates the overflow flag for
     * every would-be write. The sizing path is valid only if `need` ends
     * up non-zero; that's the contract established by the dvc selftest. */
    size_t need = 0;
    (void)deck_dvc_encode(root, NULL, 0, &need);
    if (need == 0) {
        set_err(c, DECK_RT_INTERNAL, 0, 0, "bridge.ui.render: encode sizing returned 0");
        return NULL;
    }
    uint8_t *buf = deck_arena_alloc(&s_bui_arena, need);
    if (!buf) {
        set_err(c, DECK_RT_NO_MEMORY, 0, 0, "bridge.ui.render: buffer oom (%u bytes)", (unsigned)need);
        return NULL;
    }
    size_t wrote = 0;
    if (deck_dvc_encode(root, buf, need, &wrote) != DECK_RT_OK) {
        set_err(c, DECK_RT_INTERNAL, 0, 0, "bridge.ui.render: encode failed");
        return NULL;
    }
    deck_sdi_err_t rc = deck_sdi_bridge_ui_push_snapshot(buf, wrote);
    if (rc != DECK_SDI_OK) {
        /* Non-fatal: reset and surface the error. The arena reset is
         * important or the next build session starts with stale nodes. */
        bui_reset();
        set_err(c, DECK_RT_INTERNAL, 0, 0, "bridge.ui.render push: %s",
                deck_sdi_strerror(rc));
        return NULL;
    }
    bui_reset();
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
    { "log.debug",              b_log_debug,         1, 1 },
    { "log.info",               b_log_info,          1, 1 },
    { "log.warn",               b_log_warn,          1, 1 },
    { "log.error",              b_log_error,         1, 1 },

    /* time */
    { "time.now",               b_time_now,          0, 0 },
    { "time.now_us",            b_time_now_us,       0, 0 },
    { "time.duration",          b_time_duration,     2, 2 },
    { "time.to_iso",            b_time_to_iso,       1, 1 },
    /* Concept #32 — §3 @builtin time completeness. Timestamps in epoch
     * seconds, Durations in seconds. */
    { "time.since",             b_time_since,        1, 1 },
    { "time.until",             b_time_until,        1, 1 },
    { "time.add",               b_time_add,          2, 2 },
    { "time.sub",               b_time_sub,          2, 2 },
    { "time.before",            b_time_before,       2, 2 },
    { "time.after",             b_time_after,        2, 2 },
    { "time.epoch",             b_time_epoch,        0, 0 },
    { "time.format",            b_time_format,       2, 2 },
    { "time.parse",             b_time_parse,        2, 2 },
    { "time.from_iso",          b_time_from_iso,     1, 1 },
    { "time.date_parts",        b_time_date_parts,   1, 1 },
    { "time.day_of_week",       b_time_day_of_week,  1, 1 },
    { "time.start_of_day",      b_time_start_of_day, 1, 1 },
    { "time.duration_parts",    b_time_duration_parts, 1, 1 },
    { "time.duration_str",      b_time_duration_str, 1, 1 },
    { "time.ago",               b_time_ago,          1, 1 },

    /* system.info */
    { "system.info.device_id",    b_info_device_id,    0, 0 },
    { "system.info.free_heap",    b_info_free_heap,    0, 0 },
    { "system.info.deck_level",   b_info_deck_level,   0, 0 },
    /* Concept #39 — rest of §3 system.info surface. */
    { "system.info.device_model", b_info_device_model, 0, 0 },
    { "system.info.os_name",      b_info_os_name,      0, 0 },
    { "system.info.os_version",   b_info_os_version,   0, 0 },
    { "system.info.app_id",       b_info_app_id,       0, 0 },
    { "system.info.app_version",  b_info_app_version,  0, 0 },
    { "system.info.uptime",       b_info_uptime,       0, 0 },
    { "system.info.cpu_freq_mhz", b_info_cpu_freq,     0, 0 },
    { "system.info.versions",     b_info_versions,     0, 0 },

    /* text — spec 03-deck-os §3 (post-#15a unification on `len`).
     * `len` / `starts` / `ends` match §11.2's list.len convention;
     * the earlier `length`/`_with` forms are no longer registered. */
    { "text.upper",             b_text_upper,        1, 1 },
    { "text.lower",             b_text_lower,        1, 1 },
    { "text.len",               b_text_len,          1, 1 },
    { "text.starts",            b_text_starts_with,  2, 2 },
    { "text.ends",              b_text_ends_with,    2, 2 },
    { "text.contains",          b_text_contains,     2, 2 },
    { "text.split",             b_text_split,        2, 2 },
    { "text.repeat",            b_text_repeat,       2, 2 },
    { "text.trim",              b_text_trim,         1, 1 },
    { "text.is_empty",          b_text_is_empty,     1, 1 },
    { "text.is_blank",          b_text_is_blank,     1, 1 },
    { "text.join",              b_text_join,         2, 2 },
    { "text.index_of",          b_text_index_of,     2, 2 },
    { "text.count",             b_text_count,        2, 2 },
    { "text.slice",             b_text_slice,        3, 3 },
    { "text.replace",           b_text_replace,      3, 3 },
    { "text.replace_all",       b_text_replace_all,  3, 3 },
    { "text.lines",             b_text_lines,        1, 1 },
    { "text.words",             b_text_words,        1, 1 },
    { "text.truncate",          b_text_truncate,     2, 3 },
    { "text.pad_left",          b_text_pad_left,     3, 3 },
    { "text.pad_right",         b_text_pad_right,    3, 3 },
    { "text.pad_center",        b_text_pad_center,   3, 3 },
    /* Concept #28 — §3 codecs. Self-contained implementations (no SDI). */
    { "text.base64_encode",     b_text_base64_encode, 1, 1 },
    { "text.base64_decode",     b_text_base64_decode, 1, 1 },
    { "text.url_encode",        b_text_url_encode,    1, 1 },
    { "text.url_decode",        b_text_url_decode,    1, 1 },
    { "text.hex_encode",        b_text_hex_encode,    1, 1 },
    { "text.hex_decode",        b_text_hex_decode,    1, 1 },
    /* Concept #29 — bytes / query. */
    { "text.bytes",             b_text_bytes,         1, 1 },
    { "text.from_bytes",        b_text_from_bytes,    1, 1 },
    { "text.query_build",       b_text_query_build,   1, 1 },
    { "text.query_parse",       b_text_query_parse,   1, 1 },
    { "text.format",            b_text_format,        2, 2 },
    /* Concept #31 — JSON. */
    { "text.json",              b_text_json,          1, 1 },
    { "text.from_json",         b_text_from_json,     1, 1 },

    /* list (DL2 F21.4 + F22 stdlib) */
    { "list.len",               b_list_len,          1, 1 },
    { "list.head",              b_list_head,         1, 1 },
    { "list.tail",              b_list_tail,         1, 1 },
    { "list.get",               b_list_get,          2, 2 },
    { "list.map",               b_list_map,          2, 2 },
    { "list.filter",            b_list_filter,       2, 2 },
    { "list.reduce",            b_list_reduce,       3, 3 },
    /* Concept #41 — §11.2 list ops pass 1. Deferred: sort/group_by/chunk/
     * window/zip/flat_map/unique/partition/tabulate/interleave/sort_*. */
    { "list.last",              b_list_last,         1, 1 },
    { "list.append",            b_list_append,       2, 2 },
    { "list.prepend",           b_list_prepend,      2, 2 },
    { "list.reverse",           b_list_reverse,      1, 1 },
    { "list.take",              b_list_take,         2, 2 },
    { "list.drop",              b_list_drop,         2, 2 },
    { "list.contains",          b_list_contains,     2, 2 },
    { "list.find",              b_list_find,         2, 2 },
    { "list.find_index",        b_list_find_index,   2, 2 },
    { "list.count_where",       b_list_count_where,  2, 2 },
    { "list.any",               b_list_any,          2, 2 },
    { "list.all",               b_list_all,          2, 2 },
    { "list.none",              b_list_none,         2, 2 },
    { "list.sum",               b_list_sum,          1, 1 },
    { "list.sum_f",             b_list_sum_f,        1, 1 },
    { "list.avg",               b_list_avg,          1, 1 },
    { "list.flatten",           b_list_flatten,      1, 1 },
    /* Concept #43 — list.* pass 2. */
    { "list.enumerate",         b_list_enumerate,    1, 1 },
    { "list.zip",               b_list_zip,          2, 2 },
    { "list.zip_with",          b_list_zip_with,     3, 3 },
    { "list.flat_map",          b_list_flat_map,     2, 2 },
    { "list.partition",         b_list_partition,    2, 2 },
    { "list.unique",            b_list_unique,       1, 1 },
    { "list.sort",              b_list_sort,         1, 1 },
    { "list.min_by",            b_list_min_by,       2, 2 },
    { "list.max_by",            b_list_max_by,       2, 2 },

    /* map (DL2 F21.6) */
    { "map.len",                b_map_len,           1, 1 },
    { "map.count",              b_map_len,           1, 1 },   /* spec §11.3 alias */
    { "map.get",                b_map_get_b,         2, 2 },
    /* Spec §11.3 calls the spec-canonical name `map.set`; the legacy
     * `map.put` is not registered (no-shim policy from concept #8 etc). */
    { "map.set",                b_map_put_b,         3, 3 },
    { "map.keys",               b_map_keys,          1, 1 },
    { "map.values",             b_map_values,        1, 1 },
    /* Concept #42 — §11.3 map completeness. */
    { "map.delete",             b_map_delete,        2, 2 },
    { "map.has",                b_map_has,           2, 2 },
    { "map.merge",              b_map_merge,         2, 2 },
    { "map.is_empty",           b_map_is_empty,      1, 1 },
    { "map.map_values",         b_map_map_values,    2, 2 },
    { "map.filter",             b_map_filter,        2, 2 },
    { "map.to_list",            b_map_to_list,       1, 1 },
    { "map.from_list",          b_map_from_list,     1, 1 },
    /* Concept #42 — §11.4 tuple ops. */
    /* Concept #44 — machine.send / machine.state (spec §8.4).
     * Also registered under capitalized `Machine.*` since spec examples
     * use the capitalized form. */
    { "machine.send",           b_machine_send,      1, 2 },
    { "machine.state",          b_machine_state,     0, 0 },
    { "Machine.send",           b_machine_send,      1, 2 },
    { "Machine.state",          b_machine_state,     0, 0 },

    { "tup.fst",                b_tup_fst,           1, 1 },
    { "tup.snd",                b_tup_snd,           1, 1 },
    { "tup.third",              b_tup_third,         1, 1 },
    { "tup.swap",               b_tup_swap,          1, 1 },
    { "tup.map_fst",            b_tup_map_fst,       2, 2 },
    { "tup.map_snd",            b_tup_map_snd,       2, 2 },

    /* bytes */
    { "bytes.len",              b_bytes_len,         1, 1 },
    /* Concept #40 — §3 @builtin bytes. Accepts [int] or bytes; emits [int]. */
    { "bytes.concat",           b_bytes_concat,      2, 2 },
    { "bytes.slice",            b_bytes_slice,       3, 3 },
    { "bytes.to_int_be",        b_bytes_to_int_be,   1, 1 },
    { "bytes.to_int_le",        b_bytes_to_int_le,   1, 1 },
    { "bytes.from_int",         b_bytes_from_int,    3, 3 },
    { "bytes.xor",              b_bytes_xor,         2, 2 },
    { "bytes.fill",             b_bytes_fill,        2, 2 },

    /* math */
    { "math.abs",               b_math_abs,          1, 1 },
    { "math.min",               b_math_min,          2, 2 },
    { "math.max",               b_math_max,          2, 2 },
    { "math.floor",             b_math_floor,        1, 1 },
    { "math.ceil",              b_math_ceil,         1, 1 },
    { "math.round",             b_math_round,        1, 2 },
    /* Concept #37 — §3 @builtin math completeness. */
    { "math.sqrt",              b_math_sqrt,         1, 1 },
    { "math.pow",               b_math_pow,          2, 2 },
    { "math.clamp",             b_math_clamp,        3, 3 },
    { "math.lerp",              b_math_lerp,         3, 3 },
    { "math.sin",               b_math_sin,          1, 1 },
    { "math.cos",               b_math_cos,          1, 1 },
    { "math.tan",               b_math_tan,          1, 1 },
    { "math.asin",              b_math_asin,         1, 1 },
    { "math.acos",              b_math_acos,         1, 1 },
    { "math.atan",              b_math_atan,         1, 1 },
    { "math.atan2",             b_math_atan2,        2, 2 },
    { "math.exp",               b_math_exp,          1, 1 },
    { "math.ln",                b_math_ln,           1, 1 },
    { "math.log2",              b_math_log2,         1, 1 },
    { "math.log10",             b_math_log10,        1, 1 },
    { "math.sign",              b_math_sign,         1, 1 },
    { "math.is_nan",            b_math_is_nan,       1, 1 },
    { "math.is_inf",            b_math_is_inf,       1, 1 },
    { "math.to_radians",        b_math_to_radians,   1, 1 },
    { "math.to_degrees",        b_math_to_degrees,   1, 1 },
    { "math.abs_int",           b_math_abs_int,      1, 1 },
    { "math.min_int",           b_math_min_int,      2, 2 },
    { "math.max_int",           b_math_max_int,      2, 2 },
    { "math.clamp_int",         b_math_clamp_int,    3, 3 },
    { "math.gcd",               b_math_gcd,          2, 2 },
    { "math.lcm",               b_math_lcm,          2, 2 },
    /* Constants — 0-arity so AST_DOT auto-dispatches on `math.pi` etc. */
    { "math.pi",                b_math_pi,           0, 0 },
    { "math.e",                 b_math_e,            0, 0 },
    { "math.tau",               b_math_tau,          0, 0 },

    /* nvs */
    /* Concept #35 — spec §3 arity (1-arg/2-arg, implicit app-scoped ns). */
    { "nvs.get",                b_nvs_get,           1, 1 },
    { "nvs.set",                b_nvs_set,           2, 2 },
    { "nvs.delete",             b_nvs_delete,        1, 1 },
    { "nvs.get_int",            b_nvs_get_int,       1, 1 },
    { "nvs.set_int",            b_nvs_set_int,       2, 2 },
    { "nvs.get_bool",           b_nvs_get_bool,      1, 1 },
    { "nvs.set_bool",           b_nvs_set_bool,      2, 2 },
    { "nvs.get_float",          b_nvs_get_float,     1, 1 },
    { "nvs.set_float",          b_nvs_set_float,     2, 2 },
    { "nvs.get_bytes",          b_nvs_get_bytes,     1, 1 },
    { "nvs.set_bytes",          b_nvs_set_bytes,     2, 2 },
    { "nvs.keys",               b_nvs_keys,          0, 0 },
    { "nvs.clear",              b_nvs_clear,         0, 0 },

    /* fs (read-only) */
    { "fs.exists",              b_fs_exists,         1, 1 },
    { "fs.read",                b_fs_read,           1, 1 },
    { "fs.list",                b_fs_list,           1, 1 },
    /* Concept #34 — fs.* write surface. All return Result unit fs.Error. */
    { "fs.write",               b_fs_write,          2, 2 },
    { "fs.append",              b_fs_append,         2, 2 },
    { "fs.delete",              b_fs_delete,         1, 1 },
    { "fs.mkdir",               b_fs_mkdir,          1, 1 },
    { "fs.move",                b_fs_move,           2, 2 },
    /* Concept #36 — bytes surface. */
    { "fs.read_bytes",          b_fs_read_bytes,     1, 1 },
    { "fs.write_bytes",         b_fs_write_bytes,    2, 2 },

    /* os lifecycle (single-app stubs in DL1) */
    { "os.resume",              b_os_resume,         0, 0 },
    { "os.suspend",             b_os_suspend,        0, 0 },
    { "os.terminate",           b_os_terminate,      0, 0 },
    { "os.sleep_ms",            b_os_sleep_ms,       1, 1 },

    /* assets (DL2 F28.5) */
    { "asset.path",             b_asset_path,        1, 1 },

    /* bridge.ui — DVC tree builders (DL2 F28 Phase 2) */
    { "bridge.ui.label",        b_bui_label,         1, 1 },
    { "bridge.ui.trigger",      b_bui_trigger,       2, 2 },
    { "bridge.ui.column",       b_bui_column,        1, 1 },
    { "bridge.ui.row",          b_bui_row,           1, 1 },
    { "bridge.ui.group",        b_bui_group,         2, 2 },
    { "bridge.ui.data_row",     b_bui_data_row,      2, 2 },
    { "bridge.ui.divider",      b_bui_divider,       0, 0 },
    { "bridge.ui.spacer",       b_bui_spacer,        0, 0 },
    { "bridge.ui.render",       b_bui_render,        1, 1 },

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
/* Spec 01-deck-lang §11.5 (post-#20) — polymorphic `unwrap_or` returns
 * `default` when the wrapper is empty (Optional :none) or errored
 * (Result :err). Replaces the former unwrap_or-for-Result + unwrap_opt_or
 * -for-Optional pair under one callable. */
static deck_value_t *b_unwrap_or(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)n; (void)c;
    if (!args[0]) return deck_retain(args[1]);
    if (args[0]->type == DECK_T_OPTIONAL)
        return deck_retain(args[0]->as.opt.inner ? args[0]->as.opt.inner : args[1]);
    if (result_tag_is(args[0], "ok"))
        return deck_retain(args[0]->as.tuple.items[1]);
    if (result_tag_is(args[0], "err"))
        return deck_retain(args[1]);
    return deck_retain(args[0]);
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
    { "unwrap",    b_unwrap,    1, 1 },
    { "unwrap_or", b_unwrap_or, 2, 2 },   /* polymorphic — §11.5 post-#20 */
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
            /* DL2 F22 — built-in variants plus spec 01-deck-lang §3.7 atom
             * variants. Accepted value shapes:
             *   some(p) / :some x:  DECK_T_OPTIONAL with inner != NULL, or
             *                       2-tuple (:some, payload) from a `:some x`
             *                       constructor expression.
             *   :ctor x   (generic):  2-tuple (:ctor, payload). `ok`, `err`,
             *                       and every user-declared variant go here.
             */
            if (!val) return false;
            const char *ctor = pat->as.pat_variant.ctor;
            if (!ctor) return false;
            if (pat->as.pat_variant.n_subs != 1) return false;
            if (strcmp(ctor, "some") == 0 &&
                val->type == DECK_T_OPTIONAL && val->as.opt.inner != NULL) {
                return match_pattern(a, env, pat->as.pat_variant.subs[0], val->as.opt.inner);
            }
            if (val->type == DECK_T_TUPLE && val->as.tuple.arity == 2) {
                deck_value_t *tag = val->as.tuple.items[0];
                if (!tag || tag->type != DECK_T_ATOM) return false;
                if (strcmp(tag->as.atom, ctor) != 0) return false;
                return match_pattern(a, env, pat->as.pat_variant.subs[0], val->as.tuple.items[1]);
            }
            return false;
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
             * up the field. Records use atom keys; maps built from external
             * sources (JSON objects, time.date_parts) use string keys — try
             * both so `obj.field` Just Works regardless of how the map was
             * constructed. Missing field → none. */
            deck_value_t *obj = deck_interp_run(c, env, n->as.dot.obj);
            if (!obj) break;
            if (obj->type == DECK_T_MAP) {
                deck_value_t *akey = deck_new_atom(n->as.dot.field);
                deck_value_t *v = akey ? deck_map_get(obj, akey) : NULL;
                if (akey) deck_release(akey);
                if (!v) {
                    deck_value_t *skey = deck_new_str_cstr(n->as.dot.field);
                    v = skey ? deck_map_get(obj, skey) : NULL;
                    if (skey) deck_release(skey);
                }
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
        set_err(c, DECK_RT_TYPE_MISMATCH, ln, co, "++ needs two strings");
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
            /* Mutual tail call: rebind the existing env's parent to the
             * new fn's closure. Reusing the env (instead of allocating
             * a fresh one each iteration) keeps mutually recursive deep
             * loops from blowing the arena — `is_even`/`is_odd` over
             * 2000 levels would otherwise leak ~2000 envs. */
            env_unbind_all(call_env);
            deck_env_t *new_closure = next_fn->as.fn.closure;
            if (new_closure) new_closure->refcount++;
            deck_env_t *old_parent = call_env->parent;
            call_env->parent = new_closure;
            deck_env_release(old_parent);
            deck_release(current);
            current = next_fn;              /* transfer pending retain */
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

/* DL2 F28.1 — look up `@machine.before` / `@machine.after` bodies. Parser
 * tags them as AST_ON with reserved event names "__machine_before" and
 * "__machine_after". Returns NULL if not present in the module. */
static const ast_node_t *find_on_event(const ast_node_t *mod, const char *event)
{
    if (!mod || !event || mod->kind != AST_MODULE) return NULL;
    for (uint32_t i = 0; i < mod->as.module.items.len; i++) {
        const ast_node_t *it = mod->as.module.items.items[i];
        if (it && it->kind == AST_ON && it->as.on.event &&
            strcmp(it->as.on.event, event) == 0) return it;
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

static deck_err_t run_machine(const ast_node_t *machine,
                              const ast_node_t *mod,
                              deck_interp_ctx_t *c)
{
    if (!machine || machine->as.machine.states.len == 0) return DECK_RT_OK;
    /* DL2 F28.1 — @machine.before / .after bodies run around each transition
     * (after the source state's `on leave`, before the destination state's
     * `on enter`). They see the global env and may reference top-level fn's
     * and @use'd builtins. Bodies are optional — NULL when not declared. */
    const ast_node_t *before = find_on_event(mod, "__machine_before");
    const ast_node_t *after  = find_on_event(mod, "__machine_after");
    /* Spec 02-deck-app §8.2 — honour explicit `initial :atom` if declared;
     * else fall back to the first state in declaration order. */
    const ast_node_t *state = NULL;
    if (machine->as.machine.initial_state) {
        state = find_state(machine, machine->as.machine.initial_state);
        if (!state) {
            ESP_LOGE(TAG, "machine '%s' initial :%s not found",
                     machine->as.machine.name,
                     machine->as.machine.initial_state);
            return DECK_RT_PATTERN_FAILED;
        }
    } else {
        state = machine->as.machine.states.items[0];
    }
    ESP_LOGI(TAG, "machine '%s' start state :%s",
             machine->as.machine.name, state->as.state.name);

    /* Concept #44 — when the machine declares top-level transitions
     * (spec §8.4 event-driven form), we don't auto-run. The machine sits
     * in its initial state, and user code drives transitions via
     * `machine.send(:event, payload)`. Legacy sequential flows (no top-
     * level transitions, all progression via state-internal `transition`
     * hooks) keep the auto-loop below. */
    if (machine->as.machine.transitions.len > 0) {
        const char *ignored = NULL;
        run_state_hooks(c, state, "enter", &ignored);
        /* Concept #46 — render the initial state's declarative content. */
        content_render_state(c, c->global, state);
        /* The machine now sits waiting for send() calls. The caller
         * (deck_runtime_app_load) records app->machine_state separately. */
        return c->err;
    }

    for (int steps = 0; steps < DECK_MACHINE_MAX_TRANSITIONS; steps++) {
        const char *next = NULL;
        run_state_hooks(c, state, "enter", &next);
        if (c->err != DECK_RT_OK) return c->err;
        /* Concept #46 parity — sequential machine must also render declarative
         * content on each state entry so `content =` blocks in @flow / non-
         * event-driven @machine states aren't silently dropped. Matches the
         * event-driven branch above. */
        content_render_state(c, c->global, state);
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

        if (before && before->as.on.body) {
            deck_value_t *r = deck_interp_run(c, c->global, before->as.on.body);
            if (r) deck_release(r);
            if (c->err != DECK_RT_OK) return c->err;
        }

        const ast_node_t *nextst = find_state(machine, next);
        if (!nextst) {
            set_err(c, DECK_RT_INTERNAL, state->line, state->col,
                    "transition target :%s not found", next);
            return c->err;
        }
        ESP_LOGI(TAG, "machine '%s' :%s -> :%s",
                 machine->as.machine.name, state->as.state.name, next);
        state = nextst;

        if (after && after->as.on.body) {
            deck_value_t *r = deck_interp_run(c, c->global, after->as.on.body);
            if (r) deck_release(r);
            if (c->err != DECK_RT_OK) return c->err;
        }
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
    c.module = ld.module;

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
        if (machine) run_machine(machine, ld.module, &c);
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

/* ----- DL2 F28: persistent app handles ---------------------------------
 *
 * Mirrors deck_runtime_run_on_launch but keeps arena+env alive after
 * launch so later dispatch() calls can re-enter @on <event> bodies. The
 * module AST, interned atoms, top-level fn bindings, and any let bindings
 * created during @on launch remain live until unload.
 */
/* Concept #47 — struct deck_runtime_app was moved forward (near the top
 * of this file) so content_render can access app->intents directly.
 * This spot left as a locator comment only. */

static struct deck_runtime_app s_app_slots[DECK_RUNTIME_MAX_APPS];

static struct deck_runtime_app *app_slot_alloc(void)
{
    for (int i = 0; i < DECK_RUNTIME_MAX_APPS; i++) {
        if (!s_app_slots[i].in_use) {
            memset(&s_app_slots[i], 0, sizeof(s_app_slots[i]));
            s_app_slots[i].in_use = true;
            return &s_app_slots[i];
        }
    }
    return NULL;
}

static void app_slot_free(struct deck_runtime_app *app)
{
    if (!app) return;
    app->in_use = false;
}

/* ---- machine.send / machine.state (concept #44 — spec §8.4) ----
 * Defined here (after s_app_slots) so app_from_ctx can scan the array.
 * Forward-declared near the top of the file for the BUILTINS table. */
static struct deck_runtime_app *app_from_ctx(deck_interp_ctx_t *c)
{
    for (int i = 0; i < DECK_RUNTIME_MAX_APPS; i++) {
        if (s_app_slots[i].in_use && &s_app_slots[i].ctx == c) return &s_app_slots[i];
    }
    return NULL;
}

static deck_value_t *b_machine_send(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    if (!args[0] || args[0]->type != DECK_T_ATOM) {
        set_err(c, DECK_RT_TYPE_MISMATCH, 0, 0, "machine.send(:event, payload?)"); return NULL;
    }
    struct deck_runtime_app *app = app_from_ctx(c);
    if (!app) { set_err(c, DECK_RT_INTERNAL, 0, 0, "machine.send: no app context"); return NULL; }
    const char *event = args[0]->as.atom;
    deck_value_t *payload = (n >= 2) ? args[1] : NULL;

    const ast_node_t *machine = find_machine(app->ld.module);
    if (!machine) return deck_new_none();
    if (!app->machine_state) return deck_new_none();

    /* Linear scan for the first matching transition. Deferred: specificity
     * resolution when multiple transitions match. */
    const ast_node_t *match_tn = NULL;
    for (uint32_t i = 0; i < machine->as.machine.transitions.len; i++) {
        const ast_node_t *tn = machine->as.machine.transitions.items[i];
        if (!tn || tn->kind != AST_MACHINE_TRANSITION) continue;
        if (!tn->as.machine_transition.event) continue;
        if (strcmp(tn->as.machine_transition.event, event) != 0) continue;
        if (tn->as.machine_transition.from_state &&
            strcmp(tn->as.machine_transition.from_state, app->machine_state) != 0) continue;
        match_tn = tn;
        break;
    }
    if (!match_tn) return deck_new_none();

    /* Bind payload to `event` identifier in a child env for when/before/after bodies. */
    deck_env_t *bind_env = deck_env_new(&app->arena, c->global);
    if (!bind_env) { set_err(c, DECK_RT_NO_MEMORY, 0, 0, "machine.send alloc"); return NULL; }
    deck_env_bind(&app->arena, bind_env, "event",
                  payload ? payload : deck_unit());

    if (match_tn->as.machine_transition.when_expr) {
        deck_value_t *w = deck_interp_run(c, bind_env, match_tn->as.machine_transition.when_expr);
        if (!w) { deck_env_release(bind_env); return NULL; }
        bool truthy = deck_is_truthy(w);
        deck_release(w);
        if (!truthy) { deck_env_release(bind_env); return deck_new_none(); }
    }

    const ast_node_t *src_state = find_state(machine, app->machine_state);
    if (src_state) {
        run_state_hooks(c, src_state, "leave", NULL);
        if (c->err != DECK_RT_OK) { deck_env_release(bind_env); return NULL; }
    }

    if (match_tn->as.machine_transition.before_body) {
        deck_value_t *r = deck_interp_run(c, bind_env, match_tn->as.machine_transition.before_body);
        if (r) deck_release(r);
        if (c->err != DECK_RT_OK) { deck_env_release(bind_env); return NULL; }
    }

    app->machine_state = match_tn->as.machine_transition.to_state;
    ESP_LOGI(TAG, "machine.send(:%s): :%s -> :%s", event,
             src_state ? src_state->as.state.name : "?",
             app->machine_state);

    const ast_node_t *dst_state = find_state(machine, app->machine_state);
    if (dst_state) {
        const char *ignored = NULL;
        run_state_hooks(c, dst_state, "enter", &ignored);
        if (c->err != DECK_RT_OK) { deck_env_release(bind_env); return NULL; }
        /* Concept #46 — re-render the destination state's declarative
         * content after on_enter runs, so apps see the state's UI on
         * every transition. */
        content_render_state(c, bind_env, dst_state);
    }

    if (match_tn->as.machine_transition.after_body) {
        deck_value_t *r = deck_interp_run(c, bind_env, match_tn->as.machine_transition.after_body);
        if (r) deck_release(r);
    }

    deck_env_release(bind_env);
    return deck_new_atom(app->machine_state);
}

static deck_value_t *b_machine_state(deck_value_t **args, uint32_t n, deck_interp_ctx_t *c)
{
    (void)args; (void)n;
    struct deck_runtime_app *app = app_from_ctx(c);
    if (!app || !app->machine_state) return deck_new_none();
    return deck_new_some(deck_new_atom(app->machine_state));
}

/* Concept #58/#59/#60 — bridge intent dispatch.
 *
 * The binding holds a captured action AST and the render-time env. At tap
 * time we build a child env on top of captured_env and, if the bridge
 * supplied a value payload, bind `event` to a `{value: v}` / `{values: {…}}`
 * map there. Evaluating `action_ast` in that env runs whatever the author
 * wrote — `Machine.send(:e, event.value)`, `apps.launch(app.id)`, a `do`
 * block chaining several calls, etc. */
static deck_value_t *intent_val_to_deck(const deck_intent_val_t *v)
{
    switch (v->kind) {
        case DECK_INTENT_VAL_BOOL: return deck_new_bool(v->b);
        case DECK_INTENT_VAL_I64:  return deck_new_int (v->i);
        case DECK_INTENT_VAL_F64:  return deck_new_float(v->f);
        case DECK_INTENT_VAL_STR:  return deck_new_str_cstr(v->s ? v->s : "");
        case DECK_INTENT_VAL_ATOM: return v->s ? deck_new_atom(v->s) : NULL;
        default: return NULL;
    }
}

static deck_value_t *make_intent_event_value(const deck_intent_val_t *vals,
                                              uint32_t n_vals)
{
    if (!vals || n_vals == 0) return NULL;
    deck_value_t *event_map = deck_new_map(4);
    if (!event_map) return NULL;
    bool any_keyed = false;
    for (uint32_t i = 0; i < n_vals; i++) {
        if (vals[i].key && vals[i].key[0]) { any_keyed = true; break; }
    }
    if (n_vals == 1 && !any_keyed) {
        /* Scalar payload — expose as `event.value`. */
        deck_value_t *v = intent_val_to_deck(&vals[0]);
        if (v) {
            deck_value_t *k = deck_new_atom("value");
            if (k) { deck_map_put(event_map, k, v); deck_release(k); }
            deck_release(v);
        }
        return event_map;
    }
    /* Keyed entries — expose as `event.values` map. */
    deck_value_t *values_map = deck_new_map(n_vals);
    if (!values_map) { deck_release(event_map); return NULL; }
    for (uint32_t i = 0; i < n_vals; i++) {
        const char *k = vals[i].key;
        if (!k || !*k) continue;
        deck_value_t *v = intent_val_to_deck(&vals[i]);
        if (!v) continue;
        deck_value_t *kv = deck_new_atom(k);
        if (kv) { deck_map_put(values_map, kv, v); deck_release(kv); }
        deck_release(v);
    }
    deck_value_t *values_k = deck_new_atom("values");
    if (values_k) { deck_map_put(event_map, values_k, values_map); deck_release(values_k); }
    deck_release(values_map);
    return event_map;
}

deck_err_t deck_runtime_app_intent_v(deck_runtime_app_t *app,
                                      uint32_t intent_id,
                                      const deck_intent_val_t *vals,
                                      uint32_t n_vals)
{
    if (!app || !app->in_use) return DECK_RT_INTERNAL;
    if (intent_id == 0 || intent_id >= DECK_RUNTIME_MAX_INTENTS) return DECK_RT_OK;
    deck_intent_binding_t *b = &app->intents[intent_id];
    if (b->id != intent_id || !b->action_ast) return DECK_RT_OK;

    deck_env_t *tap_env = deck_env_new(&app->arena, b->captured_env);
    if (!tap_env) return DECK_RT_NO_MEMORY;
    deck_value_t *event_val = make_intent_event_value(vals, n_vals);
    if (event_val) {
        deck_env_bind(&app->arena, tap_env, "event", event_val);
        deck_release(event_val);
    }
    app->ctx.err = DECK_RT_OK;
    app->ctx.err_line = 0;
    app->ctx.err_col  = 0;
    app->ctx.err_msg[0] = '\0';
    app->ctx.depth = 0;
    app->ctx.module = app->ld.module;
    deck_value_t *r = deck_interp_run(&app->ctx, tap_env, b->action_ast);
    if (r) deck_release(r);
    deck_env_release(tap_env);
    if (app->ctx.err != DECK_RT_OK) {
        ESP_LOGE(TAG, "app_intent(%u): %s",
                 (unsigned)intent_id, app->ctx.err_msg);
    }
    return app->ctx.err;
}

deck_err_t deck_runtime_app_intent(deck_runtime_app_t *app, uint32_t intent_id)
{
    return deck_runtime_app_intent_v(app, intent_id, NULL, 0);
}

/* ----- DL2 F28.4: @migration runner ------------------------------------
 *
 * Called at app_load after fn pre-binding and before @on launch. Reads
 * the stored schema version for this app from NVS, runs every
 * `@migration from N:` body where `N >= stored` in ascending order of
 * N, then writes back the new high-water mark.
 *
 * NVS layout:
 *   namespace = "deck.mig" (8 chars, < 15 char limit)
 *   key       = "v_" + 8-hex FNV32(app_id) → 10 chars (avoids truncating
 *               app_ids longer than the 15-char key limit; collisions
 *               are infinitesimal across the ≤ 8 apps the shell loads)
 *   value     = i64 version number
 */
static uint32_t migration_fnv32(const char *s)
{
    uint32_t h = 0x811c9dc5u;
    if (!s) return h ? h : 1;
    for (; *s; s++) {
        h ^= (uint8_t)*s;
        h *= 0x01000193u;
    }
    return h ? h : 1;
}

static void migration_key_for(const char *app_id, char out[16])
{
    uint32_t h = migration_fnv32(app_id);
    snprintf(out, 16, "v_%08x", (unsigned)h);
}

static void run_migrations(struct deck_runtime_app *app)
{
    if (!app->ld.module || app->ld.module->kind != AST_MODULE) return;
    const ast_node_t *mig = NULL;
    for (uint32_t i = 0; i < app->ld.module->as.module.items.len; i++) {
        const ast_node_t *it = app->ld.module->as.module.items.items[i];
        if (it && it->kind == AST_MIGRATION) { mig = it; break; }
    }
    if (!mig || mig->as.migration.n_entries == 0) return;
    if (!app->ld.app_id || !*app->ld.app_id) {
        ESP_LOGW(TAG, "migration: app has no id — skipping");
        return;
    }

    char key[16];
    migration_key_for(app->ld.app_id, key);

    int64_t stored = 0;
    deck_sdi_err_t rc = deck_sdi_nvs_get_i64("deck.mig", key, &stored);
    if (rc != DECK_SDI_OK && rc != DECK_SDI_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "migration: nvs.get(deck.mig,%s): %s — assuming 0",
                 key, deck_sdi_strerror(rc));
        stored = 0;
    } else if (rc == DECK_SDI_ERR_NOT_FOUND) {
        stored = 0;
    }

    /* Sort indices ascending-by-version. Parser caps n_entries at 16
     * (DECK_MIGRATION_MAX), so insertion sort with a 16-slot stack array
     * is fine. */
    uint32_t order[16];
    const uint32_t n = mig->as.migration.n_entries;
    for (uint32_t i = 0; i < n; i++) order[i] = i;
    for (uint32_t a = 1; a < n; a++) {
        uint32_t v = order[a];
        int64_t  vk = mig->as.migration.from_versions[v];
        int j = (int)a - 1;
        while (j >= 0 && mig->as.migration.from_versions[order[j]] > vk) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }

    int64_t new_high = stored;
    bool any_ran = false;
    for (uint32_t i = 0; i < n; i++) {
        int64_t fv = mig->as.migration.from_versions[order[i]];
        if (fv < stored) continue;
        ESP_LOGI(TAG, "migration %s: from %lld", app->ld.app_id, (long long)fv);
        deck_value_t *r = deck_interp_run(&app->ctx, app->ctx.global,
                                          mig->as.migration.bodies[order[i]]);
        if (r) deck_release(r);
        if (app->ctx.err != DECK_RT_OK) {
            ESP_LOGE(TAG, "migration from %lld failed: %s — aborting",
                     (long long)fv, deck_err_name(app->ctx.err));
            return;
        }
        any_ran = true;
        if (fv + 1 > new_high) new_high = fv + 1;
    }

    if (any_ran && new_high != stored) {
        deck_sdi_err_t sr = deck_sdi_nvs_set_i64("deck.mig", key, new_high);
        if (sr != DECK_SDI_OK) {
            ESP_LOGW(TAG, "migration: nvs.set v=%lld failed: %s",
                     (long long)new_high, deck_sdi_strerror(sr));
        } else {
            ESP_LOGI(TAG, "migration %s: stored v=%lld",
                     app->ld.app_id, (long long)new_high);
        }
    }
}

deck_err_t deck_runtime_app_load(const char *src, uint32_t len,
                                 deck_runtime_app_t **out)
{
    if (out) *out = NULL;
    struct deck_runtime_app *app = app_slot_alloc();
    if (!app) {
        ESP_LOGE(TAG, "app_load: no free slot (max=%d)", DECK_RUNTIME_MAX_APPS);
        return DECK_RT_INTERNAL;
    }

    deck_arena_init(&app->arena, 0);
    deck_loader_init(&app->ld, &app->arena);

    deck_err_t rc = deck_loader_load(&app->ld, src, len);
    if (rc != DECK_LOAD_OK) {
        ESP_LOGE(TAG, "app_load: load failed @ stage %u: %s (%u:%u)",
                 app->ld.err_stage, app->ld.err_msg,
                 app->ld.err_line, app->ld.err_col);
        deck_arena_reset(&app->arena);
        app_slot_free(app);
        return rc;
    }

    deck_interp_init(&app->ctx, &app->arena);
    app->ctx.module = app->ld.module;

    /* Pre-bind top-level fn's so @on launch + machine + later dispatches
     * see them by name. Same pattern as run_on_launch. */
    if (app->ld.module && app->ld.module->kind == AST_MODULE) {
        for (uint32_t i = 0; i < app->ld.module->as.module.items.len; i++) {
            const ast_node_t *it = app->ld.module->as.module.items.items[i];
            if (it && it->kind == AST_FN_DEF) {
                deck_value_t *r = deck_interp_run(&app->ctx, app->ctx.global, it);
                if (r) deck_release(r);
                if (app->ctx.err != DECK_RT_OK) break;
            }
        }
    }

    /* DL2 F28.4 — run any applicable @migration bodies BEFORE @on launch
     * so the launch body sees the post-migration NVS / FS state. */
    if (app->ctx.err == DECK_RT_OK) {
        run_migrations(app);
    }

    /* Run @on launch synchronously. Errors abort load + tear down. */
    if (app->ctx.err == DECK_RT_OK) {
        const ast_node_t *on = find_on_launch(app->ld.module);
        if (on) {
            deck_value_t *r = deck_interp_run(&app->ctx, app->ctx.global, on->as.on.body);
            if (r) deck_release(r);
        }
    }

    /* Run @machine. Two modes (concept #44):
     *   - Sequential: no top-level transitions → run_machine auto-progresses
     *     until a terminal state (legacy DL1 / @flow behaviour).
     *   - Event-driven: top-level transitions present → run_machine enters
     *     the initial state and returns; subsequent progression is via
     *     machine.send(:event, payload) calls. */
    if (app->ctx.err == DECK_RT_OK) {
        const ast_node_t *machine = find_machine(app->ld.module);
        if (machine) {
            run_machine(machine, app->ld.module, &app->ctx);
            /* Capture the state the machine settled in, so machine.send can
             * find matching transitions by from_state. */
            if (machine->as.machine.initial_state)
                app->machine_state = machine->as.machine.initial_state;
            else if (machine->as.machine.states.len > 0)
                app->machine_state = machine->as.machine.states.items[0]->as.state.name;
        }
    }

    deck_err_t run_rc = app->ctx.err;
    if (run_rc != DECK_RT_OK) {
        ESP_LOGE(TAG, "app_load: runtime error @ %u:%u — %s: %s",
                 app->ctx.err_line, app->ctx.err_col,
                 deck_err_name(run_rc), app->ctx.err_msg);
        deck_env_release(app->ctx.global);
        deck_arena_reset(&app->arena);
        app_slot_free(app);
        return run_rc;
    }

    if (out) *out = app;
    return DECK_RT_OK;
}

deck_err_t deck_runtime_app_dispatch(deck_runtime_app_t *app,
                                     const char *event,
                                     deck_value_t *payload)
{
    if (!app || !app->in_use || !event) return DECK_RT_INTERNAL;
    /* launch/terminate are handled by load/unload; ignoring them keeps
     * callers from accidentally double-firing the lifecycle. */
    if (strcmp(event, "launch") == 0 || strcmp(event, "terminate") == 0) {
        return DECK_RT_OK;
    }
    const ast_node_t *on = find_on_event(app->ld.module, event);
    if (!on || !on->as.on.body) return DECK_RT_OK;  /* no handler, no-op */

    /* Reset any lingering error state from a prior dispatch so we don't
     * short-circuit here. The global env survives across dispatches, but
     * c->err is per-call.  */
    app->ctx.err = DECK_RT_OK;
    app->ctx.err_line = 0;
    app->ctx.err_col = 0;
    app->ctx.err_msg[0] = '\0';
    app->ctx.depth = 0;

    /* Concept #38 — parameter binding / filtering per spec §11.
     * Build a child env so bindings don't leak into the module's global env.
     * Walk the declared params[]; for each, look up the payload field and:
     *   - match_pattern() against it — success extends the env (for binders)
     *                                   or passes through (for value patterns),
     *                                   failure aborts the handler without firing. */
    deck_env_t *bind_env = app->ctx.global;
    bool skip_handler = false;
    if (on->as.on.n_params > 0) {
        bind_env = deck_env_new(&app->arena, app->ctx.global);
        if (!bind_env) return DECK_RT_NO_MEMORY;
        for (uint32_t i = 0; i < on->as.on.n_params; i++) {
            const ast_on_param_t *p = &on->as.on.params[i];
            if (!p->field || !p->pattern) continue;
            deck_value_t *field_val = NULL;
            if (payload && payload->type == DECK_T_MAP) {
                /* Try atom key first (records), then string key (JSON / raw maps). */
                deck_value_t *akey = deck_new_atom(p->field);
                if (akey) {
                    field_val = deck_map_get(payload, akey);
                    deck_release(akey);
                }
                if (!field_val) {
                    deck_value_t *skey = deck_new_str_cstr(p->field);
                    if (skey) {
                        field_val = deck_map_get(payload, skey);
                        deck_release(skey);
                    }
                }
            }
            /* A missing field is unit — binders will capture that; value
             * patterns won't match unit against a concrete literal. */
            if (!field_val) field_val = deck_unit();
            if (!match_pattern(&app->arena, bind_env, p->pattern, field_val)) {
                skip_handler = true;
                break;
            }
        }
        if (skip_handler) {
            deck_env_release(bind_env);
            return DECK_RT_OK;
        }
    }

    /* Make the raw event payload accessible as `event` for no-param handlers
     * that use `event.field` accessors (the other spec §11 style). */
    if (payload) {
        if (bind_env == app->ctx.global) {
            bind_env = deck_env_new(&app->arena, app->ctx.global);
            if (!bind_env) return DECK_RT_NO_MEMORY;
        }
        deck_env_bind(&app->arena, bind_env, "event", payload);
    }

    deck_value_t *r = deck_interp_run(&app->ctx, bind_env, on->as.on.body);
    if (r) deck_release(r);

    if (bind_env != app->ctx.global) deck_env_release(bind_env);

    if (app->ctx.err != DECK_RT_OK) {
        ESP_LOGE(TAG, "app_dispatch(%s): runtime error @ %u:%u — %s: %s",
                 event, app->ctx.err_line, app->ctx.err_col,
                 deck_err_name(app->ctx.err), app->ctx.err_msg);
    }
    return app->ctx.err;
}

void deck_runtime_app_unload(deck_runtime_app_t *app)
{
    if (!app || !app->in_use) return;

    /* Best-effort @on terminate. Errors are logged, not propagated — we
     * cannot abort cleanup. */
    const ast_node_t *on = find_on_event(app->ld.module, "terminate");
    if (on && on->as.on.body) {
        app->ctx.err = DECK_RT_OK;
        app->ctx.depth = 0;
        deck_value_t *r = deck_interp_run(&app->ctx, app->ctx.global, on->as.on.body);
        if (r) deck_release(r);
        if (app->ctx.err != DECK_RT_OK) {
            ESP_LOGW(TAG, "app_unload: @on terminate failed: %s",
                     deck_err_name(app->ctx.err));
        }
    }

    /* Concept #58 — release captured envs (and their retained values)
     * before the arena dies. */
    for (uint32_t j = 0; j < DECK_RUNTIME_MAX_INTENTS; j++) {
        if (app->intents[j].captured_env) {
            deck_env_release(app->intents[j].captured_env);
            app->intents[j].captured_env = NULL;
        }
    }
    deck_env_release(app->ctx.global);
    deck_arena_reset(&app->arena);
    app_slot_free(app);
}

const char *deck_runtime_app_id(const deck_runtime_app_t *app)
{
    return (app && app->in_use) ? app->ld.app_id : NULL;
}

const char *deck_runtime_app_name(const deck_runtime_app_t *app)
{
    return (app && app->in_use) ? app->ld.app_name : NULL;
}
