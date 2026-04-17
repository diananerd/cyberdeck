#pragma once

/* deck_runtime — interpreter public entrypoint.
 *
 * Umbrella header pulling the public surfaces of the runtime:
 *   deck_error.h  — error codes + strerror
 *   deck_types.h  — value representation
 *   deck_alloc.h  — refcount allocator + constructors
 *   deck_intern.h — string interning  (F2.3)
 *   deck_lexer.h  — lexer             (F2.4)
 *   deck_parser.h — parser            (F3)
 *   deck_loader.h — loader            (F4)
 *   deck_eval.h   — evaluator         (F5)
 *   deck_dispatcher.h — effect dispatcher (F6)
 *   deck_machine.h — @machine runtime (F7)
 *
 * Consumers typically include only the specific header they need.
 */

#include "deck_error.h"
#include "deck_types.h"
#include "deck_alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the runtime. Wraps deck_alloc_init + future subsystem inits. */
deck_err_t deck_runtime_init(size_t heap_limit_bytes);

/* Runtime-level selftest — exercises allocator round-trip + invariants. */
deck_err_t deck_runtime_selftest(void);

/* Lexer selftest — 30+ token sequence cases. */
deck_err_t deck_lexer_run_selftest(void);

#ifdef __cplusplus
}
#endif
