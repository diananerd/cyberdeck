#include "deck_parser.h"
#include "deck_intern.h"

#include <stdio.h>
#include <string.h>

/* ================================================================
 * Token stream helpers
 * ================================================================ */

static void advance(deck_parser_t *p)
{
    if (p->have_peek) {
        p->cur = p->peek;
        p->have_peek = false;
    } else {
        if (!deck_lexer_next(&p->lx, &p->cur)) {
            p->cur.type = TOK_EOF;
        }
    }
}

static bool at(deck_parser_t *p, deck_tok_t t) { return p->cur.type == t; }

static void set_err(deck_parser_t *p, deck_err_t code, const char *msg)
{
    if (p->err == DECK_LOAD_OK) {
        p->err      = code;
        p->err_msg  = msg;
        p->err_line = p->cur.line;
        p->err_col  = p->cur.col;
    }
}

static bool expect(deck_parser_t *p, deck_tok_t t, const char *msg)
{
    if (p->cur.type != t) { set_err(p, DECK_LOAD_PARSE_ERROR, msg); return false; }
    advance(p);
    return true;
}

/* Skip any NEWLINE tokens without leaving indent state. */
static void skip_newlines(deck_parser_t *p)
{
    while (at(p, TOK_NEWLINE)) advance(p);
}

/* One-token lookahead. The parser already reserves a `peek` slot but
 * never used it pre-DL2; F21.2 lambdas need it to disambiguate
 * `IDENT ->` (lambda) from a bare ident expression. */
static deck_tok_t peek_next_tok(deck_parser_t *p)
{
    if (!p->have_peek) {
        if (!deck_lexer_next(&p->lx, &p->peek)) {
            p->peek.type = TOK_EOF;
        }
        p->have_peek = true;
    }
    return p->peek.type;
}

/* ================================================================
 * Expression precedence (Pratt)
 * ================================================================ */

typedef struct { binop_t op; int prec; bool right_assoc; } binop_info_t;

static bool binop_for(deck_tok_t t, binop_info_t *out)
{
    switch (t) {
        case TOK_OR_OR:
        case TOK_KW_OR:   *out = (binop_info_t){ BINOP_OR,     1, false }; return true;
        case TOK_AND_AND:
        case TOK_KW_AND:  *out = (binop_info_t){ BINOP_AND,    2, false }; return true;
        case TOK_EQ:      *out = (binop_info_t){ BINOP_EQ,     3, false }; return true;
        case TOK_NE:      *out = (binop_info_t){ BINOP_NE,     3, false }; return true;
        case TOK_LT:      *out = (binop_info_t){ BINOP_LT,     4, false }; return true;
        case TOK_LE:      *out = (binop_info_t){ BINOP_LE,     4, false }; return true;
        case TOK_GT:      *out = (binop_info_t){ BINOP_GT,     4, false }; return true;
        case TOK_GE:      *out = (binop_info_t){ BINOP_GE,     4, false }; return true;
        case TOK_CONCAT:  *out = (binop_info_t){ BINOP_CONCAT, 5, false }; return true;
        case TOK_PIPE:    *out = (binop_info_t){ BINOP_PIPE,   5, false }; return true;
        case TOK_PIPE_OPT:*out = (binop_info_t){ BINOP_PIPE_OPT, 5, false }; return true;
        case TOK_KW_IS:   *out = (binop_info_t){ BINOP_IS,     3, false }; return true;
        case TOK_PLUS:    *out = (binop_info_t){ BINOP_ADD,    6, false }; return true;
        case TOK_MINUS:   *out = (binop_info_t){ BINOP_SUB,    6, false }; return true;
        case TOK_STAR:    *out = (binop_info_t){ BINOP_MUL,    7, false }; return true;
        case TOK_SLASH:   *out = (binop_info_t){ BINOP_DIV,    7, false }; return true;
        case TOK_PERCENT: *out = (binop_info_t){ BINOP_MOD,    7, false }; return true;
        case TOK_POW:     *out = (binop_info_t){ BINOP_POW,    8, true  }; return true;
        default:          return false;
    }
}

static ast_node_t *parse_expr_prec(deck_parser_t *p, int min_prec);
static ast_node_t *parse_primary(deck_parser_t *p);
static ast_node_t *parse_match(deck_parser_t *p);
static ast_node_t *parse_if(deck_parser_t *p);
static ast_node_t *parse_postfix(deck_parser_t *p, ast_node_t *head);
static ast_node_t *parse_fn_decl(deck_parser_t *p);

static ast_node_t *mknode(deck_parser_t *p, ast_kind_t k)
{
    return ast_new(p->arena, k, p->cur.line, p->cur.col);
}

