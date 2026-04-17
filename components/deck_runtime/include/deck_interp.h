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

#define DECK_INTERP_STACK_MAX  512
#define DECK_INTERP_ERR_MSG    160

typedef struct deck_env deck_env_t;

typedef struct {
    deck_arena_t *arena;
    deck_env_t   *global;
    uint32_t      depth;
    deck_err_t    err;
    uint32_t      err_line;
    uint32_t      err_col;
    char          err_msg[DECK_INTERP_ERR_MSG];
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

/* Interpreter selftest. */
deck_err_t deck_interp_run_selftest(void);

#ifdef __cplusplus
}
#endif
