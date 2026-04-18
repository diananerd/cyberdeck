#pragma once

/* deck_alloc — refcounted allocator for deck_value_t.
 *
 * Backed by heap_caps_malloc(MALLOC_CAP_INTERNAL) to keep DL1 values out
 * of PSRAM (the tiny-target profile has no PSRAM). Hard limit enforced
 * on every allocation — panics the evaluator via the registered panic
 * callback when exceeded.
 *
 * Immortal singletons (unit, true, false) have refcount 0 and never
 * go through retain/release bookkeeping.
 *
 * Composites (list, tuple, optional, map) release their children
 * recursively when their own refcount drops to zero.
 */

#include "deck_types.h"
#include "deck_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*deck_alloc_panic_cb_t)(deck_err_t code, const char *message);

/* Initialize the allocator. Idempotent. limit_bytes=0 means unlimited. */
void deck_alloc_init(size_t limit_bytes, deck_alloc_panic_cb_t panic_cb);

/* Current bytes held by live deck_value_t objects. */
size_t deck_alloc_used(void);

/* High-water mark since init. */
size_t deck_alloc_peak(void);

/* Hard limit (0 = unlimited). */
size_t deck_alloc_limit(void);

/* Replace the hard limit without resetting peak/used. Used by F14.4
 * heap-pressure stress to force DECK_RT_NO_MEMORY in a controlled way
 * then restore the previous value. */
void deck_alloc_set_limit(size_t limit_bytes);

/* Count of live deck_value_t (excluding immortals). */
size_t deck_alloc_live_values(void);

/* --- Immortal singletons (refcount=0, retain/release are no-ops). --- */
deck_value_t *deck_unit(void);
deck_value_t *deck_true(void);
deck_value_t *deck_false(void);

/* --- Primitive constructors --- */
deck_value_t *deck_new_bool(bool v);              /* returns immortal */
deck_value_t *deck_new_int(int64_t v);
deck_value_t *deck_new_float(double v);
deck_value_t *deck_new_bytes(const uint8_t *buf, uint32_t len);  /* copies */

/* Atoms + strings route through the intern table — payload is a stable
 * pointer into intern storage, never owned by this value. Release does
 * NOT free the intern entry. */
deck_value_t *deck_new_atom(const char *name);
deck_value_t *deck_new_str(const char *s, uint32_t len);
deck_value_t *deck_new_str_cstr(const char *s);

/* --- Composite constructors --- */

/* Empty list with initial capacity slots. */
deck_value_t *deck_new_list(uint32_t cap);

/* Append — retains item. Grows storage if cap exceeded. */
deck_err_t    deck_list_push(deck_value_t *list, deck_value_t *item);

/* Fixed-arity tuple. items is copied (values are retained). Arity must be ≥ 1. */
deck_value_t *deck_new_tuple(deck_value_t **items, uint32_t arity);

/* Optional — none() and some(v). some retains v. */
deck_value_t *deck_new_none(void);
deck_value_t *deck_new_some(deck_value_t *inner);

/* DL2 F21.6 — map. Linear-scan implementation (small maps). put inserts
 * or updates by key equality (interned ptr for str/atom, value for int);
 * the value is retained. get returns the retained value or NULL. */
deck_value_t *deck_new_map(uint32_t initial_cap);
deck_err_t    deck_map_put(deck_value_t *m, deck_value_t *key, deck_value_t *val);
deck_value_t *deck_map_get(deck_value_t *m, deck_value_t *key);    /* borrowed */

/* DL2 F21.1: function value. Callee-owned references into the runtime
 * arena (params/body/closure/name); release does NOT free those. */
deck_value_t *deck_new_fn(const char *name,
                          const char **params,
                          uint32_t n_params,
                          const struct ast_node *body,
                          struct deck_env *closure);

/* --- Refcount primitives --- */

/* Increment refcount. No-op for immortals. Returns v for chaining. */
deck_value_t *deck_retain(deck_value_t *v);

/* Decrement refcount. When it reaches zero, releases children (if
 * composite) and frees the struct + payload. */
void deck_release(deck_value_t *v);

#ifdef __cplusplus
}
#endif
