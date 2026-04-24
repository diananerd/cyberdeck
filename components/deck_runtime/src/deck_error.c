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

        case DECK_LOAD_OK:                     return "ok";
        case DECK_LOAD_LEX:                    return "lex";
        case DECK_LOAD_PARSE:                  return "parse";
        case DECK_LOAD_TYPE:                   return "type";
        case DECK_LOAD_UNRESOLVED:             return "unresolved";
        case DECK_LOAD_INCOMPATIBLE:           return "incompatible";
        case DECK_LOAD_EXHAUSTIVE:             return "exhaustive";
        case DECK_LOAD_PERMISSION:             return "permission";
        case DECK_LOAD_RESOURCE:               return "resource";
        case DECK_LOAD_INTERNAL:               return "internal";
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
        case DECK_LOAD_LEX:                    return "lexical error";
        case DECK_LOAD_PARSE:                  return "parse error";
        case DECK_LOAD_TYPE:                   return "type check failed";
        case DECK_LOAD_UNRESOLVED:             return "symbol not found in scope";
        case DECK_LOAD_INCOMPATIBLE:           return "app or capability is incompatible with this runtime";
        case DECK_LOAD_EXHAUSTIVE:             return "match is not exhaustive";
        case DECK_LOAD_PERMISSION:             return "permission denied or signature invalid";
        case DECK_LOAD_RESOURCE:               return "missing or corrupt bundle resource";
        case DECK_LOAD_INTERNAL:               return "load internal error";
        default:                               return "unknown error";
    }
}

bool deck_err_is_load(deck_err_t err)
{
    return err >= DECK_LOAD_OK;
}
