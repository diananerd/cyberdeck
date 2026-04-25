#pragma once

/* deck_interp — tree-walking interpreter for the DL1 AST.
 *
 * Values are heap-allocated via deck_alloc (refcounted). The environment
 * chain is arena-allocated; bindings store retained values and drop
 * them on arena reset.
 *
 * Call dispatch: if the callee is a DOT chain rooted at a known DL1
 * capability name (log, math, text, time, system, nvs, fs, os), the
 * chain is flattened to "cap.method" and routed to a builtin.
 */

#include "deck_types.h"
#include "deck_error.h"
#include "deck_ast.h"
#include "deck_arena.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DECK_INTERP_STACK_MAX     512
#define DECK_INTERP_ERR_MSG       160
#define DECK_INTERP_MAX_TC_ARGS   16

typedef struct deck_env deck_env_t;

/* DL2 F21.3 — pending tail call.
 *
 * When a fn call is in tail position (relative to the enclosing fn), the
 * interpreter does not invoke the callee recursively on the C stack —
 * instead it stashes the resolved fn value plus evaluated args here, and
 * returns a placeholder. The enclosing invoke_user_fn loop picks the
 * pending entry up, rebinds args (re-using the env for self-recursion,
 * swapping it for mutual-recursion), and re-runs the body. */
typedef struct {
    bool          active;
    deck_value_t *fn;                                  /* retained */
    deck_value_t *args[DECK_INTERP_MAX_TC_ARGS];       /* retained */
    uint32_t      argc;
    uint32_t      line;
    uint32_t      col;
} deck_pending_tc_t;

typedef struct {
    deck_arena_t      *arena;
    deck_env_t        *global;
    uint32_t           depth;
    deck_err_t         err;
    uint32_t           err_line;
    uint32_t           err_col;
    char               err_msg[DECK_INTERP_ERR_MSG];
    bool               tail_pos;     /* DL2 F21.3: current AST node is in tail position */
    deck_pending_tc_t  pending_tc;
    /* DL2 F28.5: the currently-executing module, set by run_on_launch
     * and deck_runtime_app_* so builtins (asset.path) can look up
     * top-level decls (@assets) at call time. NULL for expressions
     * evaluated outside a module (e.g. interp_test scratch runs). */
    const ast_node_t  *module;
    /* LANG §11.1 — postfix `?` propagation state. When AST_TRY sees a
     * (:err, e) tuple, it retains the entire tuple here and flips
     * try_unwinding. The interpreter bails to the enclosing fn body,
     * which returns this value and clears the flag. */
    struct deck_value *try_propagated;   /* retained when set */
    bool               try_unwinding;
    /* Conformance-mode machine driver — when no app handle exists
     * (deck_runtime_run_on_launch path), Machine.send records the
     * pending event here and run_machine drains it to advance the
     * state machine in-process. Both fields are interned strings. */
    const char        *pending_machine_event;
    const char        *standalone_machine_state;
} deck_interp_ctx_t;

/* Initialize interpreter context. global env starts empty. */
void deck_interp_init(deck_interp_ctx_t *c, deck_arena_t *arena);

/* Run an AST expression in the given environment. Returns a retained
 * value on success, NULL on error (see c->err). Caller deck_releases. */
deck_value_t *deck_interp_run(deck_interp_ctx_t *c, deck_env_t *env,
                              const ast_node_t *n);

/* Load + run @on launch body. Used by F5 tests + F8 shell. */
deck_err_t deck_runtime_run_on_launch(const char *src, uint32_t len);

/* ----- DL2 F28 — persistent app handles --------------------------------
 *
 * `deck_runtime_run_on_launch` is a one-shot: it loads, runs @on launch,
 * runs the machine, tears everything down. That doesn't work for the
 * DL2 app-model — @on resume/pause/suspend/terminate need the module
 * AST, global env, and top-level fn bindings to outlive the launch call
 * so subsequent shell events can re-enter the same module.
 *
 * deck_runtime_app_t holds a dedicated arena + loader + interp context
 * for one loaded .deck. Lifecycle:
 *   load()      — parse + validate + run @on launch + run @machine (initial
 *                 transitions execute synchronously, same as run_on_launch)
 *   dispatch()  — find @on <event> and run its body. Null / no-match is a
 *                 silent no-op. Does NOT re-run the machine.
 *   unload()    — fire @on terminate if present, release global env, free
 *                 the arena.
 *
 * Reserved event names: "launch" and "terminate" are dispatched implicitly
 * by load/unload; calling dispatch() with them is a no-op (to prevent
 * double-dispatch). Other lifecycle names ("resume", "pause", "suspend",
 * "low_memory", "network_change") are passed through as-is.
 *
 * Apps are kept internally in a fixed array; max DECK_RUNTIME_MAX_APPS.
 */
