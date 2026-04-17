#include "deck_types.h"

const char *deck_type_name(deck_type_t t)
{
    switch (t) {
        case DECK_T_UNIT:     return "unit";
        case DECK_T_BOOL:     return "bool";
        case DECK_T_INT:      return "int";
        case DECK_T_FLOAT:    return "float";
        case DECK_T_ATOM:     return "atom";
        case DECK_T_STR:      return "str";
        case DECK_T_BYTES:    return "bytes";
        case DECK_T_LIST:     return "list";
        case DECK_T_MAP:      return "map";
        case DECK_T_TUPLE:    return "tuple";
        case DECK_T_OPTIONAL: return "optional";
        default:              return "unknown";
    }
}