static ast_node_t *parse_primary(deck_parser_t *p)
{
    ast_node_t *n = NULL;
    switch (p->cur.type) {
        case TOK_INT:
            n = mknode(p, AST_LIT_INT); if (!n) return NULL;
            n->as.i = p->cur.as.i; advance(p); break;
        case TOK_FLOAT:
            n = mknode(p, AST_LIT_FLOAT); if (!n) return NULL;
            n->as.f = p->cur.as.f; advance(p); break;
        case TOK_KW_TRUE:
            n = mknode(p, AST_LIT_BOOL); if (!n) return NULL;
            n->as.b = true;  advance(p); break;
        case TOK_KW_FALSE:
            n = mknode(p, AST_LIT_BOOL); if (!n) return NULL;
            n->as.b = false; advance(p); break;
        case TOK_KW_UNIT:
            n = mknode(p, AST_LIT_UNIT); advance(p); break;
        case TOK_KW_NONE:
            n = mknode(p, AST_LIT_NONE); advance(p); break;
        case TOK_STRING:
            n = mknode(p, AST_LIT_STR); if (!n) return NULL;
            n->as.s = p->cur.text; advance(p); break;
        case TOK_STRING_INTERP: {
            /* DL2 F21.7 — split the raw string text by ${...} and build
             * a concat tree of string fragments + parsed expressions
             * (auto-stringified via str()). Wraps the whole thing in a
             * single concat AST so binop precedence sees an atom. */
            const char *src = p->cur.text;
            uint32_t    src_len = p->cur.text_len;
            uint32_t    ln = p->cur.line, co = p->cur.col;
            advance(p);

            ast_node_t *acc = NULL;   /* accumulated concat result */
            char        text_buf[512];
            uint32_t    tk = 0;

            #define APPEND_TEXT(node)                                       \
                do { ast_node_t *_t = (node); if (!_t) return NULL;        \
                     if (!acc) { acc = _t; }                                \
                     else { ast_node_t *bn = ast_new(p->arena, AST_BINOP, ln, co); \
                            if (!bn) return NULL;                           \
                            bn->as.binop.op = BINOP_CONCAT;                 \
                            bn->as.binop.lhs = acc;                         \
                            bn->as.binop.rhs = _t;                          \
                            acc = bn; }                                     \
                } while(0)

            #define FLUSH_TEXT()                                             \
                do { if (tk > 0) {                                           \
                       ast_node_t *sn = ast_new(p->arena, AST_LIT_STR, ln, co); \
                       if (!sn) return NULL;                                 \
                       sn->as.s = deck_intern(text_buf, tk);                 \
                       APPEND_TEXT(sn);                                      \
                       tk = 0;                                               \
                   } } while(0)

            for (uint32_t i = 0; i < src_len;) {
                char c = src[i];
                if (c == '\\' && i + 1 < src_len) {
                    char esc;
                    switch (src[i + 1]) {
                        case 'n': esc = '\n'; break;
                        case 't': esc = '\t'; break;
                        case 'r': esc = '\r'; break;
                        case '\\': esc = '\\'; break;
                        case '"': esc = '"'; break;
                        case '0': esc = '\0'; break;
                        default: esc = src[i + 1]; break;
                    }
                    if (tk + 1 >= sizeof(text_buf)) { set_err(p, DECK_LOAD_PARSE_ERROR, "interp string too long"); return NULL; }
                    text_buf[tk++] = esc;
                    i += 2;
                    continue;
                }
                if (c == '$' && i + 1 < src_len && src[i + 1] == '{') {
                    FLUSH_TEXT();
                    /* find matching `}` (skip the leading `${`) */
                    uint32_t start = i + 2;
                    int depth = 1;
                    uint32_t j = start;
                    while (j < src_len && depth > 0) {
                        if (src[j] == '{') depth++;
                        else if (src[j] == '}') depth--;
                        if (depth > 0) j++;
                    }
                    if (depth != 0) {
                        set_err(p, DECK_LOAD_PARSE_ERROR, "unterminated interpolation");
                        return NULL;
                    }
                    /* Recursively parse the inner expression source. */
                    deck_parser_t inner;
                    deck_parser_init(&inner, p->arena, src + start, j - start);
                    ast_node_t *expr = deck_parser_parse_expr(&inner);
                    if (!expr || deck_parser_err_code(&inner) != DECK_LOAD_OK) {
                        set_err(p, DECK_LOAD_PARSE_ERROR,
                                deck_parser_err_msg(&inner) ?
                                deck_parser_err_msg(&inner) :
                                "bad expression in interpolation");
                        return NULL;
                    }
                    /* Wrap in str(...) so non-string values stringify. */
                    ast_node_t *callee = ast_new(p->arena, AST_IDENT, ln, co);
                    if (!callee) return NULL;
                    callee->as.s = deck_intern_cstr("str");
                    ast_node_t *call = ast_new(p->arena, AST_CALL, ln, co);
                    if (!call) return NULL;
                    call->as.call.fn = callee;
                    ast_list_init(&call->as.call.args);
                    ast_list_push(p->arena, &call->as.call.args, expr);
                    APPEND_TEXT(call);
                    i = j + 1;   /* past the `}` */
                    continue;
                }
                if (tk + 1 >= sizeof(text_buf)) { set_err(p, DECK_LOAD_PARSE_ERROR, "interp string too long"); return NULL; }
                text_buf[tk++] = c;
                i++;
            }
            FLUSH_TEXT();

            #undef APPEND_TEXT
            #undef FLUSH_TEXT

            if (!acc) {
                /* Empty interpolated string e.g. `""` (shouldn't reach here
                 * since plain "" emits TOK_STRING). Build empty literal. */
                acc = ast_new(p->arena, AST_LIT_STR, ln, co);
                if (!acc) return NULL;
                acc->as.s = deck_intern("", 0);
            }
            n = acc;
            break;
        }
        case TOK_ATOM: {
            uint32_t ln = p->cur.line, co = p->cur.col;
            const char *ctor = p->cur.text;
            advance(p);
            /* Spec 01-deck-lang §3.7 — atom variant value: `:ctor payload`.
             * If the next token clearly starts a primary expression, this
             * is a variant-constructor expression; otherwise it's a bare
             * atom literal. Desugars to a 2-tuple (:ctor, payload) so the
             * match-side `AST_PAT_VARIANT` (spec-canonical `:ctor x`) can
             * destructure it uniformly. The existing `some()/ok()/err()`
             * builtins keep their native shapes (Optional / tuple); the
             * tuple shape produced here is compatible with `:ok x` /
             * `:err x` match arms (both are tuple (:ctor, payload)). */
            bool starts_primary = false;
            switch (p->cur.type) {
                case TOK_INT: case TOK_FLOAT: case TOK_STRING:
                case TOK_KW_TRUE: case TOK_KW_FALSE: case TOK_KW_UNIT:
                case TOK_KW_NONE: case TOK_KW_SOME:
                case TOK_ATOM: case TOK_IDENT:
                case TOK_LPAREN: case TOK_LBRACKET: case TOK_LBRACE:
                    starts_primary = true; break;
                default: break;
            }
            if (!starts_primary) {
                n = ast_new(p->arena, AST_LIT_ATOM, ln, co); if (!n) return NULL;
                n->as.s = ctor;
                break;
            }
            ast_node_t *payload = parse_primary(p);
            if (!payload) return NULL;
            ast_node_t *atom_node = ast_new(p->arena, AST_LIT_ATOM, ln, co);
            if (!atom_node) return NULL;
            atom_node->as.s = ctor;
            ast_node_t *tup = ast_new(p->arena, AST_LIT_TUPLE, ln, co);
            if (!tup) return NULL;
            ast_list_init(&tup->as.tuple_lit.items);
            ast_list_push(p->arena, &tup->as.tuple_lit.items, atom_node);
            ast_list_push(p->arena, &tup->as.tuple_lit.items, payload);
            n = tup;
            break;
        }
        case TOK_KW_SOME: {
            /* DL2 F21.9 — `some` is a callable Optional constructor.
             * Treat as a bare ident so call dispatch finds the builtin. */
            n = mknode(p, AST_IDENT); if (!n) return NULL;
            n->as.s = deck_intern_cstr("some");
            advance(p);
            break;
        }
        case TOK_IDENT:
            /* DL2 F21.2: single-ident lambda `x -> body`. We commit
             * before consuming the ident, then take the lambda branch
             * if the next token is `->`. */
            if (peek_next_tok(p) == TOK_ARROW) {
                uint32_t ln = p->cur.line, co = p->cur.col;
                const char *param = p->cur.text;
                advance(p);  /* ident */
                advance(p);  /* -> */
                ast_node_t *body = parse_expr_prec(p, 0);
                if (!body) return NULL;
                n = ast_new(p->arena, AST_FN_DEF, ln, co); if (!n) return NULL;
                const char **plist = deck_arena_alloc(p->arena, sizeof(char *));
                if (!plist) return NULL;
                plist[0] = param;
                n->as.fndef.name     = NULL;
                n->as.fndef.params   = plist;
                n->as.fndef.n_params = 1;
                n->as.fndef.body     = body;
                break;
            }
            n = mknode(p, AST_IDENT); if (!n) return NULL;
            n->as.s = p->cur.text; advance(p); break;
        case TOK_LPAREN: {
            /* DL2 F21.5 + F21.2 multi-arg lambda:
             *   (e)                   → paren-grouping
             *   (e1, e2, ...)         → tuple literal
             *   (a, b, ...) -> body   → multi-arg lambda
             *   (a) -> body           → single-arg lambda (paren form)
             * After parsing the inner content we peek for `->` to upgrade
             * a paren/tuple shape into a lambda. */
            uint32_t ln = p->cur.line, co = p->cur.col;
            advance(p);
            ast_node_t *first = parse_expr_prec(p, 0);
            if (!first) return NULL;
            ast_node_t *tup = NULL;
            if (at(p, TOK_COMMA)) {
                tup = ast_new(p->arena, AST_LIT_TUPLE, ln, co); if (!tup) return NULL;
                ast_list_init(&tup->as.tuple_lit.items);
                ast_list_push(p->arena, &tup->as.tuple_lit.items, first);
                while (at(p, TOK_COMMA)) {
                    advance(p);
                    if (at(p, TOK_RPAREN)) break;
                    ast_node_t *item = parse_expr_prec(p, 0);
                    if (!item) return NULL;
                    ast_list_push(p->arena, &tup->as.tuple_lit.items, item);
                }
                if (!expect(p, TOK_RPAREN, "expected ')' to close tuple")) return NULL;
            } else {
                if (!expect(p, TOK_RPAREN, "expected ')'")) return NULL;
            }
            if (at(p, TOK_ARROW)) {
                /* Lambda. Items (or the single first) must be ident-only. */
                advance(p);    /* -> */
                ast_list_t *src = tup ? &tup->as.tuple_lit.items : NULL;
                uint32_t np = src ? src->len : 1;
                const char **plist = deck_arena_alloc(p->arena, np * sizeof(char *));
                if (!plist) return NULL;
                if (src) {
                    for (uint32_t i = 0; i < np; i++) {
                        ast_node_t *item = src->items[i];
                        if (!item || item->kind != AST_IDENT) {
                            set_err(p, DECK_LOAD_PARSE_ERROR,
                                    "lambda parameter list must contain only identifiers");
                            return NULL;
                        }
                        plist[i] = item->as.s;
                    }
                } else {
                    if (!first || first->kind != AST_IDENT) {
                        set_err(p, DECK_LOAD_PARSE_ERROR,
                                "lambda parameter list must contain only identifiers");
                        return NULL;
                    }
                    plist[0] = first->as.s;
                }
                ast_node_t *body = parse_expr_prec(p, 0);
                if (!body) return NULL;
                n = ast_new(p->arena, AST_FN_DEF, ln, co); if (!n) return NULL;
                n->as.fndef.name     = NULL;
                n->as.fndef.params   = plist;
                n->as.fndef.n_params = np;
                n->as.fndef.body     = body;
            } else {
                n = tup ? tup : first;
            }
            break;
        }
        case TOK_LBRACE: {
            /* DL2 F21.6: map literal `{k1: v1, k2: v2, ...}` (also `{}`).
             * Keys may be any expression (typically atom/string/int). */
            uint32_t ln = p->cur.line, co = p->cur.col;
            advance(p);
            n = ast_new(p->arena, AST_LIT_MAP, ln, co); if (!n) return NULL;
            ast_list_init(&n->as.map_lit.keys);
            ast_list_init(&n->as.map_lit.vals);
            if (!at(p, TOK_RBRACE)) {
                for (;;) {
                    ast_node_t *k = parse_expr_prec(p, 0);
                    if (!k) return NULL;
                    if (!expect(p, TOK_COLON, "expected ':' in map literal")) return NULL;
                    ast_node_t *v = parse_expr_prec(p, 0);
                    if (!v) return NULL;
                    ast_list_push(p->arena, &n->as.map_lit.keys, k);
                    ast_list_push(p->arena, &n->as.map_lit.vals, v);
                    if (!at(p, TOK_COMMA)) break;
                    advance(p);
                    if (at(p, TOK_RBRACE)) break;   /* trailing comma */
                }
            }
            if (!expect(p, TOK_RBRACE, "expected '}' to close map")) return NULL;
            break;
        }
        case TOK_LBRACKET: {
            /* DL2 F21.4: list literal `[e1, e2, ...]` (also empty `[]`). */
            uint32_t ln = p->cur.line, co = p->cur.col;
            advance(p);
            n = ast_new(p->arena, AST_LIT_LIST, ln, co); if (!n) return NULL;
            ast_list_init(&n->as.list.items);
            if (!at(p, TOK_RBRACKET)) {
                for (;;) {
                    ast_node_t *item = parse_expr_prec(p, 0);
                    if (!item) return NULL;
                    ast_list_push(p->arena, &n->as.list.items, item);
                    if (!at(p, TOK_COMMA)) break;
                    advance(p);
                }
            }
            if (!expect(p, TOK_RBRACKET, "expected ']' to close list")) return NULL;
            break;
        }
        case TOK_MINUS: {
            uint32_t ln = p->cur.line, co = p->cur.col;
            advance(p);
            ast_node_t *sub = parse_primary(p);
            if (!sub) return NULL;
            n = ast_new(p->arena, AST_UNARY, ln, co); if (!n) return NULL;
            n->as.unary.op = UNARY_NEG; n->as.unary.expr = sub;
            return parse_postfix(p, n);
        }
        case TOK_BANG:
        case TOK_KW_NOT: {
            uint32_t ln = p->cur.line, co = p->cur.col;
            advance(p);
            ast_node_t *sub = parse_primary(p);
            if (!sub) return NULL;
            n = ast_new(p->arena, AST_UNARY, ln, co); if (!n) return NULL;
            n->as.unary.op = UNARY_NOT; n->as.unary.expr = sub;
            return parse_postfix(p, n);
        }
        case TOK_KW_MATCH: return parse_match(p);
        case TOK_KW_IF:    return parse_if(p);
        case TOK_KW_FN: {
            /* DL2 F21.2: anonymous fn in expression position. */
            ast_node_t *fnn = parse_fn_decl(p);
            if (!fnn) return NULL;
            if (fnn->as.fndef.name) {
                set_err(p, DECK_LOAD_PARSE_ERROR,
                        "named fn declarations are only allowed at top level");
                return NULL;
            }
            return parse_postfix(p, fnn);
        }
        default:
            set_err(p, DECK_LOAD_PARSE_ERROR, "expected expression");
            return NULL;
    }
    if (!n) return NULL;
    return parse_postfix(p, n);
}