#define DECK_RUNTIME_MAX_APPS  8

typedef struct deck_runtime_app deck_runtime_app_t;

/* Load a source buffer as a persistent app. On success *out is set to a
 * non-NULL handle and return is DECK_RT_OK. On failure *out is NULL and
 * the returned error identifies the stage (load / launch / machine). */
deck_err_t deck_runtime_app_load(const char *src, uint32_t len,
                                 deck_runtime_app_t **out);

/* Fire @on <event> on the app. Missing handler is a silent no-op, returns
 * DECK_RT_OK. Event body errors bubble up.
 *
 * `payload` is an optional map of event fields. When the handler declares
 * parameter clauses (spec §11 `@on os.wifi_changed (ssid: s, connected: c)`),
 * each parameter is matched against the corresponding payload field:
 *   - binder pattern (IDENT) → bind payload field into the handler's env
 *   - value pattern (literal/atom) → filter: handler only fires when payload
 *     field equals the declared value
 *   - wildcard `_` → accept any, no binding
 * Pass NULL for lifecycle events (launch/resume/pause/terminate) that carry
 * no payload; the handler's body runs with its implicit `event` identifier
 * bound to unit. */
deck_err_t deck_runtime_app_dispatch(deck_runtime_app_t *app,
                                     const char *event,
                                     deck_value_t *payload);

/* Concept #47 + #58 — bridge intent dispatch.
 *
 * When the bridge emits a tap/interaction with an intent_id (the value the
 * runtime stamped onto a DVC_TRIGGER / DVC_NAVIGATE / input-intent node at
 * render time), the shell calls this entry point. The runtime evaluates the
 * captured action AST in the captured env — which works for `Machine.send`
 * as well as arbitrary action calls (`apps.launch`, etc.). Unknown intent_id
 * is a silent no-op (returns DECK_RT_OK). */
deck_err_t deck_runtime_app_intent(deck_runtime_app_t *app, uint32_t intent_id);

/* Concept #59 + #60 — bridge intent dispatch with payload.
 *
 * `vals` carries either the scalar that the widget produced (single entry,
 * `key == NULL`) or a form-submit's aggregated field map (multiple entries,
 * every `key != NULL`). The runtime binds `event.value` (scalar form) or
 * `event.values` (map form) into a child of the captured env before
 * evaluating the action AST, so authors can write:
 *     toggle :lights state: s on -> Machine.send(:toggled, event.value)
 *     form on submit -> Machine.send(:login, event.values)
 *
 * `n_vals == 0` and `vals == NULL` is equivalent to the zero-arg form. */
typedef enum {
    DECK_INTENT_VAL_NONE = 0,
    DECK_INTENT_VAL_BOOL,
    DECK_INTENT_VAL_I64,
    DECK_INTENT_VAL_F64,
    DECK_INTENT_VAL_STR,    /* NUL-terminated */
    DECK_INTENT_VAL_ATOM,
} deck_intent_val_kind_t;

typedef struct {
    const char            *key;     /* NULL → single scalar payload */
    deck_intent_val_kind_t kind;
    bool                   b;
    int64_t                i;
    double                 f;
    const char            *s;       /* for STR / ATOM */
} deck_intent_val_t;

deck_err_t deck_runtime_app_intent_v(deck_runtime_app_t *app,
                                      uint32_t intent_id,
                                      const deck_intent_val_t *vals,
                                      uint32_t n_vals);

/* LANG §14.8 — @on back routing.
 *
 * Fires @on back on the app (if present) and interprets the returned
 * BackResult. Unlike app_dispatch, the body's return value is not
 * discarded — it steers the gesture:
 *
 *   :handled             → DECK_BACK_HANDLED    (shell consumes)
 *   :unhandled           → DECK_BACK_UNHANDLED  (shell delegates to OS)
 *   :confirm (prompt:,   → DECK_BACK_CONFIRMED  (shell shows the dialog
 *             confirm:,     via the bridge; the user's choice asynchronously
 *             cancel:)      maps to the :handled/:unhandled atom carried
 *                           in the chosen (str, atom) pair)
 *
 * If the handler is missing, returns DECK_BACK_UNHANDLED. If the body
 * panics, returns DECK_BACK_UNHANDLED (§14.8 panic rule). */
