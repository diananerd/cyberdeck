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
} deck_interp_ctx_t;

/* Initialize interpreter context. global env starts empty. */
void deck_interp_init(deck_interp_ctx_t *c, deck_arena_t *arena);

/* Run an AST expression in the given environment. Returns a retained
 * value on success, NULL on error (see c->err). Caller deck_releases. */
deck_value_t *deck_interp_run(deck_interp_ctx_t *c, deck_env_t *env,
                              const ast_node_t *n);

/* Load + run @on launch body. Used by F5 tests + F8 shell. */
deck_err_t deck_runtime_run_on_launch(const char *src, uint32_t len);

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

/* Interpreter selftest. */
deck_err_t deck_interp_run_selftest(void);

#ifdef __cplusplus
}
#endif