static ast_node_t *parse_postfix(deck_parser_t *p, ast_node_t *head)
{
    for (;;) {
        if (at(p, TOK_DOT)) {
            advance(p);
            /* DL2 F21.5: `.0`, `.1`, ... is tuple-field access. */
            if (at(p, TOK_INT)) {
                if (p->cur.as.i < 0) {
                    set_err(p, DECK_LOAD_PARSE_ERROR, "tuple index must be non-negative");
                    return NULL;
                }
                ast_node_t *n = mknode(p, AST_TUPLE_GET); if (!n) return NULL;
                n->as.tuple_get.obj = head;
                n->as.tuple_get.idx = (uint32_t)p->cur.as.i;
                advance(p);
                head = n;
                continue;
            }
            if (!at(p, TOK_IDENT)) { set_err(p, DECK_LOAD_PARSE_ERROR, "expected field name or index after '.'"); return NULL; }
            ast_node_t *n = mknode(p, AST_DOT); if (!n) return NULL;
            n->as.dot.obj   = head;
            n->as.dot.field = p->cur.text;
            advance(p);
            head = n;
            continue;
        }
        if (at(p, TOK_LPAREN)) {
            advance(p);
            ast_node_t *c = mknode(p, AST_CALL); if (!c) return NULL;
            c->as.call.fn = head;
            ast_list_init(&c->as.call.args);
            if (!at(p, TOK_RPAREN)) {
                for (;;) {
                    ast_node_t *arg = parse_expr_prec(p, 0);
                    if (!arg) return NULL;
                    ast_list_push(p->arena, &c->as.call.args, arg);
                    if (!at(p, TOK_COMMA)) break;
                    advance(p);
                }
            }
            if (!expect(p, TOK_RPAREN, "expected ')' after call args")) return NULL;
            head = c;
            continue;
        }
        if (at(p, TOK_KW_WITH)) {
            /* DL2 F22.2 — `expr with { field: val, ... }` returns a
             * new record/map with the given fields updated. */
            advance(p); /* with */
            if (!expect(p, TOK_LBRACE, "expected '{' after `with`")) return NULL;
            ast_node_t *w = mknode(p, AST_WITH); if (!w) return NULL;
            w->as.with_.base = head;
            ast_list_init(&w->as.with_.keys);
            ast_list_init(&w->as.with_.vals);
            if (!at(p, TOK_RBRACE)) {
                for (;;) {
                    ast_node_t *k = parse_expr_prec(p, 0);
                    if (!k) return NULL;
                    if (!expect(p, TOK_COLON, "expected ':' in with update")) return NULL;
                    ast_node_t *v = parse_expr_prec(p, 0);
                    if (!v) return NULL;
                    ast_list_push(p->arena, &w->as.with_.keys, k);
                    ast_list_push(p->arena, &w->as.with_.vals, v);
                    if (!at(p, TOK_COMMA)) break;
                    advance(p);
                    if (at(p, TOK_RBRACE)) break;
                }
            }
            if (!expect(p, TOK_RBRACE, "expected '}' to close with update")) return NULL;
            head = w;
            continue;
        }
        break;
    }
    return head;
}

/* DL2 F21.11 — `where` bindings.
 *
 * Postfix on an expression: introduces local bindings into its scope.
 * Two forms:
 *   expr where x = v                  (single inline)
 *   expr where x = v, y = w           (inline list, comma-sep)
 *   expr where\n  x = v\n  y = w      (indented block)
 *
 * Implemented as nested AST_LET wrappers — outermost binding is the
 * first declared. Only fires at the top of a min_prec=0 expression so
 * `where` inside a binop sub-expression doesn't accidentally bind. */
static ast_node_t *wrap_where_bindings(deck_parser_t *p, ast_node_t *body,
                                       const char **names, ast_node_t **vals,
                                       uint32_t n)
{
    for (int i = (int)n - 1; i >= 0; i--) {
        ast_node_t *let = ast_new(p->arena, AST_LET, p->cur.line, p->cur.col);
        if (!let) return NULL;
        let->as.let.name  = names[i];
        let->as.let.value = vals[i];
        let->as.let.body  = body;
        body = let;
    }
    return body;
}

static ast_node_t *parse_where_postfix(deck_parser_t *p, ast_node_t *body)
{
    advance(p);   /* consume `where` */
    const char *names[16];
    ast_node_t *vals[16];
    uint32_t   n = 0;

    if (at(p, TOK_NEWLINE)) {
        /* Indented block form. */
        while (at(p, TOK_NEWLINE)) advance(p);
        if (!expect(p, TOK_INDENT, "expected indented `where` bindings")) return NULL;
        while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
            if (n >= 16) {
                set_err(p, DECK_LOAD_PARSE_ERROR, "too many `where` bindings (max 16)");
                return NULL;
            }
            if (!at(p, TOK_IDENT)) {
                set_err(p, DECK_LOAD_PARSE_ERROR, "expected name in `where` binding");
                return NULL;
            }
            names[n] = p->cur.text;
            advance(p);
            if (!expect(p, TOK_ASSIGN, "expected '=' in `where` binding")) return NULL;
            /* Use min_prec=1 so a nested `where` inside the value won't
             * eat the surrounding context. */
            vals[n] = parse_expr_prec(p, 1);
            if (!vals[n]) return NULL;
            n++;
            while (at(p, TOK_NEWLINE)) advance(p);
        }
        if (!expect(p, TOK_DEDENT, "expected dedent closing `where` block")) return NULL;
    } else {
        /* Inline single or comma-separated. */
        for (;;) {
            if (n >= 16) {
                set_err(p, DECK_LOAD_PARSE_ERROR, "too many `where` bindings (max 16)");
                return NULL;
            }
            if (!at(p, TOK_IDENT)) {
                set_err(p, DECK_LOAD_PARSE_ERROR, "expected name in `where` binding");
                return NULL;
            }
            names[n] = p->cur.text;
            advance(p);
            if (!expect(p, TOK_ASSIGN, "expected '=' in `where` binding")) return NULL;
            vals[n] = parse_expr_prec(p, 1);
            if (!vals[n]) return NULL;
            n++;
            if (!at(p, TOK_COMMA)) break;
            advance(p);
        }
    }
    return wrap_where_bindings(p, body, names, vals, n);
}

static ast_node_t *parse_expr_prec(deck_parser_t *p, int min_prec)
{
    ast_node_t *lhs = parse_primary(p);
    if (!lhs) return NULL;
    for (;;) {
        binop_info_t info;
        if (!binop_for(p->cur.type, &info) || info.prec < min_prec) break;
        int next_min = info.right_assoc ? info.prec : info.prec + 1;
        uint32_t ln = p->cur.line, co = p->cur.col;
        advance(p);
        ast_node_t *rhs = parse_expr_prec(p, next_min);
        if (!rhs) return NULL;
        ast_node_t *n = ast_new(p->arena, AST_BINOP, ln, co); if (!n) return NULL;
        n->as.binop.op = info.op; n->as.binop.lhs = lhs; n->as.binop.rhs = rhs;
        lhs = n;
    }
    if (min_prec == 0 && at(p, TOK_KW_WHERE)) {
        lhs = parse_where_postfix(p, lhs);
        if (!lhs) return NULL;
    }
    return lhs;
}

ast_node_t *deck_parser_parse_expr(deck_parser_t *p)
{
    if (p->err != DECK_LOAD_OK) return NULL;
    return parse_expr_prec(p, 0);
}

/* ================================================================
 * Patterns + match / if
 * ================================================================ */