typedef enum {
    DECK_BACK_HANDLED = 0,
    DECK_BACK_UNHANDLED,
    DECK_BACK_CONFIRMED,
    DECK_BACK_ERROR,
} deck_back_result_t;

deck_back_result_t deck_runtime_app_back(deck_runtime_app_t *app);

/* When @on back returns :confirm the runtime shows the dialog and
 * returns DECK_BACK_CONFIRMED synchronously. The user's eventual pick
 * fires this resolver with the final outcome (HANDLED or UNHANDLED) so
 * the shell can pop or not. The resolver is a singleton — the shell
 * registers once at boot. NULL clears it. */
typedef void (*deck_runtime_back_resolved_cb_t)(deck_back_result_t outcome);
void deck_runtime_set_back_resolved_handler(deck_runtime_back_resolved_cb_t cb);

/* J12 — @service provider runtime.
 *
 * Apps that declare `@service "id"` expose `on :method (params) → body`
 * entries. Other apps in the same runtime can invoke those methods
 * via deck_runtime_app_invoke_service. The caller passes the app
 * handle that *provides* the service (not the consumer); the runtime
 * looks up the named method, binds the supplied payload, runs the
 * body in the provider's env, and returns its result.
 *
 * Returns NULL on missing service / missing method / runtime error;
 * c->err on the provider's ctx carries the actual diagnostic.
 *
 * The shell can list providers by walking the loaded apps and reading
 * deck_runtime_app_service_id (NULL if the app declares no service). */
const char *deck_runtime_app_service_id(const deck_runtime_app_t *app);
deck_value_t *deck_runtime_app_invoke_service(deck_runtime_app_t *app,
                                                const char *method,
                                                deck_value_t *payload);

/* Fire @on terminate if present, then free everything. Handle is invalid
 * after this call. NULL-safe. */
void deck_runtime_app_unload(deck_runtime_app_t *app);

/* Read app metadata extracted by the loader. NULL returns for missing. */
const char *deck_runtime_app_id(const deck_runtime_app_t *app);
const char *deck_runtime_app_name(const deck_runtime_app_t *app);

/* Env helpers. */
deck_env_t *deck_env_new(deck_arena_t *a, deck_env_t *parent);
bool        deck_env_bind(deck_arena_t *a, deck_env_t *e,
                          const char *name, deck_value_t *val);
deck_value_t *deck_env_lookup(deck_env_t *e, const char *name);

/* Refcount semantics for envs.
 *
 * deck_env_new returns refcount=1 and retains the parent. retain++ the
 * count; release-- it; reaching zero clears bindings (releasing every
 * retained value) and releases the parent.
 *
 * Why refcount and not just "clear at scope exit": F21.2 closures may
 * outlive the scope that created them. A `fn (n) = fn (x) = x + n`
 * returns an inner fn whose closure points back to the outer call_env.
 * Refcount keeps that env alive until the closure itself is released.
 * The cycle (env → fn-binding → fn → closure-env) is broken by a
 * `tearing_down` flag that ignores re-entry during teardown. */
deck_env_t *deck_env_retain(deck_env_t *e);
void        deck_env_release(deck_env_t *e);

/* Force-teardown an env regardless of refcount. Used at module end to
 * break the fn-closure cycle where every top-level fn's closure retains
 * the global env, so a plain release never reaches refcount 0. See
 * concept #73. */
void        deck_env_force_release(deck_env_t *e);

/* Interpreter selftest. */
deck_err_t deck_interp_run_selftest(void);

/* DL3 — tick-scheduler smoke test. Builds an in-process stream of 10
 * ticks at 100ms, applies throttle(300ms) and debounce(50ms) and
 * verifies the output list lengths match real interval math. Logs a
 * one-line summary and returns DECK_RT_OK on pass. Used by the shell
 * as a hardware canary for the K1–K4 stream surface. */
deck_err_t deck_runtime_dl3_tick_canary(void);

#ifdef __cplusplus
}
#endif
