#pragma once

/* deck_ast — AST node types for the DL1 subset of Deck.
 *
 * Nodes live in a deck_arena_t; there is no per-node refcount. The AST
 * is built by the parser and consumed by the loader + evaluator.
 *
 * DL1 coverage (see deck-lang/01-deck-lang.md + 02-deck-app.md + 16 §4):
 *   Expressions:  literals, identifiers, binop/unary, call, dot, if,
 *                 let, match (atoms/literals/wildcards), do block.
 *   Top-level:    @app header, @use locals, @on launch, @machine with
 *                 states + on enter/leave + transitions + send.
 *
 * Not at DL1 (future): @flow, @stream, @task, @handles, @config,
 * records, effect annotations, closures.
 */

#include "deck_arena.h"
#include "deck_error.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* literals */
    AST_LIT_INT = 1,
    AST_LIT_FLOAT,
    AST_LIT_BOOL,
    AST_LIT_STR,
    AST_LIT_ATOM,
    AST_LIT_UNIT,
    AST_LIT_NONE,
    AST_LIT_LIST,    /* DL2 F21.4 — [e1, e2, ...] */
    AST_LIT_TUPLE,   /* DL2 F21.5 — (e1, e2, ...) */
    AST_TUPLE_GET,   /* DL2 F21.5 — tuple.<N> */
    AST_LIT_MAP,     /* DL2 F21.6 — {k: v, ...} */
    AST_WITH,        /* DL2 F22.2 — record with { field: val, ... } */

    /* expressions */
    AST_IDENT,
    AST_BINOP,
    AST_UNARY,
    AST_CALL,
    AST_DOT,
    AST_IF,
    AST_LET,
    AST_DO,
    AST_MATCH,

    /* patterns */
    AST_PAT_LIT,
    AST_PAT_WILD,
    AST_PAT_IDENT,
    AST_PAT_VARIANT,    /* DL2 F22 — `some(x)`, `ok(v)`, `err(e)` */

    /* statements */
    AST_SEND,
    AST_TRANSITION,

    /* DL2 F21.1: user-defined functions */
    AST_FN_DEF,

    /* top-level */
    AST_APP,
    AST_APP_FIELD,
    AST_USE,
    AST_ON,
    AST_MACHINE,
    AST_STATE,
    AST_STATE_HOOK,   /* on enter / on leave */
    AST_MACHINE_TRANSITION,   /* Spec §8.4 — top-level `transition :event from:/to:/when:` */

    AST_MODULE,
    AST_TYPE_DEF,    /* DL2 F22.2 — @type X { fields } */
    AST_ASSETS,      /* DL2 F28.5 — @assets name: "path" ... */
    AST_MIGRATION,   /* DL2 F28.4 — @migration from N: body ... */
    AST_REQUIRES,    /* Spec 02-deck-app §4A — top-level @requires block.
                      * Shares ast_app_field_t layout with AST_APP; parsed
                      * as a sibling to @app, not as a nested field. */
} ast_kind_t;

typedef enum {
    BINOP_ADD, BINOP_SUB, BINOP_MUL, BINOP_DIV, BINOP_MOD, BINOP_POW,
    BINOP_LT,  BINOP_LE,  BINOP_GT,  BINOP_GE,  BINOP_EQ,  BINOP_NE,
    BINOP_AND, BINOP_OR,
    BINOP_CONCAT,
    BINOP_PIPE,
    BINOP_PIPE_OPT,    /* DL2 F21.10 — `x |>? f` short-circuits on none */
    BINOP_IS,          /* DL2 F21.9  — `x is y` value/atom equality */
} binop_t;

typedef enum {
    UNARY_NEG,
    UNARY_NOT,
} unary_t;

typedef struct ast_node ast_node_t;

typedef struct {
    ast_node_t **items;
    uint32_t     len;
    uint32_t     cap;
} ast_list_t;

typedef struct {
    ast_node_t *pattern;
    ast_node_t *guard;   /* may be NULL */
    ast_node_t *body;
} ast_arm_t;