static ast_node_t *parse_pattern(deck_parser_t *p)
{
    /* DL2 F22 — variant patterns `some(x)`, `ok(v)`, etc. detected by
     * IDENT-or-KW-some followed by `(`. Treat TOK_KW_SOME as an ident
     * for this purpose. */
    bool ident_like = (p->cur.type == TOK_IDENT || p->cur.type == TOK_KW_SOME);
    if (ident_like && peek_next_tok(p) == TOK_LPAREN) {
        const char *ctor = p->cur.text;
        if (p->cur.type == TOK_KW_SOME) ctor = deck_intern_cstr("some");
        uint32_t ln = p->cur.line, co = p->cur.col;
        advance(p); advance(p);   /* ident, ( */
        ast_node_t *subs[8];
        uint32_t n_subs = 0;
        if (!at(p, TOK_RPAREN)) {
            for (;;) {
                if (n_subs >= 8) {
                    set_err(p, DECK_LOAD_PARSE_ERROR, "variant pattern: too many subs (max 8)");
                    return NULL;
                }
                ast_node_t *s = parse_pattern(p);
                if (!s) return NULL;
                subs[n_subs++] = s;
                if (!at(p, TOK_COMMA)) break;
                advance(p);
            }
        }
        if (!expect(p, TOK_RPAREN, "expected ')' in variant pattern")) return NULL;
        ast_node_t *n = ast_new(p->arena, AST_PAT_VARIANT, ln, co);
        if (!n) return NULL;
        n->as.pat_variant.ctor   = ctor;
        n->as.pat_variant.subs   = deck_arena_memdup(p->arena, subs, n_subs * sizeof(ast_node_t *));
        n->as.pat_variant.n_subs = n_subs;
        return n;
    }
    switch (p->cur.type) {
        case TOK_IDENT: {
            /* "_" wildcard or bind ident */
            if (p->cur.text && strcmp(p->cur.text, "_") == 0) {
                ast_node_t *n = mknode(p, AST_PAT_WILD);
                advance(p); return n;
            }
            ast_node_t *n = mknode(p, AST_PAT_IDENT); if (!n) return NULL;
            n->as.pat_ident = p->cur.text;
            advance(p); return n;
        }
        case TOK_INT: case TOK_FLOAT: case TOK_STRING:
        case TOK_KW_TRUE: case TOK_KW_FALSE: case TOK_KW_UNIT:
        case TOK_KW_NONE: {
            ast_node_t *wrap = mknode(p, AST_PAT_LIT); if (!wrap) return NULL;
            wrap->as.pat_lit = parse_primary(p);
            return wrap;
        }
        case TOK_ATOM: {
            /* Spec 01-deck-lang §8 — atom variant pattern `:name binder`.
             * Construct the atom literal directly (do NOT route through
             * parse_primary, which now also treats `:atom value` as a
             * variant-value constructor — that's expression position, not
             * pattern position). When an atom is followed by an ident the
             * arm is `:ctor binder`; bare `:atom` stays a literal pattern. */
            uint32_t ln = p->cur.line, co = p->cur.col;
            const char *atom_name = p->cur.text;
            advance(p);
            ast_node_t *atom_lit = ast_new(p->arena, AST_LIT_ATOM, ln, co);
            if (!atom_lit) return NULL;
            atom_lit->as.s = atom_name;
            if (at(p, TOK_IDENT)) {
                ast_node_t *sub = parse_pattern(p);
                if (!sub) return NULL;
                ast_node_t *n = ast_new(p->arena, AST_PAT_VARIANT, ln, co);
                if (!n) return NULL;
                ast_node_t **subs = deck_arena_alloc(p->arena, sizeof(ast_node_t *));
                if (!subs) return NULL;
                subs[0] = sub;
                n->as.pat_variant.ctor   = atom_name;
                n->as.pat_variant.subs   = subs;
                n->as.pat_variant.n_subs = 1;
                return n;
            }
            ast_node_t *wrap = ast_new(p->arena, AST_PAT_LIT, ln, co);
            if (!wrap) return NULL;
            wrap->as.pat_lit = atom_lit;
            return wrap;
        }
        default:
            set_err(p, DECK_LOAD_PARSE_ERROR, "expected pattern");
            return NULL;
    }
}

static ast_node_t *parse_match(deck_parser_t *p)
{
    ast_node_t *m = mknode(p, AST_MATCH); if (!m) return NULL;
    advance(p); /* match */
    m->as.match.scrut = parse_expr_prec(p, 0);
    if (!m->as.match.scrut) return NULL;
    if (!expect(p, TOK_NEWLINE, "expected newline after match scrutinee")) return NULL;
    if (!expect(p, TOK_INDENT, "expected indented block for match arms")) return NULL;

    ast_arm_t arms[32];
    uint32_t n_arms = 0;
    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF) && n_arms < 32) {
        /* Spec 01-deck-lang §8 — each arm is written `| pattern -> expr`.
         * The leading `|` is required by the canonical form. We tolerate
         * its absence for one-arm matches and historic fixtures; future
         * deepening should add a lint to prefer the explicit `|`. */
        if (at(p, TOK_BAR)) advance(p);
        ast_node_t *pat = parse_pattern(p); if (!pat) return NULL;
        ast_node_t *guard = NULL;
        if (at(p, TOK_KW_WHEN)) { advance(p); guard = parse_expr_prec(p, 0); if (!guard) return NULL; }
        if (!at(p, TOK_ARROW)) {
            set_err(p, DECK_LOAD_PARSE_ERROR,
                    "expected '->' in match arm (spec 01-deck-lang §8); "
                    "the legacy '=>' arrow is no longer accepted");
            return NULL;
        }
        advance(p); /* -> */
        ast_node_t *body = parse_expr_prec(p, 0); if (!body) return NULL;
        while (at(p, TOK_NEWLINE)) advance(p);
        arms[n_arms].pattern = pat;
        arms[n_arms].guard   = guard;
        arms[n_arms].body    = body;
        n_arms++;
    }
    if (!expect(p, TOK_DEDENT, "expected dedent after match arms")) return NULL;
    m->as.match.arms   = deck_arena_memdup(p->arena, arms, n_arms * sizeof(ast_arm_t));
    m->as.match.n_arms = n_arms;
    return m;
}

static ast_node_t *parse_if(deck_parser_t *p)
{
    ast_node_t *n = mknode(p, AST_IF); if (!n) return NULL;
    advance(p); /* if */
    n->as.if_.cond = parse_expr_prec(p, 0); if (!n->as.if_.cond) return NULL;
    if (!expect(p, TOK_KW_THEN, "expected 'then' after if condition")) return NULL;
    n->as.if_.then_ = parse_expr_prec(p, 0); if (!n->as.if_.then_) return NULL;
    if (!expect(p, TOK_KW_ELSE, "expected 'else' in if expression")) return NULL;
    n->as.if_.else_ = parse_expr_prec(p, 0); if (!n->as.if_.else_) return NULL;
    return n;
}

/* ================================================================
 * Statements / suites
 * ================================================================ */

static ast_node_t *parse_suite(deck_parser_t *p);

static ast_node_t *parse_let_stmt(deck_parser_t *p)
{
    ast_node_t *n = mknode(p, AST_LET); if (!n) return NULL;
    advance(p); /* let */
    if (!at(p, TOK_IDENT)) { set_err(p, DECK_LOAD_PARSE_ERROR, "expected name after 'let'"); return NULL; }
    n->as.let.name = p->cur.text;
    advance(p);
    if (!expect(p, TOK_ASSIGN, "expected '=' in let binding")) return NULL;
    n->as.let.value = parse_expr_prec(p, 0); if (!n->as.let.value) return NULL;
    n->as.let.body  = NULL; /* body attached by enclosing suite */
    return n;
}

static ast_node_t *parse_send_stmt(deck_parser_t *p)
{
    ast_node_t *n = mknode(p, AST_SEND); if (!n) return NULL;
    advance(p); /* send */
    if (!at(p, TOK_ATOM)) { set_err(p, DECK_LOAD_PARSE_ERROR, "expected atom after 'send'"); return NULL; }
    n->as.send.event = p->cur.text;
    advance(p);
    return n;
}

static ast_node_t *parse_transition_stmt(deck_parser_t *p)
{
    ast_node_t *n = mknode(p, AST_TRANSITION); if (!n) return NULL;
    advance(p); /* transition */
    if (!at(p, TOK_ATOM)) { set_err(p, DECK_LOAD_PARSE_ERROR, "expected atom after 'transition'"); return NULL; }
    n->as.transition.target = p->cur.text;
    advance(p);
    return n;
}

static ast_node_t *parse_stmt(deck_parser_t *p)
{
    switch (p->cur.type) {
        case TOK_KW_LET:        return parse_let_stmt(p);
        case TOK_KW_SEND:       return parse_send_stmt(p);
        case TOK_KW_TRANSITION: return parse_transition_stmt(p);
        default:                return parse_expr_prec(p, 0);
    }
}

/* Suite — one of:
 *   <stmt> NEWLINE
 *   NEWLINE INDENT stmt+ DEDENT
 * Returns a DO node wrapping statements (or the single stmt directly). */
static ast_node_t *parse_suite(deck_parser_t *p)
{
    if (at(p, TOK_NEWLINE)) {
        advance(p);
        /* Tolerate extra NEWLINE tokens inserted by blank/comment-only
         * lines before the indented block begins. */
        while (at(p, TOK_NEWLINE)) advance(p);
        if (!expect(p, TOK_INDENT, "expected indented suite")) return NULL;
        ast_node_t *d = mknode(p, AST_DO); if (!d) return NULL;
        ast_list_init(&d->as.do_.exprs);
        while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
            ast_node_t *s = parse_stmt(p); if (!s) return NULL;
            ast_list_push(p->arena, &d->as.do_.exprs, s);
            while (at(p, TOK_NEWLINE)) advance(p);
        }
        if (!expect(p, TOK_DEDENT, "expected dedent closing suite")) return NULL;
        return d;
    }
    ast_node_t *single = parse_stmt(p);
    while (at(p, TOK_NEWLINE)) advance(p);
    return single;
}

/* ================================================================
 * Top-level decorators
 * ================================================================ */

static bool dec_is(const deck_token_t *t, const char *name)
{
    return t->text && strcmp(t->text, name) == 0;
}

static ast_node_t *parse_app_block(deck_parser_t *p);
static ast_node_t *parse_requires_decl(deck_parser_t *p);
static ast_node_t *parse_use_decl(deck_parser_t *p);
static ast_node_t *parse_on_decl(deck_parser_t *p);
static ast_node_t *parse_machine_decl(deck_parser_t *p);
static ast_node_t *parse_machine_hook_decl(deck_parser_t *p, const char *event);
static ast_node_t *parse_assets_decl(deck_parser_t *p);
static ast_node_t *parse_flow_decl(deck_parser_t *p);
static ast_node_t *parse_migration_decl(deck_parser_t *p);

/* @app
 *   name:    "..."
 *   id:      "..."
 *   version: "..."
 *   edition: 2026
 *
 * Spec 02-deck-app §3: @app is identity-only. Every field is a scalar
 * expression (str/int/list literal). Version contracts live in a sibling
 * `@requires` block (§4A), never nested inside @app. The parser rejects
 * nested blocks inside @app with a clear spec pointer so authors migrate.
 */
static bool parse_scalar_fields(deck_parser_t   *p,
                                ast_app_field_t **out,
                                uint32_t         *out_n,
                                const char       *owner);
