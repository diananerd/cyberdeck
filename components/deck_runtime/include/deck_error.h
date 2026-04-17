#pragma once

/* deck_error — canonical runtime error codes.
 *
 * Two families:
 *   RT_OK, RT_* — runtime/eval errors during execution
 *   LOAD_*     — structured load errors raised by loader stages 0-9
 *
 * Catalog from deck-lang/15-deck-versioning.md §9.3 + DL-level errors from
 * deck-lang/16-deck-levels.md §10.
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

    /* --- load-time --- */
    DECK_LOAD_OK = 100,
    DECK_LOAD_LEX_ERROR,
    DECK_LOAD_PARSE_ERROR,
    DECK_LOAD_TYPE_ERROR,
    DECK_LOAD_UNRESOLVED_SYMBOL,
    DECK_LOAD_CAPABILITY_MISSING,
    DECK_LOAD_CAPABILITY_INCOMPAT,
    DECK_LOAD_PATTERN_NOT_EXHAUSTIVE,
    DECK_LOAD_INCOMPATIBLE_EDITION,
    DECK_LOAD_INCOMPATIBLE_SURFACE,
    DECK_LOAD_INCOMPATIBLE_RUNTIME,
    DECK_LOAD_LEVEL_BELOW_REQUIRED,
    DECK_LOAD_LEVEL_UNKNOWN,
    DECK_LOAD_LEVEL_INCONSISTENT,
    DECK_LOAD_PERMISSION_DENIED,
    DECK_LOAD_SIGNATURE_INVALID,
    DECK_LOAD_UNKNOWN_SIGNER,
    DECK_LOAD_BUNDLE_CORRUPT,
    DECK_LOAD_MIGRATION_FAILED,
    DECK_LOAD_NO_MEMORY,
    DECK_LOAD_INTERNAL,
} deck_err_t;

/* Canonical atom-style name as in the spec ("type_mismatch", "missing_capability"). */
const char *deck_err_name(deck_err_t err);

/* One-line human description. */
const char *deck_err_message(deck_err_t err);

/* true if the error is load-time (DECK_LOAD_*), false if runtime (DECK_RT_*). */
bool deck_err_is_load(deck_err_t err);

#ifdef __cplusplus
}
#endif