typedef struct {
    const char *name;    /* interned — "name", "id", "version", "edition", "requires" */
    ast_node_t *value;
} ast_app_field_t;

/* Spec 02-deck-app §11 — one entry in an `@on` parameter clause.
 * `field` is the payload field name the handler is binding or matching.
 * `pattern` is an AST pattern node:
 *   - AST_PAT_IDENT        →  binder (captures value into the name)
 *   - AST_PAT_WILD         →  `_` (accept any value, no binding)
 *   - AST_PAT_LIT / other  →  value-pattern filter (handler fires only
 *                             when the incoming field equals the value)
 */
typedef struct {
    const char *field;
    ast_node_t *pattern;
} ast_on_param_t;

/* Spec 02-deck-app §4 — one entry in an `@use` block body:
 *   capability.path  as alias  [optional | when: cond]
 *   ./local/path
 *
 * `module` is the capability path or relative path (interned).
 * `alias` is the explicit `as alias` name, or (when absent) the last
 * dotted segment of `module`. Code refers to the capability by alias.
 * `is_optional` is set when the entry ends in the `optional` keyword.
 */
typedef struct {
    const char *module;
    const char *alias;
    bool        is_optional;
} ast_use_entry_t;

struct ast_node {
    ast_kind_t  kind;
    uint32_t    line;
    uint32_t    col;
    union {
        int64_t             i;           /* AST_LIT_INT */
        double              f;           /* AST_LIT_FLOAT */
        bool                b;           /* AST_LIT_BOOL */
        const char         *s;           /* AST_LIT_STR / AST_LIT_ATOM / AST_IDENT — interned */

        struct { binop_t op; ast_node_t *lhs; ast_node_t *rhs; } binop;
        struct { unary_t op; ast_node_t *expr; }                 unary;
        struct { ast_node_t *fn;  ast_list_t args; }             call;
        struct { ast_node_t *obj; const char *field; }           dot;
        struct { ast_node_t *cond; ast_node_t *then_; ast_node_t *else_; } if_;
        struct { const char *name; ast_node_t *value; ast_node_t *body; }  let;
        struct { ast_list_t exprs; }                             do_;
        struct { ast_list_t items; }                             list;  /* DL2 F21.4 list literal */
        struct { ast_list_t items; }                             tuple_lit; /* DL2 F21.5 tuple literal */
        struct { ast_node_t *obj; uint32_t idx; }                tuple_get; /* DL2 F21.5 tuple field */
        struct { ast_list_t keys; ast_list_t vals; }             map_lit;   /* DL2 F21.6 {k:v, ...} */
        struct { ast_node_t *base; ast_list_t keys; ast_list_t vals; } with_;/* DL2 F22.2 record update */
        struct { ast_node_t *scrut; ast_arm_t *arms; uint32_t n_arms; } match;

        ast_node_t         *pat_lit;                 /* AST_PAT_LIT -> inner literal */
        const char         *pat_ident;               /* AST_PAT_IDENT */
        struct {
            const char *ctor;
            ast_node_t **subs;
            uint32_t     n_subs;
        } pat_variant;                               /* AST_PAT_VARIANT — ctor(sub1, sub2, ...) */

        struct { const char *event; }                send;
        struct { const char *target; }               transition;

        /* DL2 F21.1 + F23: function declaration. params is an array of
         * interned idents in the arena. effects is the list of `!alias`
         * effect annotations declared in the signature; the loader cross-
         * checks each against the surrounding module's @use bindings. */
        struct {
            const char  *name;       /* interned */
            const char **params;     /* array length n_params, arena-owned */
            uint32_t     n_params;
            const char **effects;    /* array length n_effects */
            uint32_t     n_effects;
            ast_node_t  *body;
            bool         is_private; /* DL2 F22.9 — @private prefix */
        } fndef;