static bool parse_requires_fields(deck_parser_t   *p,
                                  ast_app_field_t **out,
                                  uint32_t         *out_n);

static ast_node_t *parse_app_block(deck_parser_t *p)
{
    ast_node_t *n = mknode(p, AST_APP); if (!n) return NULL;
    advance(p); /* @app */
    if (!expect(p, TOK_NEWLINE, "expected newline after @app")) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);
    if (!expect(p, TOK_INDENT, "expected indented @app body")) return NULL;
    if (!parse_scalar_fields(p, &n->as.app.fields, &n->as.app.n_fields, "@app")) return NULL;
    if (!expect(p, TOK_DEDENT, "expected dedent closing @app")) return NULL;
    return n;
}

/* @requires
 *   deck_level: 2
 *   deck_os:    ">= 2"
 *   runtime:    ">= 1.0"
 *   capabilities:
 *     network.http: ">= 2"
 *     storage.local: "any"
 *
 * Top-level per 02-deck-app §4A. Fields may be scalars or a single-level
 * nested block (`capabilities:`). The parser produces an AST_REQUIRES
 * node reusing AST_APP's ast_app_field_t layout; the nested block is
 * stored as a child AST_REQUIRES value. The loader reads whichever
 * arrangement the source uses. */
static ast_node_t *parse_requires_decl(deck_parser_t *p)
{
    ast_node_t *n = mknode(p, AST_REQUIRES); if (!n) return NULL;
    advance(p); /* @requires */
    if (!expect(p, TOK_NEWLINE, "expected newline after @requires")) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);
    if (!expect(p, TOK_INDENT, "expected indented @requires body")) return NULL;
    if (!parse_requires_fields(p, &n->as.app.fields, &n->as.app.n_fields)) return NULL;
    if (!expect(p, TOK_DEDENT, "expected dedent closing @requires")) return NULL;
    return n;
}

static bool parse_scalar_fields(deck_parser_t    *p,
                                ast_app_field_t **out,
                                uint32_t         *out_n,
                                const char       *owner)
{
    ast_app_field_t buf[32];
    uint32_t n = 0;
    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF) && n < 32) {
        if (!at(p, TOK_IDENT)) {
            set_err(p, DECK_LOAD_PARSE_ERROR, "expected field name");
            return false;
        }
        const char *name = p->cur.text;
        advance(p);
        if (!expect(p, TOK_COLON, "expected ':' after field name")) return false;
        if (at(p, TOK_NEWLINE)) {
            /* Reject nested blocks — spec §3 @app is identity-only.
             * The common mistake is writing `requires:` nested inside
             * @app; point authors at §4A's top-level @requires form. */
            if (strcmp(owner, "@app") == 0 &&
                strcmp(name, "requires") == 0) {
                set_err(p, DECK_LOAD_PARSE_ERROR,
                        "`requires:` must be a top-level `@requires` "
                        "annotation (see 02-deck-app §4A), not a nested "
                        "field inside @app");
            } else {
                set_err(p, DECK_LOAD_PARSE_ERROR,
                        "nested blocks are not allowed in this context");
            }
            return false;
        }
        ast_node_t *val = parse_expr_prec(p, 0);
        if (!val) return false;
        while (at(p, TOK_NEWLINE)) advance(p);
        buf[n].name  = name;
        buf[n].value = val;
        n++;
    }
    *out   = deck_arena_memdup(p->arena, buf, n * sizeof(ast_app_field_t));
    *out_n = n;
    return true;
}

static bool parse_requires_fields(deck_parser_t   *p,
                                  ast_app_field_t **out,
                                  uint32_t         *out_n)
{
    ast_app_field_t buf[32];
    uint32_t n = 0;
    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF) && n < 32) {
        if (!at(p, TOK_IDENT)) {
            set_err(p, DECK_LOAD_PARSE_ERROR, "expected field name in @requires");
            return false;
        }
        const char *name = p->cur.text;
        advance(p);
        /* Support dotted keys for capability paths like `network.http:`. */
        char scratch[128];
        uint32_t k = (uint32_t)snprintf(scratch, sizeof(scratch), "%s", name);
        while (at(p, TOK_DOT)) {
            advance(p);
            if (!at(p, TOK_IDENT)) {
                set_err(p, DECK_LOAD_PARSE_ERROR,
                        "expected ident after '.' in @requires key");
                return false;
            }
            if (k < sizeof(scratch) - 1) scratch[k++] = '.';
            k += (uint32_t)snprintf(scratch + k, sizeof(scratch) - k,
                                    "%s", p->cur.text);
            advance(p);
        }
        const char *full_name = deck_intern(scratch, k);
        if (!expect(p, TOK_COLON, "expected ':' after @requires field name")) return false;
        ast_node_t *val = NULL;
        if (at(p, TOK_NEWLINE)) {
            /* Nested block (capabilities:). One level of nesting only. */
            advance(p);
            while (at(p, TOK_NEWLINE)) advance(p);
            if (!expect(p, TOK_INDENT, "expected indented nested block in @requires")) return false;
            ast_node_t *nested = ast_new(p->arena, AST_REQUIRES,
                                         p->cur.line, p->cur.col);
            if (!nested) return false;
            if (!parse_requires_fields(p, &nested->as.app.fields,
                                       &nested->as.app.n_fields)) return false;
            if (!expect(p, TOK_DEDENT, "expected dedent in @requires nested block")) return false;
            val = nested;
        } else {
            val = parse_expr_prec(p, 0);
            if (!val) return false;
            while (at(p, TOK_NEWLINE)) advance(p);
        }
        buf[n].name  = full_name;
        buf[n].value = val;
        n++;
    }
    *out   = deck_arena_memdup(p->arena, buf, n * sizeof(ast_app_field_t));
    *out_n = n;
    return true;
}

/* Spec 02-deck-app §4 — `@use` is a block annotation. Its body holds
 * one entry per line:
 *   capability.path  as alias
 *   capability.path  as alias   optional
 *   ./relative/path
 *
 * `@use.optional` (DL2 F23.4) is a vestigial decorator form where the
 * block-wide `optional` flag applies to every entry. Accepted for
 * backwards compat but prefer the per-entry `optional` trailer.
 *
 * Returns one AST_USE node whose `entries` array holds every binding
 * declared in the block. When the block has a single entry, the
 * top-level mirror fields (`module`/`alias`/`is_optional`) are also
 * populated so older single-binding walkers keep working.
 */
static bool parse_dotted_or_relative(deck_parser_t *p, char *scratch,
                                     size_t cap, uint32_t *out_k);
static const char *default_alias_of(const char *module);

static ast_node_t *parse_use_decl(deck_parser_t *p)
{
    ast_node_t *n = mknode(p, AST_USE); if (!n) return NULL;
    bool block_wide_optional =
        (p->cur.text && strcmp(p->cur.text, "use.optional") == 0);
    advance(p); /* @use or @use.optional */

    if (!expect(p, TOK_NEWLINE, "expected newline after @use; spec §4 "
                                "requires a block body with one entry "
                                "per line")) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);
    if (!expect(p, TOK_INDENT, "expected indented @use body")) return NULL;

    ast_use_entry_t buf[32];
    uint32_t n_entries = 0;

    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF) && n_entries < 32) {
        char scratch[160];
        uint32_t k = 0;
        if (!parse_dotted_or_relative(p, scratch, sizeof(scratch), &k)) return NULL;
        const char *module = deck_intern(scratch, k);

        const char *alias = NULL;
        bool is_optional = block_wide_optional;

        /* Optional `as alias` clause. */
        if (at(p, TOK_IDENT) && p->cur.text && strcmp(p->cur.text, "as") == 0) {
            advance(p);
            if (!at(p, TOK_IDENT)) {
                set_err(p, DECK_LOAD_PARSE_ERROR,
                        "expected alias after `as` in @use entry");
                return NULL;
            }
            alias = deck_intern(p->cur.text, (uint32_t)strlen(p->cur.text));
            advance(p);
        }

        /* Optional trailing `optional` keyword. */
        if (at(p, TOK_IDENT) && p->cur.text &&
            strcmp(p->cur.text, "optional") == 0) {
            is_optional = true;
            advance(p);
        }

        /* `when: condition_expr` — parsed and discarded for now.
         * `when` is TOK_KW_WHEN in the lexer, so match the keyword
         * token rather than a bare identifier. Runtime gating by
         * `when:` is a post-DL1 feature; the spec allows it and the
         * loader records it as graceful-degradation optional. */
        if (at(p, TOK_KW_WHEN)) {
            advance(p);
            if (!expect(p, TOK_COLON, "expected ':' after `when` in @use entry")) return NULL;
            ast_node_t *cond = parse_expr_prec(p, 0);
            if (!cond) return NULL;
            (void)cond;   /* parsed for syntax; runtime gating is post-DL1. */
            is_optional = true;
        }

        while (at(p, TOK_NEWLINE)) advance(p);

        if (!alias) alias = default_alias_of(module);

        buf[n_entries].module      = module;
        buf[n_entries].alias       = alias;
        buf[n_entries].is_optional = is_optional;
        n_entries++;
    }

    if (!expect(p, TOK_DEDENT, "expected dedent closing @use block")) return NULL;

    if (n_entries == 0) {
        set_err(p, DECK_LOAD_PARSE_ERROR,
                "@use block is empty; spec §4 requires at least one entry");
        return NULL;
    }

    n->as.use.entries   = deck_arena_memdup(p->arena, buf,
                                            n_entries * sizeof(ast_use_entry_t));
    n->as.use.n_entries = n_entries;
    n->as.use.module      = (n_entries == 1) ? buf[0].module      : NULL;
    n->as.use.alias       = (n_entries == 1) ? buf[0].alias       : NULL;
    n->as.use.is_optional = (n_entries == 1) ? buf[0].is_optional : false;
    return n;
}

