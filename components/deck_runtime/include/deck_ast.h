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

    AST_MODULE,
} ast_kind_t;

typedef enum {
    BINOP_ADD, BINOP_SUB, BINOP_MUL, BINOP_DIV, BINOP_MOD, BINOP_POW,
    BINOP_LT,  BINOP_LE,  BINOP_GT,  BINOP_GE,  BINOP_EQ,  BINOP_NE,
    BINOP_AND, BINOP_OR,
    BINOP_CONCAT,
    BINOP_PIPE,
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
        struct { ast_node_t *scrut; ast_arm_t *arms; uint32_t n_arms; } match;

        ast_node_t         *pat_lit;                 /* AST_PAT_LIT -> inner literal */
        const char         *pat_ident;               /* AST_PAT_IDENT */

        struct { const char *event; }                send;
        struct { const char *target; }               transition;

        /* DL2 F21.1: function declaration. params is an array of interned
         * idents in the arena; type annotations on params and return are
         * parsed but not retained at this level. */
        struct {
            const char  *name;       /* interned */
            const char **params;     /* array length n_params, arena-owned */
            uint32_t     n_params;
            ast_node_t  *body;
        } fndef;

        struct { ast_app_field_t *fields; uint32_t n_fields; } app;
        struct { const char *module; }               use;
        struct { const char *event; ast_node_t *body; } on;
        struct { const char *name; ast_list_t states; }        machine;
        struct { const char *name; ast_list_t hooks; }         state;
        struct { const char *kind; ast_node_t *body; }         state_hook; /* "enter" | "leave" | atom for transition */

        struct { ast_list_t items; }                 module;
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
