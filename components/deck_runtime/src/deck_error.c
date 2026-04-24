#include "deck_error.h"

const char *deck_err_name(deck_err_t err)
{
    switch (err) {
        case DECK_RT_OK:                       return "ok";
        case DECK_RT_TYPE_MISMATCH:            return "type_mismatch";
        case DECK_RT_OUT_OF_RANGE:             return "out_of_range";
        case DECK_RT_DIVIDE_BY_ZERO:           return "divide_by_zero";
        case DECK_RT_INDEX_OUT_OF_BOUNDS:      return "index_out_of_bounds";
        case DECK_RT_KEY_NOT_FOUND:            return "key_not_found";
        case DECK_RT_PATTERN_FAILED:           return "pattern_failed";
        case DECK_RT_STACK_OVERFLOW:           return "stack_overflow";
        case DECK_RT_NO_MEMORY:                return "no_memory";
        case DECK_RT_ASSERTION_FAILED:         return "assertion_failed";
        case DECK_RT_ABORTED:                  return "aborted";
        case DECK_RT_INTERNAL:                 return "internal";

        case DECK_LOAD_OK:                     return "load_ok";
        case DECK_LOAD_LEX_ERROR:              return "lex_error";
        case DECK_LOAD_PARSE_ERROR:            return "parse_error";
        case DECK_LOAD_TYPE_ERROR:             return "type_error";
        case DECK_LOAD_UNRESOLVED_SYMBOL:      return "unresolved_symbol";
        case DECK_LOAD_CAPABILITY_MISSING:     return "missing_capability";
        case DECK_LOAD_CAPABILITY_INCOMPAT:    return "incompatible_capability";
        case DECK_LOAD_PATTERN_NOT_EXHAUSTIVE: return "pattern_not_exhaustive";
        case DECK_LOAD_INCOMPATIBLE_EDITION:   return "incompatible_edition";
        case DECK_LOAD_INCOMPATIBLE_SURFACE:   return "incompatible_surface";
        case DECK_LOAD_INCOMPATIBLE_RUNTIME:   return "incompatible_runtime";
        case DECK_LOAD_LEVEL_BELOW_REQUIRED:   return "level_below_required";
        case DECK_LOAD_LEVEL_UNKNOWN:          return "level_unknown";
        case DECK_LOAD_LEVEL_INCONSISTENT:     return "level_inconsistent";
        case DECK_LOAD_PERMISSION_DENIED:      return "permission_denied";
        case DECK_LOAD_SIGNATURE_INVALID:      return "signature_invalid";
        case DECK_LOAD_UNKNOWN_SIGNER:         return "unknown_signer";
        case DECK_LOAD_BUNDLE_CORRUPT:         return "bundle_corrupt";
        case DECK_LOAD_MIGRATION_FAILED:       return "migration_failed";
        case DECK_LOAD_NO_MEMORY:              return "load_no_memory";
        case DECK_LOAD_INTERNAL:               return "load_internal";
        default:                               return "unknown";
    }
}

const char *deck_err_message(deck_err_t err)
{
    switch (err) {
        case DECK_RT_OK:                       return "no error";
        case DECK_RT_TYPE_MISMATCH:            return "operation applied to the wrong type";
        case DECK_RT_OUT_OF_RANGE:             return "numeric value out of allowed range";
        case DECK_RT_DIVIDE_BY_ZERO:           return "division by zero";
        case DECK_RT_INDEX_OUT_OF_BOUNDS:      return "list/tuple index out of bounds";
        case DECK_RT_KEY_NOT_FOUND:            return "map key not found";
        case DECK_RT_PATTERN_FAILED:           return "no match arm matched and no wildcard present";
        case DECK_RT_STACK_OVERFLOW:           return "evaluator stack depth exceeded";
        case DECK_RT_NO_MEMORY:                return "heap exhausted during evaluation";
        case DECK_RT_ASSERTION_FAILED:         return "explicit assertion failed";
        case DECK_RT_ABORTED:                  return "app requested abort";
        case DECK_RT_INTERNAL:                 return "runtime internal error";

        case DECK_LOAD_OK:                     return "load completed";
        case DECK_LOAD_LEX_ERROR:              return "lexical error";
        case DECK_LOAD_PARSE_ERROR:            return "parse error";
        case DECK_LOAD_TYPE_ERROR:             return "type check failed";
        case DECK_LOAD_UNRESOLVED_SYMBOL:      return "symbol not found in scope";
        case DECK_LOAD_CAPABILITY_MISSING:     return "required capability has no driver";
        case DECK_LOAD_CAPABILITY_INCOMPAT:    return "capability version does not satisfy app";
        case DECK_LOAD_PATTERN_NOT_EXHAUSTIVE: return "match is not exhaustive";
        case DECK_LOAD_INCOMPATIBLE_EDITION:   return "app edition not supported by runtime";
        case DECK_LOAD_INCOMPATIBLE_SURFACE:   return "app surface API level not satisfied";
        case DECK_LOAD_INCOMPATIBLE_RUNTIME:   return "app runtime version not satisfied";
        case DECK_LOAD_LEVEL_BELOW_REQUIRED:   return "app requires a higher DL level than this runtime provides";
        case DECK_LOAD_LEVEL_UNKNOWN:          return "app declares an unknown DL level";
        case DECK_LOAD_LEVEL_INCONSISTENT:     return "app uses capabilities above its declared DL level";
        case DECK_LOAD_PERMISSION_DENIED:      return "permission denied by user or policy";
        case DECK_LOAD_SIGNATURE_INVALID:      return "bundle signature does not validate";
        case DECK_LOAD_UNKNOWN_SIGNER:         return "bundle signer not recognized";
        case DECK_LOAD_BUNDLE_CORRUPT:         return "bundle hash mismatch";
        case DECK_LOAD_MIGRATION_FAILED:       return "@migrate step failed";
        case DECK_LOAD_NO_MEMORY:              return "insufficient heap to load app";
        case DECK_LOAD_INTERNAL:               return "load internal error";
        default:                               return "unknown error";
    }
}

bool deck_err_is_load(deck_err_t err)
{
    return err >= DECK_LOAD_OK;
}