/* Accepts a dotted capability path (`a.b.c`) in an @use entry.
 *
 * Spec 02-deck-app §4.2 also allows relative local-module paths
 * (`./relative/path`) but the current runtime does not resolve those;
 * attempting to use one yields a clear load-time error. */
static bool parse_dotted_or_relative(deck_parser_t *p, char *scratch,
                                     size_t cap, uint32_t *out_k)
{
    uint32_t k = 0;
    if (at(p, TOK_DOT)) {
        set_err(p, DECK_LOAD_PARSE_ERROR,
                "relative module paths in @use (spec §4.2) are not yet "
                "supported by this runtime; use dotted capability paths");
        return false;
    }
    if (!at(p, TOK_IDENT)) {
        set_err(p, DECK_LOAD_PARSE_ERROR,
                "expected capability path in @use entry");
        return false;
    }
    k += (uint32_t)snprintf(scratch + k, cap - k, "%s", p->cur.text);
    advance(p);
    while (at(p, TOK_DOT)) {
        advance(p);
        if (!at(p, TOK_IDENT)) {
            set_err(p, DECK_LOAD_PARSE_ERROR,
                    "expected ident after '.' in @use path");
            return false;
        }
        if (k < cap - 1) scratch[k++] = '.';
        k += (uint32_t)snprintf(scratch + k, cap - k, "%s", p->cur.text);
        advance(p);
    }
    *out_k = k;
    return true;
}

static const char *default_alias_of(const char *module)
{
    if (!module) return NULL;
    const char *dot = strrchr(module, '.');
    const char *slash = strrchr(module, '/');
    const char *tail = dot > slash ? dot : slash;
    if (!tail) return module;
    return deck_intern(tail + 1, (uint32_t)strlen(tail + 1));
}

static ast_node_t *parse_on_decl(deck_parser_t *p)
{
    ast_node_t *n = mknode(p, AST_ON); if (!n) return NULL;
    advance(p); /* @on */
    /* event is an identifier (launch/resume/terminate etc). */
    if (at(p, TOK_IDENT)) {
        n->as.on.event = p->cur.text; advance(p);
    } else {
        set_err(p, DECK_LOAD_PARSE_ERROR, "expected event name after @on");
        return NULL;
    }
    if (!expect(p, TOK_COLON, "expected ':' after @on event")) return NULL;
    n->as.on.body = parse_suite(p);
    if (!n->as.on.body) return NULL;
    return n;
}

/* DL2 F28.1 — `@machine.before` / `@machine.after` parse as AST_ON with a
 * reserved event name ("__machine_before" / "__machine_after"). The runtime
 * machine loop (run_machine) looks these up and runs the body around each
 * transition. The leading "__" prefix prevents collision with user-defined
 * @on events, which are all bare identifiers. */
static ast_node_t *parse_machine_hook_decl(deck_parser_t *p, const char *event)
{
    ast_node_t *n = mknode(p, AST_ON); if (!n) return NULL;
    advance(p); /* @machine.before or @machine.after */
    /* Optional newline(s) then the body. Some sources use ':' directly, some
     * skip it — accept both to be permissive with hand-written code. */
    if (at(p, TOK_COLON)) advance(p);
    n->as.on.event = deck_intern_cstr(event);
    n->as.on.body  = parse_suite(p);
    if (!n->as.on.body) return NULL;
    return n;
}

/* DL2 F28.5 — `@assets` block.
 *
 *   @assets
 *     icon: "icon.png"
 *     font: "mono.ttf"
 *
 * Each line binds a name (ident) to a path (string literal). The parser
 * collects the pairs into an AST_ASSETS node; `asset.path(name)` walks
 * these at call time. Max 32 entries per block (sufficient for a typical
 * DL2 app; DL3 may raise this if needed). */
#define DECK_ASSETS_MAX   32
static ast_node_t *parse_assets_decl(deck_parser_t *p)
{
    ast_node_t *n = mknode(p, AST_ASSETS); if (!n) return NULL;
    advance(p); /* @assets */
    if (!expect(p, TOK_NEWLINE, "expected newline after @assets")) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);
    if (!expect(p, TOK_INDENT, "expected indented @assets body")) return NULL;

    const char *names[DECK_ASSETS_MAX];
    const char *paths[DECK_ASSETS_MAX];
    uint32_t    k = 0;

    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
        if (k >= DECK_ASSETS_MAX) {
            set_err(p, DECK_LOAD_PARSE_ERROR, "too many @assets entries (max 32)");
            return NULL;
        }
        if (!at(p, TOK_IDENT)) {
            set_err(p, DECK_LOAD_PARSE_ERROR, "expected asset name");
            return NULL;
        }
        names[k] = p->cur.text;
        advance(p);
        if (!expect(p, TOK_COLON, "expected ':' after asset name")) return NULL;
        if (!at(p, TOK_STRING)) {
            set_err(p, DECK_LOAD_PARSE_ERROR, "expected string literal path for asset");
            return NULL;
        }
        paths[k] = p->cur.text;
        advance(p);
        k++;
        while (at(p, TOK_NEWLINE)) advance(p);
    }
    if (!expect(p, TOK_DEDENT, "expected dedent closing @assets body")) return NULL;

    if (k > 0) {
        n->as.assets.names = deck_arena_memdup(p->arena, names, k * sizeof(char *));
        n->as.assets.paths = deck_arena_memdup(p->arena, paths, k * sizeof(char *));
        if (!n->as.assets.names || !n->as.assets.paths) return NULL;
    }
    n->as.assets.n_entries = k;
    return n;
}

/* DL2 F28.2 — `@flow name` desugars at parse time into an AST_MACHINE.
 *
 *   @flow signup
 *     step welcome:
 *       log.info("hi")
 *     step collect:
 *       log.info("email")
 *     step done:
 *       log.info("ok")
 *
 * produces the equivalent of:
 *
 *   @machine signup
 *     state welcome:
 *       on enter:
 *         log.info("hi")
 *       transition :collect
 *     state collect:
 *       on enter:
 *         log.info("email")
 *       transition :done
 *     state done:
 *       on enter:
 *         log.info("ok")
 *
 * The last step carries no transition and the machine terminates in it.
 * Steps see the machine name + each other via the normal @machine runtime
 * (enter hooks, before/after machine hooks also apply). */
#define DECK_FLOW_MAX_STEPS  32
static ast_node_t *parse_flow_decl(deck_parser_t *p)
{
    ast_node_t *m = mknode(p, AST_MACHINE); if (!m) return NULL;
    advance(p); /* @flow */
    if (!at(p, TOK_IDENT)) { set_err(p, DECK_LOAD_PARSE_ERROR, "expected flow name"); return NULL; }
    m->as.machine.name = p->cur.text;
    advance(p);
    /* Allow optional colon for symmetry with @machine syntax. */
    if (at(p, TOK_COLON)) advance(p);
    if (!expect(p, TOK_NEWLINE, "expected newline after @flow name")) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);
    if (!expect(p, TOK_INDENT, "expected indented @flow body")) return NULL;
    ast_list_init(&m->as.machine.states);

    /* Remember each step's body so we can attach the transition to the
     * next step only after we know its name. */
    typedef struct { ast_node_t *state; } step_ref_t;
    step_ref_t refs[DECK_FLOW_MAX_STEPS];
    uint32_t   n_steps = 0;

    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
        if (n_steps >= DECK_FLOW_MAX_STEPS) {
            set_err(p, DECK_LOAD_PARSE_ERROR, "too many @flow steps (max 32)");
            return NULL;
        }
        /* `step <name>:` */
        if (!at(p, TOK_IDENT) || strcmp(p->cur.text, "step") != 0) {
            set_err(p, DECK_LOAD_PARSE_ERROR,
                    "expected 'step' in @flow body");
            return NULL;
        }
        advance(p); /* step */
        if (!at(p, TOK_IDENT)) {
            set_err(p, DECK_LOAD_PARSE_ERROR, "expected step name"); return NULL;
        }
        ast_node_t *state = ast_new(p->arena, AST_STATE, p->cur.line, p->cur.col);
        if (!state) return NULL;
        state->as.state.name = p->cur.text;
        ast_list_init(&state->as.state.hooks);
        advance(p);
        if (!expect(p, TOK_COLON, "expected ':' after step name")) return NULL;

        /* Body becomes the `on enter` hook. */
        ast_node_t *hook = ast_new(p->arena, AST_STATE_HOOK, p->cur.line, p->cur.col);
        if (!hook) return NULL;
        hook->as.state_hook.kind = deck_intern_cstr("enter");
        hook->as.state_hook.body = parse_suite(p);
        if (!hook->as.state_hook.body) return NULL;
        ast_list_push(p->arena, &state->as.state.hooks, hook);

        ast_list_push(p->arena, &m->as.machine.states, state);
        refs[n_steps++].state = state;
        while (at(p, TOK_NEWLINE)) advance(p);
    }
    if (!expect(p, TOK_DEDENT, "expected dedent closing @flow body")) return NULL;
    if (n_steps == 0) {
        set_err(p, DECK_LOAD_PARSE_ERROR, "@flow must have at least one step");
        return NULL;
    }

    /* Wire auto-transitions: step[i] → step[i+1]. Last step terminates. */
    for (uint32_t i = 0; i + 1 < n_steps; i++) {
        ast_node_t *tr = ast_new(p->arena, AST_TRANSITION,
                                  refs[i].state->line, refs[i].state->col);
        if (!tr) return NULL;
        tr->as.transition.target = refs[i + 1].state->as.state.name;
        ast_list_push(p->arena, &refs[i].state->as.state.hooks, tr);
    }
    return m;
}

