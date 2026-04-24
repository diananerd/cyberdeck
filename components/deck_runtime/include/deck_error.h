#pragma once

/* deck_error — canonical runtime error codes.
 *
 * Two families:
 *   RT_OK, RT_* — runtime/eval errors during execution
 *   LOAD_*     — structured load errors raised by the loader
 *
 * LoadErrorKind is fixed by LANG.md §11.3 at exactly nine variants:
 *   :lex :parse :type :unresolved :incompatible :exhaustive
 *   :permission :resource :internal
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* --- runtime (eval) --- */
    DECK_RT_OK = 0,
    DECK_RT_TYPE_MISMATCH,
    DECK_RT_OUT_OF_RANGE,
    DECK_RT_DIVIDE_BY_ZERO,
    DECK_RT_INDEX_OUT_OF_BOUNDS,
    DECK_RT_KEY_NOT_FOUND,
    DECK_RT_PATTERN_FAILED,        /* no match arm fired and no wildcard */
    DECK_RT_STACK_OVERFLOW,
    DECK_RT_NO_MEMORY,
    DECK_RT_ASSERTION_FAILED,
    DECK_RT_ABORTED,               /* user called abort/panic */
    DECK_RT_INTERNAL,              /* defect in runtime */

    /* --- load-time — LANG §11.3 canonical nine --- */
    DECK_LOAD_OK = 100,
    DECK_LOAD_LEX,
    DECK_LOAD_PARSE,
    DECK_LOAD_TYPE,
    DECK_LOAD_UNRESOLVED,
    DECK_LOAD_INCOMPATIBLE,
    DECK_LOAD_EXHAUSTIVE,
    DECK_LOAD_PERMISSION,
    DECK_LOAD_RESOURCE,
    DECK_LOAD_INTERNAL,
} deck_err_t;

/* Canonical atom-style name ("type_mismatch", "incompatible"). */
const char *deck_err_name(deck_err_t err);

/* One-line human description. */
const char *deck_err_message(deck_err_t err);

/* true if the error is load-time (DECK_LOAD_*), false if runtime (DECK_RT_*). */
bool deck_err_is_load(deck_err_t err);

#ifdef __cplusplus
}
#endif