        struct { ast_app_field_t *fields; uint32_t n_fields; } app;
        /* Spec 02-deck-app §4 — @use declares a block of bindings. The
         * spec's canonical shape is always a block body; a single
         * binding is expressed as a one-entry block. `entries` is
         * arena-owned. For backwards-compat with older accessors the
         * struct also exposes `module` / `alias` / `is_optional`
         * mirroring `entries[0]` when n_entries == 1. Multi-entry
         * blocks must walk `entries`. */
        struct {
            ast_use_entry_t *entries;
            uint32_t         n_entries;
            const char      *module;       /* = entries[0].module when n==1 */
            const char      *alias;        /* = entries[0].alias  when n==1 */
            bool             is_optional;  /* = entries[0].is_optional when n==1 */
        } use;
        /* Spec 02-deck-app §11 — `@on <dotted.event>` with optional
         * parameter clause. Three binding styles (documented in §11):
         *   (a) no params      — `@on os.locked`             — payload via `event`
         *   (b) named binders  — `@on os.wifi_changed (ssid: s, connected: c)`
         *   (c) value patterns — `@on hardware.button (id: 0, action: :press)`
         *
         * `params` is an array of `ast_on_param_t {field, pattern}` pairs.
         * `pattern` is parsed by parse_pattern, so an IDENT there is a
         * binder, a literal/atom is a value-pattern filter, and `_` is
         * wildcard-accept. n_params == 0 means no clause (style a). */
        struct {
            const char       *event;
            ast_on_param_t   *params;
            uint32_t          n_params;
            ast_node_t       *body;
        } on;
        /* Spec 02-deck-app §8 — @machine has a name, an optional explicit
         * `initial :state` declaration, and a list of state children.
         * When `initial_state` is NULL the runtime falls back to the first
         * state in `states` (historic behaviour, preserved for back-compat). */
        struct {
            const char *name;
            const char *initial_state;   /* interned atom text, or NULL */
            ast_list_t  states;
            ast_list_t  transitions;     /* Concept #44 — AST_MACHINE_TRANSITION list */
        } machine;
        struct { const char *name; ast_list_t hooks; }         state;
        struct { const char *kind; ast_node_t *body; }         state_hook; /* "enter" | "leave" | atom for transition */
        /* Concept #44 — spec §8.4 top-level machine transition.
         * from_state == NULL means `from *` (wildcard — any source state).
         * when_expr, before_body, after_body are optional (NULL = absent). */
        struct {
            const char *event;
            const char *from_state;
            const char *to_state;
            ast_node_t *when_expr;
            ast_node_t *before_body;
            ast_node_t *after_body;
        } machine_transition;

        struct { ast_list_t items; }                 module;
        struct {
            const char  *name;
            const char **fields;     /* field names (interned), arena */
            uint32_t     n_fields;
        } typedef_;

        /* DL2 F28.5 — @assets name: "path" pairs. names and paths are
         * parallel arrays of length n_entries, arena-owned. Looked up by
         * the asset.path(name) builtin at call time. */
        struct {
            const char **names;
            const char **paths;
            uint32_t     n_entries;
        } assets;

        /* DL2 F28.4 — @migration from N: <body> entries. from_versions
         * is an int64 array of the N keys in source order; bodies is a
         * parallel array of AST bodies (each a AST_DO or single stmt).
         * At app_load the runtime reads the NVS-stored version for this
         * app and runs bodies whose key >= stored_version in ascending
         * order, then writes back max(key)+1. */
        struct {
            int64_t     *from_versions;
            ast_node_t **bodies;
            uint32_t     n_entries;
        } migration;
    } as;
};

/* ----- helpers ----- */

ast_node_t *ast_new(deck_arena_t *a, ast_kind_t kind, uint32_t line, uint32_t col);
void        ast_list_init(ast_list_t *l);
deck_err_t  ast_list_push(deck_arena_t *a, ast_list_t *l, ast_node_t *n);

const char *ast_kind_name(ast_kind_t k);
const char *ast_binop_name(binop_t op);
const char *ast_unary_name(unary_t op);

/* S-expr printer into a bounded buffer. Returns number of bytes written
 * (excluding NUL) or 0 on truncation. */
size_t ast_print(const ast_node_t *n, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