/* DL2 F28.4 — `@migration from N: <body>` blocks.
 *
 *   @migration
 *     from 0:
 *       log.info("0 → 1")
 *     from 1:
 *       nvs.set("app", "schema", "v2")
 *
 * Each `from N:` entry attaches a body that runs when the app was last
 * observed at version N. The runtime reads the stored version from NVS
 * at load time (see deck_runtime_app_load), runs every `from K >= stored`
 * body in ascending order of K, then stores `max(K) + 1`. App version
 * is tracked as a simple int (not the semver string in @app.version).
 *
 * Max 16 entries per block — 16 migration steps is plenty for any app
 * that ships over a reasonable lifetime. */
#define DECK_MIGRATION_MAX   16
static ast_node_t *parse_migration_decl(deck_parser_t *p)
{
    ast_node_t *n = mknode(p, AST_MIGRATION); if (!n) return NULL;
    advance(p); /* @migration */
    if (!expect(p, TOK_NEWLINE, "expected newline after @migration")) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);
    if (!expect(p, TOK_INDENT, "expected indented @migration body")) return NULL;

    int64_t      versions[DECK_MIGRATION_MAX];
    ast_node_t  *bodies[DECK_MIGRATION_MAX];
    uint32_t     k = 0;

    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
        if (k >= DECK_MIGRATION_MAX) {
            set_err(p, DECK_LOAD_PARSE_ERROR, "too many @migration entries (max 16)");
            return NULL;
        }
        if (!at(p, TOK_IDENT) || strcmp(p->cur.text, "from") != 0) {
            set_err(p, DECK_LOAD_PARSE_ERROR, "expected 'from' in @migration body");
            return NULL;
        }
        advance(p); /* from */
        if (!at(p, TOK_INT)) {
            set_err(p, DECK_LOAD_PARSE_ERROR, "expected integer version after 'from'");
            return NULL;
        }
        versions[k] = p->cur.as.i;
        advance(p);
        if (!expect(p, TOK_COLON, "expected ':' after 'from N'")) return NULL;
        bodies[k] = parse_suite(p);
        if (!bodies[k]) return NULL;
        k++;
        while (at(p, TOK_NEWLINE)) advance(p);
    }
    if (!expect(p, TOK_DEDENT, "expected dedent closing @migration body")) return NULL;

    if (k > 0) {
        n->as.migration.from_versions = deck_arena_memdup(p->arena, versions, k * sizeof(int64_t));
        n->as.migration.bodies        = deck_arena_memdup(p->arena, bodies,   k * sizeof(ast_node_t *));
        if (!n->as.migration.from_versions || !n->as.migration.bodies) return NULL;
    }
    n->as.migration.n_entries = k;
    return n;
}

static ast_node_t *parse_state_decl(deck_parser_t *p)
{
    ast_node_t *st = mknode(p, AST_STATE); if (!st) return NULL;
    advance(p); /* state */
    if (!at(p, TOK_IDENT)) { set_err(p, DECK_LOAD_PARSE_ERROR, "expected state name"); return NULL; }
    st->as.state.name = p->cur.text;
    advance(p);
    if (!expect(p, TOK_COLON, "expected ':' after state name")) return NULL;
    if (!expect(p, TOK_NEWLINE, "expected newline after state ':'")) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);
    if (!expect(p, TOK_INDENT, "expected indented state body")) return NULL;
    ast_list_init(&st->as.state.hooks);

    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
        if (at(p, TOK_KW_ON)) {
            advance(p);
            if (!at(p, TOK_KW_ENTER) && !at(p, TOK_KW_LEAVE)) {
                set_err(p, DECK_LOAD_PARSE_ERROR, "expected 'enter' or 'leave'"); return NULL;
            }
            const char *kind = at(p, TOK_KW_ENTER) ? "enter" : "leave";
            advance(p);
            if (!expect(p, TOK_COLON, "expected ':' after on enter/leave")) return NULL;
            ast_node_t *hook = ast_new(p->arena, AST_STATE_HOOK, p->cur.line, p->cur.col);
            if (!hook) return NULL;
            hook->as.state_hook.kind = deck_intern_cstr(kind);
            hook->as.state_hook.body = parse_suite(p);
            if (!hook->as.state_hook.body) return NULL;
            ast_list_push(p->arena, &st->as.state.hooks, hook);
        } else if (at(p, TOK_KW_TRANSITION)) {
            ast_node_t *tr = parse_transition_stmt(p);
            if (!tr) return NULL;
            while (at(p, TOK_NEWLINE)) advance(p);
            ast_list_push(p->arena, &st->as.state.hooks, tr);
        } else {
            set_err(p, DECK_LOAD_PARSE_ERROR, "expected on/transition in state body");
            return NULL;
        }
    }
    if (!expect(p, TOK_DEDENT, "expected dedent closing state body")) return NULL;
    return st;
}

static ast_node_t *parse_machine_decl(deck_parser_t *p)
{
    ast_node_t *m = mknode(p, AST_MACHINE); if (!m) return NULL;
    advance(p); /* @machine */
    if (!at(p, TOK_IDENT)) { set_err(p, DECK_LOAD_PARSE_ERROR, "expected machine name"); return NULL; }
    m->as.machine.name = p->cur.text;
    advance(p);
    if (!expect(p, TOK_NEWLINE, "expected newline after machine name")) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);
    if (!expect(p, TOK_INDENT, "expected indented machine body")) return NULL;
    ast_list_init(&m->as.machine.states);
    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
        if (!at(p, TOK_KW_STATE)) {
            set_err(p, DECK_LOAD_PARSE_ERROR, "expected 'state' in machine body");
            return NULL;
        }
        ast_node_t *st = parse_state_decl(p); if (!st) return NULL;
        ast_list_push(p->arena, &m->as.machine.states, st);
        while (at(p, TOK_NEWLINE)) advance(p);
    }
    if (!expect(p, TOK_DEDENT, "expected dedent closing machine")) return NULL;
    return m;
}

/* ================================================================
 * Module entry point
 * ================================================================ */

/* DL2 F21.1: parse `fn name (p1, p2: T) -> T = body`.
 * F21.2 makes the name optional — `fn (params) = body` becomes a lambda
 * value usable in any expression position (let RHS, call argument, etc).
 * Type annotations and effect annotations are accepted but discarded at
 * F21.1; F22 lands the type system, F23 effect checks. */
static ast_node_t *parse_fn_decl(deck_parser_t *p)
{
    ast_node_t *n = mknode(p, AST_FN_DEF); if (!n) return NULL;
    advance(p); /* fn */
    if (at(p, TOK_LPAREN)) {
        n->as.fndef.name = NULL;   /* anonymous (F21.2 lambda) */
    } else if (at(p, TOK_IDENT)) {
        n->as.fndef.name = p->cur.text;
        advance(p);
    } else {
        set_err(p, DECK_LOAD_PARSE_ERROR,
                "expected function name or '(' after 'fn'");
        return NULL;
    }
    if (!expect(p, TOK_LPAREN, "expected '(' after fn name")) return NULL;

    const char *params_buf[16];
    uint32_t n_params = 0;
    if (!at(p, TOK_RPAREN)) {
        for (;;) {
            if (n_params >= 16) {
                set_err(p, DECK_LOAD_PARSE_ERROR, "too many parameters (max 16)");
                return NULL;
            }
            if (!at(p, TOK_IDENT)) {
                set_err(p, DECK_LOAD_PARSE_ERROR, "expected parameter name");
                return NULL;
            }
            params_buf[n_params++] = p->cur.text;
            advance(p);
            /* Optional ': Type' — a single ident type at F21.1, ignored. */
            if (at(p, TOK_COLON)) {
                advance(p);
                if (!at(p, TOK_IDENT)) {
                    set_err(p, DECK_LOAD_PARSE_ERROR, "expected type name after ':'");
                    return NULL;
                }
                advance(p);
            }
            if (!at(p, TOK_COMMA)) break;
            advance(p);
        }
    }
    if (!expect(p, TOK_RPAREN, "expected ')' after fn parameters")) return NULL;

    /* Optional `-> Type`. Single ident type at F21.1, ignored. */
    if (at(p, TOK_ARROW)) {
        advance(p);
        if (!at(p, TOK_IDENT)) {
            set_err(p, DECK_LOAD_PARSE_ERROR, "expected return type after '->'");
            return NULL;
        }
        advance(p);
    }

    /* DL2 F23: collect effect annotations `!alias` for loader to enforce
     * against @use declarations. */
    const char *effects_buf[8];
    uint32_t n_effects = 0;
    while (at(p, TOK_BANG)) {
        advance(p);
        if (!at(p, TOK_IDENT)) {
            set_err(p, DECK_LOAD_PARSE_ERROR, "expected effect alias after '!'");
            return NULL;
        }
        if (n_effects >= 8) {
            set_err(p, DECK_LOAD_PARSE_ERROR, "too many effect annotations (max 8)");
            return NULL;
        }
        effects_buf[n_effects++] = p->cur.text;
        advance(p);
    }

    if (!expect(p, TOK_ASSIGN, "expected '=' in fn declaration")) return NULL;

    /* Body: indented suite (NEWLINE INDENT ... DEDENT) or a single inline expr. */
    ast_node_t *body = parse_suite(p);
    if (!body) return NULL;

    n->as.fndef.params   = deck_arena_memdup(p->arena, params_buf,
                                             n_params * sizeof(char *));
    n->as.fndef.n_params = n_params;
    n->as.fndef.effects  = n_effects > 0
                          ? deck_arena_memdup(p->arena, effects_buf,
                                              n_effects * sizeof(char *))
                          : NULL;
    n->as.fndef.n_effects = n_effects;
    n->as.fndef.body     = body;
    return n;
}

/* DL2 F23.6 / F23.7 — `@permissions` and `@errors` are documented as
 * indented blocks of `key: value` entries. F23 minimum: parse and
 * discard (metadata for future shell prompts / runtime cataloging). */
