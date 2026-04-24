#pragma once

/* deck_types — runtime value representation (DL1 subset).
 *
 * All values are heap-allocated, refcounted, immutable. Mutation happens
 * by creating a new value; the old one drops on the next release.
 *
 * DL1 types mapped from deck-lang/01-deck-lang.md §3:
 *   unit, bool, int (i64), float (f64), str (interned), byte (buffer),
 *   atom (interned), list T, map K V, tuple, Optional T
 *
 * Creation/retain/release live in deck_alloc.h (F2.2).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DECK_T_UNIT = 0,
    DECK_T_BOOL,
    DECK_T_INT,
    DECK_T_FLOAT,
    DECK_T_ATOM,       /* interned — payload is a const char * */
    DECK_T_STR,        /* interned — payload is {ptr,len} */
    DECK_T_BYTES,      /* mutable-length immutable-content byte buffer */
    DECK_T_LIST,       /* ordered, dynamic length */
    DECK_T_MAP,        /* hash map of value → value */
    DECK_T_TUPLE,      /* fixed arity */
    DECK_T_OPTIONAL,   /* some(inner) | none (inner NULL) */
    DECK_T_FN,         /* DL2 F21.1 — user-defined function value */
    DECK_T_STREAM,     /* BUILTINS §12 — reactive pipeline value; v1 is a
                          cold (list-backed) representation. Time-based
                          operators (throttle/debounce/delay/merge/…)
                          still require the DL3 tick scheduler. */
} deck_type_t;

const char *deck_type_name(deck_type_t t);

typedef struct deck_value deck_value_t;

typedef struct {
    const char *ptr;   /* interned — owned by the intern table */
    uint32_t    len;
} deck_strview_t;

typedef struct {
    uint8_t *buf;
    uint32_t len;
} deck_bytes_t;

typedef struct {
    deck_value_t **items;
    uint32_t       len;
    uint32_t       cap;
} deck_list_t;

typedef struct deck_map_entry {
    deck_value_t *key;
    deck_value_t *val;
    uint32_t      hash;
    bool          used;
} deck_map_entry_t;

typedef struct {
    deck_map_entry_t *entries;
    uint32_t          len;
    uint32_t          cap;    /* power of two */
} deck_map_t;

typedef struct {
    deck_value_t **items;
    uint32_t       arity;
} deck_tuple_t;

typedef struct {
    deck_value_t *inner;   /* NULL = none */
} deck_opt_t;

/* Stream value — v1 cold representation: wraps a retained list whose
 * items have already been pulled from the source. Every `stream.*`
 * combinator walks the full list eagerly and returns a new stream.
 * The DL3 tick-scheduler (stream.throttle/debounce/delay/merge/combine/
 * buffer/window) still panics :bug — those require per-stream state
 * and timer ticks that are not part of the v1 runtime. */
typedef struct {
    deck_value_t *list;       /* retained — items already emitted */
    bool          terminated; /* cold streams are always terminated */
} deck_stream_t;

/* DL2 F21.1: function value. All pointers point into the runtime arena
 * — releasing the value frees only the deck_value_t struct, not the AST
 * or environment (which live for the arena's lifetime). */
struct ast_node;
struct deck_env;
typedef struct {
    const char            *name;       /* interned, may be NULL for lambdas */
    const char           **params;
    uint32_t               n_params;
    const struct ast_node *body;
    struct deck_env       *closure;    /* lexical scope at definition */
} deck_fn_t;

struct deck_value {
    deck_type_t type;
    uint32_t    refcount;   /* 0 = immortal (unit, true, false, common atoms) */
    union {
        bool              b;
        int64_t           i;
        double            f;
        const char       *atom;      /* interned, NUL-terminated */
        deck_strview_t    s;
        deck_bytes_t      bytes;
        deck_list_t       list;
        deck_map_t        map;
        deck_tuple_t      tuple;
        deck_opt_t        opt;
        deck_fn_t         fn;
        deck_stream_t     stream;
    } as;
};

/* Convenience predicates. */
static inline bool deck_is(const deck_value_t *v, deck_type_t t) {
    return v && v->type == t;
}
static inline bool deck_is_truthy(const deck_value_t *v) {
    if (!v) return false;
    switch (v->type) {
        case DECK_T_UNIT:     return false;
        case DECK_T_BOOL:     return v->as.b;
        case DECK_T_INT:      return v->as.i != 0;
        case DECK_T_FLOAT:    return v->as.f != 0.0;
        case DECK_T_STR:      return v->as.s.len > 0;
        case DECK_T_BYTES:    return v->as.bytes.len > 0;
        case DECK_T_LIST:     return v->as.list.len > 0;
        case DECK_T_MAP:      return v->as.map.len > 0;
        case DECK_T_TUPLE:    return v->as.tuple.arity > 0;
        case DECK_T_OPTIONAL: return v->as.opt.inner != NULL;
        case DECK_T_ATOM:     return true;
        case DECK_T_FN:       return true;
        case DECK_T_STREAM:   return true;
        default:              return false;
    }
}

#ifdef __cplusplus
}
#endif