/* DL2 F28 — opaque indented block: consumes tokens until matching DEDENT,
 * returns a benign stub. Used for decorators whose body is not
 * interpreted today (@machine.before/.after, @flow, @flow.step,
 * @migration, @assets). The decorator name is logged for the loader
 * to surface in `info` / debug. Runtime semantics land post-DL2. */
static ast_node_t *parse_opaque_block(deck_parser_t *p)
{
    advance(p); /* the @decorator */
    /* Some opaque blocks may have an inline name token (e.g. @flow user_signup). */
    while (at(p, TOK_IDENT) || at(p, TOK_DOT)) advance(p);
    if (!expect(p, TOK_NEWLINE, "expected newline after opaque decorator")) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);
    if (!at(p, TOK_INDENT)) {
        /* No body — accept empty. */
        ast_node_t *stub = mknode(p, AST_USE);
        if (stub) { stub->as.use.module = "__metadata"; stub->as.use.is_optional = true; }
        return stub;
    }
    advance(p); /* INDENT */
    int depth = 1;
    while (depth > 0 && !at(p, TOK_EOF)) {
        if (at(p, TOK_INDENT))      { depth++; advance(p); }
        else if (at(p, TOK_DEDENT)) { depth--; advance(p); }
        else                         { advance(p); }
    }
    ast_node_t *stub = mknode(p, AST_USE);
    if (stub) { stub->as.use.module = "__metadata"; stub->as.use.is_optional = true; }
    return stub;
}

static ast_node_t *parse_metadata_block(deck_parser_t *p)
{
    advance(p); /* @permissions or @errors */
    if (!expect(p, TOK_NEWLINE, "expected newline after metadata decorator")) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);
    if (!expect(p, TOK_INDENT, "expected indented metadata body")) return NULL;
    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
        if (!at(p, TOK_IDENT)) {
            set_err(p, DECK_LOAD_PARSE_ERROR, "expected name in metadata block");
            return NULL;
        }
        advance(p);
        /* Dotted keys like `fs.write`, `network.http`. */
        while (at(p, TOK_DOT)) {
            advance(p);
            if (!at(p, TOK_IDENT)) {
                set_err(p, DECK_LOAD_PARSE_ERROR, "expected ident after '.' in metadata key");
                return NULL;
            }
            advance(p);
        }
        if (!expect(p, TOK_COLON, "expected ':' in metadata entry")) return NULL;
        /* Value: skip the rest of the line (one expr or simple ident). */
        if (at(p, TOK_NEWLINE)) {
            /* allow empty / nested block — eat it */
            advance(p);
            while (at(p, TOK_NEWLINE)) advance(p);
            if (at(p, TOK_INDENT)) {
                advance(p);
                while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) advance(p);
                if (!expect(p, TOK_DEDENT, "expected dedent")) return NULL;
            }
            continue;
        }
        ast_node_t *v = parse_expr_prec(p, 0);
        if (!v) return NULL;
        while (at(p, TOK_NEWLINE)) advance(p);
    }
    if (!expect(p, TOK_DEDENT, "expected dedent closing metadata block")) return NULL;
    /* Return a benign AST_USE node so module item list stays well-formed. */
    ast_node_t *stub = mknode(p, AST_USE);
    if (stub) {
        stub->as.use.module = "__metadata";
        stub->as.use.is_optional = true;
    }
    return stub;
}

/* DL2 F22.2 — `@type Name`<NEWLINE><INDENT> field: TypeName ... <DEDENT>.
 * Type annotations on fields are parsed and discarded (the runtime is
 * dynamic). Union types `T1 | T2` are also accepted and discarded. */
static ast_node_t *parse_type_decl(deck_parser_t *p)
{
    advance(p); /* @type */
    if (!at(p, TOK_IDENT)) {
        set_err(p, DECK_LOAD_PARSE_ERROR, "expected type name after @type");
        return NULL;
    }
    ast_node_t *n = mknode(p, AST_TYPE_DEF); if (!n) return NULL;
    n->as.typedef_.name = p->cur.text;
    advance(p);
    if (!expect(p, TOK_NEWLINE, "expected newline after @type name")) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);
    if (!expect(p, TOK_INDENT, "expected indented @type body")) return NULL;

    const char *fields[32];
    uint32_t nf = 0;
    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
        if (nf >= 32) { set_err(p, DECK_LOAD_PARSE_ERROR, "too many fields (max 32)"); return NULL; }
        if (!at(p, TOK_IDENT)) { set_err(p, DECK_LOAD_PARSE_ERROR, "expected field name"); return NULL; }
        fields[nf++] = p->cur.text;
        advance(p);
        if (!expect(p, TOK_COLON, "expected ':' after field name")) return NULL;
        /* Type annotation: IDENT (`|` IDENT)* — F22.3 union types parsed
         * and discarded for now (no static type system yet). */
        if (!at(p, TOK_IDENT)) { set_err(p, DECK_LOAD_PARSE_ERROR, "expected type name"); return NULL; }
        advance(p);
        while (at(p, TOK_OR_OR) || at(p, TOK_PIPE)) {
            advance(p);
            if (!at(p, TOK_IDENT)) {
                set_err(p, DECK_LOAD_PARSE_ERROR, "expected type after '|'");
                return NULL;
            }
            advance(p);
        }
        while (at(p, TOK_NEWLINE)) advance(p);
    }
    if (!expect(p, TOK_DEDENT, "expected dedent closing @type body")) return NULL;
    n->as.typedef_.fields   = nf > 0 ? deck_arena_memdup(p->arena, fields, nf * sizeof(char *)) : NULL;
    n->as.typedef_.n_fields = nf;
    return n;
}

static ast_node_t *parse_top_item(deck_parser_t *p)
{
    if (at(p, TOK_KW_FN)) {
        ast_node_t *fnn = parse_fn_decl(p);
        if (!fnn) return NULL;
        if (!fnn->as.fndef.name) {
            set_err(p, DECK_LOAD_PARSE_ERROR,
                    "anonymous fn (lambda) is not allowed at top level");
            return NULL;
        }
        return fnn;
    }
    if (at(p, TOK_DECORATOR)) {
        if      (dec_is(&p->cur, "app"))            return parse_app_block(p);
        else if (dec_is(&p->cur, "requires"))       return parse_requires_decl(p); /* 02-deck-app §4A */
        else if (dec_is(&p->cur, "use"))            return parse_use_decl(p);
        else if (dec_is(&p->cur, "use.optional"))   return parse_use_decl(p);   /* DL2 F23.4 */
        else if (dec_is(&p->cur, "on"))             return parse_on_decl(p);
        else if (dec_is(&p->cur, "machine"))        return parse_machine_decl(p);
        else if (dec_is(&p->cur, "machine.before")) return parse_machine_hook_decl(p, "__machine_before");  /* F28.1 */
        else if (dec_is(&p->cur, "machine.after"))  return parse_machine_hook_decl(p, "__machine_after");   /* F28.1 */
        else if (dec_is(&p->cur, "flow"))           return parse_flow_decl(p);     /* F28.2 */
        else if (dec_is(&p->cur, "flow.step"))      return parse_opaque_block(p);  /* F28.2 — unused outside @flow */
        else if (dec_is(&p->cur, "migration"))      return parse_migration_decl(p); /* F28.4 */
        else if (dec_is(&p->cur, "assets"))         return parse_assets_decl(p);   /* F28.5 */
        else if (dec_is(&p->cur, "type"))           return parse_type_decl(p);
        else if (dec_is(&p->cur, "permissions"))    return parse_metadata_block(p);  /* F23.6 */
        else if (dec_is(&p->cur, "errors"))         return parse_metadata_block(p);  /* F23.7 */
        else if (dec_is(&p->cur, "private")) {
            /* DL2 F22.9 — @private prefixes a fn declaration. */
            advance(p);
            while (at(p, TOK_NEWLINE)) advance(p);
            if (!at(p, TOK_KW_FN)) {
                set_err(p, DECK_LOAD_PARSE_ERROR, "@private must precede a fn declaration");
                return NULL;
            }
            ast_node_t *fnn = parse_fn_decl(p);
            if (!fnn) return NULL;
            if (!fnn->as.fndef.name) {
                set_err(p, DECK_LOAD_PARSE_ERROR, "@private cannot precede an anonymous fn");
                return NULL;
            }
            fnn->as.fndef.is_private = true;
            return fnn;
        }
        set_err(p, DECK_LOAD_PARSE_ERROR, "unknown top-level decorator");
        return NULL;
    }
    set_err(p, DECK_LOAD_PARSE_ERROR, "expected @app, @use, @on, @machine, or fn at top level");
    return NULL;
}

ast_node_t *deck_parser_parse_module(deck_parser_t *p)
{
    if (!p) return NULL;
    ast_node_t *mod = mknode(p, AST_MODULE);
    if (!mod) return NULL;
    ast_list_init(&mod->as.module.items);

    skip_newlines(p);
    while (!at(p, TOK_EOF) && p->err == DECK_LOAD_OK) {
        ast_node_t *item = parse_top_item(p);
        if (!item) return NULL;
        ast_list_push(p->arena, &mod->as.module.items, item);
        skip_newlines(p);
    }
    return mod;
}

/* ================================================================
 * Init + error accessors
 * ================================================================ */

void deck_parser_init(deck_parser_t *p, deck_arena_t *arena,
                      const char *src, uint32_t len)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->arena = arena;
    deck_lexer_init(&p->lx, src, len);
    p->err = DECK_LOAD_OK;
    advance(p); /* load first token into cur */
}

deck_err_t  deck_parser_err_code(const deck_parser_t *p) { return p ? p->err : DECK_LOAD_INTERNAL; }
const char *deck_parser_err_msg(const deck_parser_t *p)  { return p ? p->err_msg : NULL; }
uint32_t    deck_parser_err_line(const deck_parser_t *p) { return p ? p->err_line : 0; }
uint32_t    deck_parser_err_col(const deck_parser_t *p)  { return p ? p->err_col : 0; }
