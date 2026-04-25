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
    if (p->cur.type != t) { set_err(p, DECK_LOAD_PARSE, msg); return false; }
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
static ast_node_t *parse_suite(deck_parser_t *p);
static ast_node_t *parse_stmt(deck_parser_t *p);
static bool        skip_type_annotation(deck_parser_t *p);

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
                    if (tk + 1 >= sizeof(text_buf)) { set_err(p, DECK_LOAD_PARSE, "interp string too long"); return NULL; }
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
                        set_err(p, DECK_LOAD_PARSE, "unterminated interpolation");
                        return NULL;
                    }
                    /* Recursively parse the inner expression source. */
                    deck_parser_t inner;
                    deck_parser_init(&inner, p->arena, src + start, j - start);
                    ast_node_t *expr = deck_parser_parse_expr(&inner);
                    if (!expr || deck_parser_err_code(&inner) != DECK_LOAD_OK) {
                        set_err(p, DECK_LOAD_PARSE,
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
                if (tk + 1 >= sizeof(text_buf)) { set_err(p, DECK_LOAD_PARSE, "interp string too long"); return NULL; }
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
                /* `:ctor -N` — unary-negated numeric payload counts as a
                 * primary (spec §3.7 allows any expression as payload;
                 * in practice fixtures use `:some -7` / `:err -1`). */
                case TOK_MINUS:
                    if (peek_next_tok(p) == TOK_INT ||
                        peek_next_tok(p) == TOK_FLOAT)
                        starts_primary = true;
                    break;
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
             * if the next token is `->`. Suppressed in guard-expression
             * contexts (match-when) so the arrow delimits the arm. */
            if (!p->no_lambda && peek_next_tok(p) == TOK_ARROW) {
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
            /* Spec §4.1 — `TypeName { field: value, … }` record
             * construction. Desugars to a map literal with an extra
             * `__type` entry tagging the type, so field access via
             * `.field` and type_of(x) both Just Work on the resulting
             * map. Heuristic: the ident starts with an uppercase ASCII
             * letter (Deck convention for type names) and the very next
             * token is `{`. Lowercase idents never take this branch so
             * regular variable refs still work. */
            if (peek_next_tok(p) == TOK_LBRACE &&
                p->cur.text && p->cur.text[0] >= 'A' && p->cur.text[0] <= 'Z') {
                uint32_t ln = p->cur.line, co = p->cur.col;
                const char *type_name = p->cur.text;
                advance(p);   /* ident */
                advance(p);   /* { */
                ast_node_t *map = ast_new(p->arena, AST_LIT_MAP, ln, co);
                if (!map) return NULL;
                ast_list_init(&map->as.map_lit.keys);
                ast_list_init(&map->as.map_lit.vals);
                /* Inject `__type: :TypeName` as the first entry. */
                ast_node_t *tk = ast_new(p->arena, AST_LIT_STR, ln, co);
                ast_node_t *tv = ast_new(p->arena, AST_LIT_ATOM, ln, co);
                if (!tk || !tv) return NULL;
                tk->as.s = deck_intern_cstr("__type");
                tv->as.s = type_name;
                ast_list_push(p->arena, &map->as.map_lit.keys, tk);
                ast_list_push(p->arena, &map->as.map_lit.vals, tv);
                if (!at(p, TOK_RBRACE)) {
                    for (;;) {
                        if (!at(p, TOK_IDENT)) {
                            set_err(p, DECK_LOAD_PARSE,
                                    "expected field name in record construction");
                            return NULL;
                        }
                        ast_node_t *k = ast_new(p->arena, AST_LIT_STR,
                                                p->cur.line, p->cur.col);
                        if (!k) return NULL;
                        k->as.s = p->cur.text;
                        advance(p);
                        if (!expect(p, TOK_COLON,
                                    "expected ':' after field name")) return NULL;
                        ast_node_t *v = parse_expr_prec(p, 0);
                        if (!v) return NULL;
                        ast_list_push(p->arena, &map->as.map_lit.keys, k);
                        ast_list_push(p->arena, &map->as.map_lit.vals, v);
                        if (!at(p, TOK_COMMA)) break;
                        advance(p);
                        if (at(p, TOK_RBRACE)) break;
                    }
                }
                if (!expect(p, TOK_RBRACE,
                            "expected '}' closing record construction")) return NULL;
                n = map;
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
            /* Zero-arg lambda `() -> body` (BUILTINS §14 etc. uses
             * `() -> T` thunks). Peek for the empty paren followed by
             * `->`; if matched, build the lambda with zero params. */
            if (at(p, TOK_RPAREN) && peek_next_tok(p) == TOK_ARROW &&
                !p->no_lambda) {
                advance(p); /* ) */
                advance(p); /* -> */
                ast_node_t *body = parse_expr_prec(p, 0);
                if (!body) return NULL;
                ast_node_t *fn = ast_new(p->arena, AST_FN_DEF, ln, co);
                if (!fn) return NULL;
                fn->as.fndef.name     = NULL;
                fn->as.fndef.params   = NULL;
                fn->as.fndef.n_params = 0;
                fn->as.fndef.effects  = NULL;
                fn->as.fndef.n_effects= 0;
                fn->as.fndef.body     = body;
                n = fn;
                break;
            }
            ast_node_t *first = parse_expr_prec(p, 0);
            if (!first) return NULL;
            /* Spec §3.7 named-field atom variant payload: `(name: value, …)`
             * appears after `:ctor` — when `first` parsed as a bare ident
             * followed by `:` with a NON-ident value (literal, expression),
             * treat the whole paren group as a map literal with string keys.
             * The outer TOK_ATOM branch wraps it into `(:ctor, {map})` via
             * the existing atom-variant tupleization path, so match arms
             * like `| :active s -> s.temp + s.max` see `s` as a map and
             * dot access via string key Just Works (see AST_DOT lookup). */
            if (at(p, TOK_COLON) && first && first->kind == AST_IDENT &&
                peek_next_tok(p) != TOK_IDENT) {
                ast_node_t *map = ast_new(p->arena, AST_LIT_MAP, ln, co);
                if (!map) return NULL;
                ast_list_init(&map->as.map_lit.keys);
                ast_list_init(&map->as.map_lit.vals);
                const char *first_name = first->as.s;
                advance(p); /* : */
                ast_node_t *first_val = parse_expr_prec(p, 0);
                if (!first_val) return NULL;
                ast_node_t *first_key = ast_new(p->arena, AST_LIT_STR, ln, co);
                if (!first_key) return NULL;
                first_key->as.s = first_name;
                ast_list_push(p->arena, &map->as.map_lit.keys, first_key);
                ast_list_push(p->arena, &map->as.map_lit.vals, first_val);
                while (at(p, TOK_COMMA)) {
                    advance(p);
                    if (at(p, TOK_RPAREN)) break;
                    if (!at(p, TOK_IDENT)) {
                        set_err(p, DECK_LOAD_PARSE,
                                "named-field group expects `name: value` pairs");
                        return NULL;
                    }
                    ast_node_t *key = ast_new(p->arena, AST_LIT_STR,
                                              p->cur.line, p->cur.col);
                    if (!key) return NULL;
                    key->as.s = p->cur.text;
                    advance(p);
                    if (!expect(p, TOK_COLON,
                                "expected ':' after field name in named-field group"))
                        return NULL;
                    ast_node_t *val = parse_expr_prec(p, 0);
                    if (!val) return NULL;
                    ast_list_push(p->arena, &map->as.map_lit.keys, key);
                    ast_list_push(p->arena, &map->as.map_lit.vals, val);
                }
                if (!expect(p, TOK_RPAREN,
                            "expected ')' closing named-field group")) return NULL;
                n = map;
                break;
            }
            /* Spec §5 — paren content may be a typed lambda param list
             * `(a: Type, b: Type) -> body`. Eat `:Type` only when the
             * parsed element is a bare ident AND the type is also an
             * ident / dotted path (safe — rules out variant-record
             * payload `:active (temp: 25.0, max: 30.0)` where `25.0`
             * is a numeric literal, not a type). */
            if (at(p, TOK_COLON) && first && first->kind == AST_IDENT &&
                (peek_next_tok(p) == TOK_IDENT)) {
                advance(p);
                if (!skip_type_annotation(p)) return NULL;
            }
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
                    if (at(p, TOK_COLON) && item->kind == AST_IDENT &&
                        (peek_next_tok(p) == TOK_IDENT)) {
                        advance(p);
                        if (!skip_type_annotation(p)) return NULL;
                    }
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
                            set_err(p, DECK_LOAD_PARSE,
                                    "lambda parameter list must contain only identifiers");
                            return NULL;
                        }
                        plist[i] = item->as.s;
                    }
                } else {
                    if (!first || first->kind != AST_IDENT) {
                        set_err(p, DECK_LOAD_PARSE,
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
             * Keys may be any expression (typically atom/string/int).
             *
             * Spec §4.3 record-update form: `{ <base_expr> with f1: v1,
             * f2: v2 }`. Detected by scanning the first sub-expression
             * for a trailing `with` keyword before the first `:`. We
             * emit AST_WITH with bare-name keys (string literals) so it
             * matches the existing `expr with { … }` postfix's runtime. */
            uint32_t ln = p->cur.line, co = p->cur.col;
            advance(p);
            n = ast_new(p->arena, AST_LIT_MAP, ln, co); if (!n) return NULL;
            ast_list_init(&n->as.map_lit.keys);
            ast_list_init(&n->as.map_lit.vals);
            if (!at(p, TOK_RBRACE)) {
                /* First lookahead: a bare-keyword `with` after the
                 * primary expression flips the brace into record-update
                 * mode. parse_expr_prec will not consume `with` because
                 * the postfix-`with` form expects `{` after it, not
                 * another field. Detect by parsing the base, then
                 * checking for `with`.
                 *
                 * Bare-name → string key shortcut: `{ tasks: [...], … }`
                 * — when the first token is an IDENT immediately
                 * followed by `:`, treat it as a string key (the spec's
                 * record-literal form). Same applies in the comma loop
                 * below. */
                ast_node_t *first = NULL;
                if (at(p, TOK_IDENT) && peek_next_tok(p) == TOK_COLON) {
                    first = mknode(p, AST_LIT_STR); if (!first) return NULL;
                    first->as.s = p->cur.text;
                    advance(p);
                } else {
                    first = parse_expr_prec(p, 0);
                    if (!first) return NULL;
                }
                if (at(p, TOK_KW_WITH)) {
                    advance(p); /* with */
                    ast_node_t *w = mknode(p, AST_WITH); if (!w) return NULL;
                    w->as.with_.base = first;
                    ast_list_init(&w->as.with_.keys);
                    ast_list_init(&w->as.with_.vals);
                    for (;;) {
                        ast_node_t *k = NULL;
                        if (at(p, TOK_IDENT) && peek_next_tok(p) == TOK_COLON) {
                            k = mknode(p, AST_LIT_STR); if (!k) return NULL;
                            k->as.s = p->cur.text;
                            advance(p);
                        } else {
                            k = parse_expr_prec(p, 0);
                            if (!k) return NULL;
                        }
                        if (!expect(p, TOK_COLON, "expected ':' in `{ x with f: v }`")) return NULL;
                        ast_node_t *v = parse_expr_prec(p, 0);
                        if (!v) return NULL;
                        ast_list_push(p->arena, &w->as.with_.keys, k);
                        ast_list_push(p->arena, &w->as.with_.vals, v);
                        if (!at(p, TOK_COMMA)) break;
                        advance(p);
                        if (at(p, TOK_RBRACE)) break;
                    }
                    if (!expect(p, TOK_RBRACE, "expected '}' to close `{ x with … }`")) return NULL;
                    n = w;
                    break;
                }
                /* Plain map literal — `first` was the first key. */
                if (!expect(p, TOK_COLON, "expected ':' in map literal")) return NULL;
                ast_node_t *v0 = parse_expr_prec(p, 0);
                if (!v0) return NULL;
                ast_list_push(p->arena, &n->as.map_lit.keys, first);
                ast_list_push(p->arena, &n->as.map_lit.vals, v0);
                while (at(p, TOK_COMMA)) {
                    advance(p);
                    if (at(p, TOK_RBRACE)) break;
                    ast_node_t *k = NULL;
                    if (at(p, TOK_IDENT) && peek_next_tok(p) == TOK_COLON) {
                        k = mknode(p, AST_LIT_STR); if (!k) return NULL;
                        k->as.s = p->cur.text;
                        advance(p);
                    } else {
                        k = parse_expr_prec(p, 0);
                        if (!k) return NULL;
                    }
                    if (!expect(p, TOK_COLON, "expected ':' in map literal")) return NULL;
                    ast_node_t *v = parse_expr_prec(p, 0);
                    if (!v) return NULL;
                    ast_list_push(p->arena, &n->as.map_lit.keys, k);
                    ast_list_push(p->arena, &n->as.map_lit.vals, v);
                }
            }
            if (!expect(p, TOK_RBRACE, "expected '}' to close map")) return NULL;
            break;
        }
        case TOK_LBRACKET: {
            /* DL2 F21.4: list literal `[e1, e2, ...]` (also empty `[]`).
             * H2 — trailing comma allowed for multi-line literals. */
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
                    if (at(p, TOK_RBRACKET)) break;   /* trailing comma */
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
        case TOK_KW_DO: {
            /* Spec §7 — explicit `do` block as an expression. Desugars
             * to the suite form: `do NEWLINE INDENT stmt+ DEDENT` yields
             * a DO node whose value is the final statement's value. The
             * body may be an indented suite (canonical) or a single
             * inline statement `do stmt`. */
            uint32_t ln = p->cur.line, co = p->cur.col;
            advance(p);   /* do */
            ast_node_t *body = parse_suite(p);
            if (!body) return NULL;
            /* parse_suite returns either AST_DO (indented form) or the
             * bare statement (single-line form). Wrap bare statements
             * so the shape is uniform for downstream consumers. */
            if (body->kind != AST_DO) {
                ast_node_t *d = ast_new(p->arena, AST_DO, ln, co);
                if (!d) return NULL;
                ast_list_init(&d->as.do_.exprs);
                ast_list_push(p->arena, &d->as.do_.exprs, body);
                body = d;
            }
            return parse_postfix(p, body);
        }
        case TOK_KW_FN: {
            /* DL2 F21.2: anonymous fn in expression position. */
            ast_node_t *fnn = parse_fn_decl(p);
            if (!fnn) return NULL;
            if (fnn->as.fndef.name) {
                set_err(p, DECK_LOAD_PARSE,
                        "named fn declarations are only allowed at top level");
                return NULL;
            }
            return parse_postfix(p, fnn);
        }
        default:
            set_err(p, DECK_LOAD_PARSE, "expected expression");
            return NULL;
    }
    if (!n) return NULL;
    return parse_postfix(p, n);
}

static ast_node_t *parse_postfix(deck_parser_t *p, ast_node_t *head)
{
    for (;;) {
        /* LANG §11.1 — postfix `?` propagation on a Result-typed expr.
         * Desugars at eval time: if the value is (:ok, v) yield v, if
         * (:err, e) unwind to the enclosing fn body and return that
         * err tuple directly. */
        if (at(p, TOK_QUESTION)) {
            ast_node_t *t = mknode(p, AST_TRY); if (!t) return NULL;
            t->as.try_.inner = head;
            advance(p);
            head = t;
            continue;
        }
        if (at(p, TOK_DOT)) {
            advance(p);
            /* DL2 F21.5: `.0`, `.1`, ... is tuple-field access. */
            if (at(p, TOK_INT)) {
                if (p->cur.as.i < 0) {
                    set_err(p, DECK_LOAD_PARSE, "tuple index must be non-negative");
                    return NULL;
                }
                ast_node_t *n = mknode(p, AST_TUPLE_GET); if (!n) return NULL;
                n->as.tuple_get.obj = head;
                n->as.tuple_get.idx = (uint32_t)p->cur.as.i;
                advance(p);
                head = n;
                continue;
            }
            /* `send` and `some`/`none` are reserved keywords that also
             * appear legitimately as method names on capabilities
             * (Machine.send, option.some, option.none). Accept them
             * here so `cap.method` lookups don't trip the keyword. */
            const char *field_name = NULL;
            if      (at(p, TOK_IDENT))    field_name = p->cur.text;
            else if (at(p, TOK_KW_SEND))  field_name = deck_intern_cstr("send");
            else if (at(p, TOK_KW_SOME))  field_name = deck_intern_cstr("some");
            else if (at(p, TOK_KW_NONE))  field_name = deck_intern_cstr("none");
            else {
                set_err(p, DECK_LOAD_PARSE,
                        "expected field name or index after '.'");
                return NULL;
            }
            ast_node_t *n = mknode(p, AST_DOT); if (!n) return NULL;
            n->as.dot.obj   = head;
            n->as.dot.field = field_name;
            advance(p);
            head = n;
            continue;
        }
        if (at(p, TOK_LPAREN)) {
            advance(p);
            ast_node_t *c = mknode(p, AST_CALL); if (!c) return NULL;
            c->as.call.fn = head;
            c->as.call.arg_names = NULL;
            ast_list_init(&c->as.call.args);
            /* Concept #66 — spec §6.6: named call args `fn(a: 7, b: 8)`.
             * A leading IDENT followed by COLON marks a named arg; the
             * whole call must then be all-named (no mixing). We decide
             * mode on the first arg and enforce consistency across the
             * list. Collect names in a local buffer and copy into the
             * arena only when all-named (saves an alloc for positional
             * calls). */
            const char *names_buf[16];
            uint32_t    n_named = 0;
            bool        all_named = false;
            if (!at(p, TOK_RPAREN)) {
                for (;;) {
                    const char *name = NULL;
                    if (at(p, TOK_IDENT) && peek_next_tok(p) == TOK_COLON) {
                        name = p->cur.text;
                        advance(p);   /* ident */
                        advance(p);   /* : */
                    }
                    /* First arg sets the mode. Subsequent args must match. */
                    if (c->as.call.args.len == 0) {
                        all_named = (name != NULL);
                    } else if ((name != NULL) != all_named) {
                        set_err(p, DECK_LOAD_PARSE,
                                "cannot mix positional and named args in one call (spec §6.6)");
                        return NULL;
                    }
                    ast_node_t *arg = parse_expr_prec(p, 0);
                    if (!arg) return NULL;
                    ast_list_push(p->arena, &c->as.call.args, arg);
                    if (all_named) {
                        if (n_named >= 16) {
                            set_err(p, DECK_LOAD_PARSE,
                                    "too many named args (max 16)");
                            return NULL;
                        }
                        names_buf[n_named++] = name;
                    }
                    if (!at(p, TOK_COMMA)) break;
                    advance(p);
                }
            }
            if (!expect(p, TOK_RPAREN, "expected ')' after call args")) return NULL;
            if (all_named && n_named > 0) {
                c->as.call.arg_names =
                    deck_arena_memdup(p->arena, names_buf,
                                      n_named * sizeof(char *));
                if (!c->as.call.arg_names) return NULL;
            }
            head = c;
            continue;
        }
        if (at(p, TOK_KW_WITH)) {
            /* DL2 F22.2 — `expr with { field: val, ... }` returns a
             * new record/map with the given fields updated. Spec §4.3
             * treats `field` on the LHS of `:` as a bare name (not an
             * expression) — emit a string literal so the runtime's
             * with-update matches map string keys. */
            advance(p); /* with */
            if (!expect(p, TOK_LBRACE, "expected '{' after `with`")) return NULL;
            ast_node_t *w = mknode(p, AST_WITH); if (!w) return NULL;
            w->as.with_.base = head;
            ast_list_init(&w->as.with_.keys);
            ast_list_init(&w->as.with_.vals);
            if (!at(p, TOK_RBRACE)) {
                for (;;) {
                    ast_node_t *k = NULL;
                    if (at(p, TOK_IDENT) && peek_next_tok(p) == TOK_COLON) {
                        k = mknode(p, AST_LIT_STR); if (!k) return NULL;
                        k->as.s = p->cur.text;
                        advance(p);
                    } else {
                        k = parse_expr_prec(p, 0);
                        if (!k) return NULL;
                    }
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
                set_err(p, DECK_LOAD_PARSE, "too many `where` bindings (max 16)");
                return NULL;
            }
            if (!at(p, TOK_IDENT)) {
                set_err(p, DECK_LOAD_PARSE, "expected name in `where` binding");
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
                set_err(p, DECK_LOAD_PARSE, "too many `where` bindings (max 16)");
                return NULL;
            }
            if (!at(p, TOK_IDENT)) {
                set_err(p, DECK_LOAD_PARSE, "expected name in `where` binding");
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
        /* Indent-continuation: `let X = A\n   && B` — the lexer leaves
         * only a bare NEWLINE before the binop (INDENT suppressed by
         * handle_line_start continuation detection). Peek across the
         * newline; if the next token is a binop we continue the chain.
         *
         * When a blank or comment-only line sits between the LHS and the
         * continuation (`= A \n -- comment \n  && B`) the lexer emits two
         * NEWLINEs back-to-back — one per source line. Skip past the run
         * of NEWLINEs before deciding, so comments between continuation
         * lines are transparent (spec §2.2 — comments are whitespace). */
        if (at(p, TOK_NEWLINE)) {
            deck_tok_t nx = peek_next_tok(p);
            while (nx == TOK_NEWLINE) {
                advance(p);
                nx = peek_next_tok(p);
            }
            binop_info_t peek_info;
            if (binop_for(nx, &peek_info) && peek_info.prec >= min_prec) {
                advance(p);   /* consume the final NEWLINE */
            } else {
                break;
            }
        }
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

static ast_node_t *parse_pattern_primary(deck_parser_t *p);

static ast_node_t *parse_pattern(deck_parser_t *p)
{
    ast_node_t *head = parse_pattern_primary(p);
    if (!head) return NULL;
    /* Spec §8 — right-associative cons pattern `H :: T` destructures a
     * non-empty list. Desugars to a variant pattern with ctor "::" and
     * two sub-patterns so the matcher can handle it uniformly. */
    if (at(p, TOK_CONS)) {
        uint32_t ln = p->cur.line, co = p->cur.col;
        advance(p);
        ast_node_t *tail = parse_pattern(p);
        if (!tail) return NULL;
        ast_node_t *n = ast_new(p->arena, AST_PAT_VARIANT, ln, co);
        if (!n) return NULL;
        ast_node_t **subs = deck_arena_alloc(p->arena, sizeof(ast_node_t *) * 2);
        if (!subs) return NULL;
        subs[0] = head; subs[1] = tail;
        n->as.pat_variant.ctor   = deck_intern_cstr("::");
        n->as.pat_variant.subs   = subs;
        n->as.pat_variant.n_subs = 2;
        return n;
    }
    return head;
}

static ast_node_t *parse_pattern_primary(deck_parser_t *p)
{
    /* Spec §5.6 — record pattern `Type { field: pat, … }` /
     * `Type { field }` (field-shorthand). Detected by an IDENT followed
     * by `{`. The runtime treats records as maps, so we encode each
     * named-field sub-pattern as a positional sub here, dropping the
     * field name (the matcher accesses by atom-key in the map). */
    if (p->cur.type == TOK_IDENT && peek_next_tok(p) == TOK_LBRACE) {
        const char *ctor = p->cur.text;
        uint32_t ln = p->cur.line, co = p->cur.col;
        advance(p); advance(p);   /* type name, { */
        ast_node_t *subs[16];
        uint32_t n_subs = 0;
        if (!at(p, TOK_RBRACE)) {
            for (;;) {
                if (n_subs >= 16) {
                    set_err(p, DECK_LOAD_PARSE,
                            "record pattern: too many fields (max 16)");
                    return NULL;
                }
                if (!at(p, TOK_IDENT)) {
                    set_err(p, DECK_LOAD_PARSE,
                            "expected field name in record pattern");
                    return NULL;
                }
                const char *field_name = p->cur.text;
                advance(p);
                ast_node_t *s;
                if (at(p, TOK_COLON)) {
                    advance(p);
                    s = parse_pattern(p); if (!s) return NULL;
                } else {
                    /* field-shorthand: `Type { field }` binds `field`
                     * to the field's value. */
                    s = ast_new(p->arena, AST_PAT_IDENT, ln, co);
                    if (!s) return NULL;
                    s->as.pat_ident = field_name;
                }
                subs[n_subs++] = s;
                if (!at(p, TOK_COMMA)) break;
                advance(p);
            }
        }
        if (!expect(p, TOK_RBRACE, "expected '}' closing record pattern")) return NULL;
        ast_node_t *n = ast_new(p->arena, AST_PAT_VARIANT, ln, co);
        if (!n) return NULL;
        n->as.pat_variant.ctor   = ctor;
        n->as.pat_variant.subs   = n_subs > 0
            ? deck_arena_memdup(p->arena, subs, n_subs * sizeof(ast_node_t *))
            : NULL;
        n->as.pat_variant.n_subs = n_subs;
        return n;
    }
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
                    set_err(p, DECK_LOAD_PARSE, "variant pattern: too many subs (max 8)");
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
        case TOK_LBRACKET: {
            /* Spec §8.2 — list patterns:
             *   `[]`          empty-list pattern
             *   `[p1, …, pN]` fixed-length list pattern (matches when
             *                 list.len == N and each sub-pattern matches)
             * Both encoded as AST_PAT_VARIANT with ctor "[]"; n_subs == 0
             * is the empty-list case, n_subs > 0 is fixed-length. The
             * `[head, ...tail]` rest form stays expressed via the
             * existing `h :: t` cons pattern for now. */
            uint32_t ln = p->cur.line, co = p->cur.col;
            advance(p); /* [ */
            ast_node_t *subs[16];
            uint32_t n_subs = 0;
            const char *rest_binder = NULL;   /* Spec §5.6 cons pattern */
            if (!at(p, TOK_RBRACKET)) {
                for (;;) {
                    if (n_subs >= 16) {
                        set_err(p, DECK_LOAD_PARSE,
                                "list pattern: too many elements (max 16)");
                        return NULL;
                    }
                    /* `...rest` cons-binder — `...` lexes as three
                     * TOK_DOTs followed by an IDENT. Captures the
                     * tail binding name and stops the loop. */
                    if (at(p, TOK_DOT) &&
                        peek_next_tok(p) == TOK_DOT) {
                        advance(p); advance(p);
                        if (at(p, TOK_DOT)) advance(p);
                        if (!at(p, TOK_IDENT)) {
                            set_err(p, DECK_LOAD_PARSE,
                                    "expected rest-binder ident after `...`");
                            return NULL;
                        }
                        rest_binder = p->cur.text;
                        advance(p);
                        break;
                    }
                    ast_node_t *s = parse_pattern(p); if (!s) return NULL;
                    subs[n_subs++] = s;
                    if (!at(p, TOK_COMMA)) break;
                    advance(p);
                }
            }
            if (!expect(p, TOK_RBRACKET, "expected ']' closing list pattern")) return NULL;
            /* Encode `[head, ...rest]` as a variant whose ctor signals
             * the cons shape and last sub is a synthetic IDENT pattern
             * for the rest binder. The matcher distinguishes cons via
             * the special ctor name. */
            const char *ctor = rest_binder ? "[h...]" : "[]";
            if (rest_binder) {
                ast_node_t *r = ast_new(p->arena, AST_PAT_IDENT, ln, co);
                if (!r) return NULL;
                r->as.pat_ident = rest_binder;
                if (n_subs >= 16) {
                    set_err(p, DECK_LOAD_PARSE,
                            "list pattern: too many elements (max 16)");
                    return NULL;
                }
                subs[n_subs++] = r;
            }
            ast_node_t *n = ast_new(p->arena, AST_PAT_VARIANT, ln, co);
            if (!n) return NULL;
            n->as.pat_variant.ctor   = deck_intern_cstr(ctor);
            n->as.pat_variant.subs   = n_subs ? deck_arena_memdup(p->arena, subs, n_subs * sizeof(ast_node_t *)) : NULL;
            n->as.pat_variant.n_subs = n_subs;
            return n;
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
            /* `:ctor <sub>` — variant pattern. Accept any pattern-start
             * token as the sub: ident binder / wildcard (TOK_IDENT),
             * literal (INT/FLOAT/STRING/TRUE/FALSE/UNIT/NONE), nested
             * atom patterns `:err :oops`, empty-list pattern `:ok []`,
             * and parenthesised / tuple sub-patterns `:ok (:some v)`
             * (spec §8.2 nested variant-of-variant). */
            if (at(p, TOK_IDENT) || at(p, TOK_ATOM) ||
                at(p, TOK_INT) || at(p, TOK_FLOAT) || at(p, TOK_STRING) ||
                at(p, TOK_KW_TRUE) || at(p, TOK_KW_FALSE) ||
                at(p, TOK_KW_UNIT) || at(p, TOK_KW_NONE) ||
                at(p, TOK_LBRACKET) || at(p, TOK_LPAREN)) {
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
        case TOK_LPAREN: {
            /* Spec §5.6 — paren pattern is one of:
             *   ()                       unit pattern
             *   (pat)                    parenthesised
             *   (pat, pat, …)            positional tuple
             *   (field: pat, …)          named tuple — fields collected
             *                            into the same variant shape as
             *                            positional, sub patterns only
             *                            (field names are parsed-and-
             *                            discarded since the runtime
             *                            represents named tuples as the
             *                            same map shape used for atom-
             *                            variant payloads, keyed by
             *                            field name in match). */
            uint32_t ln = p->cur.line, co = p->cur.col;
            advance(p); /* ( */
            if (at(p, TOK_RPAREN)) {
                advance(p);
                ast_node_t *atom_lit = ast_new(p->arena, AST_LIT_UNIT, ln, co);
                if (!atom_lit) return NULL;
                ast_node_t *wrap = ast_new(p->arena, AST_PAT_LIT, ln, co);
                if (!wrap) return NULL;
                wrap->as.pat_lit = atom_lit;
                return wrap;
            }
            ast_node_t *subs[16];
            uint32_t n_subs = 0;
            for (;;) {
                if (n_subs >= 16) {
                    set_err(p, DECK_LOAD_PARSE,
                            "tuple pattern: too many elements (max 16)");
                    return NULL;
                }
                /* Named-tuple element `field: pat` — eat the field
                 * name + colon, then parse the sub-pattern. The field
                 * name is positional in the variant we build, mirroring
                 * how the runtime stores named tuples. */
                if (at(p, TOK_IDENT) && peek_next_tok(p) == TOK_COLON) {
                    advance(p); /* field name */
                    advance(p); /* : */
                }
                ast_node_t *s = parse_pattern(p); if (!s) return NULL;
                subs[n_subs++] = s;
                if (!at(p, TOK_COMMA)) break;
                advance(p);
            }
            if (!expect(p, TOK_RPAREN, "expected ')' in tuple pattern")) return NULL;
            if (n_subs == 1) return subs[0];   /* parenthesised pattern */
            ast_node_t *n = ast_new(p->arena, AST_PAT_VARIANT, ln, co);
            if (!n) return NULL;
            n->as.pat_variant.ctor   = deck_intern_cstr("(,)");
            n->as.pat_variant.subs   = deck_arena_memdup(p->arena, subs, n_subs * sizeof(ast_node_t *));
            n->as.pat_variant.n_subs = n_subs;
            return n;
        }
        default:
            set_err(p, DECK_LOAD_PARSE, "expected pattern");
            return NULL;
    }
}

static ast_node_t *parse_match(deck_parser_t *p)
{
    ast_node_t *m = mknode(p, AST_MATCH); if (!m) return NULL;
    advance(p); /* match */
    /* Scrutinee expression — suppress the IDENT-lambda lookahead so
     * `match x | …` doesn't eat the pipe as part of an `IDENT -> body`
     * lambda candidate. Not strictly needed (| isn't ->), but keeps
     * the shape symmetric with guard parsing. */
    m->as.match.scrut = parse_expr_prec(p, 0);
    if (!m->as.match.scrut) return NULL;
    /* Spec §8 — arms may appear either on continuation lines (NEWLINE
     * INDENT) or inline on the scrutinee's own line when the first arm
     * starts with `|`. This one-liner form is common for bool matches:
     *     let m = match 1 < 2 | true -> 100 | false -> 200 */
    if (at(p, TOK_BAR)) {
        ast_arm_t arms[64];
        uint32_t n_arms = 0;
        while (at(p, TOK_BAR) && n_arms < 64) {
            advance(p); /* | */
            ast_node_t *pat = parse_pattern(p); if (!pat) return NULL;
            ast_node_t *guard = NULL;
            if (at(p, TOK_KW_WHEN)) {
                advance(p);
                bool prev = p->no_lambda;
                p->no_lambda = true;
                guard = parse_expr_prec(p, 0);
                p->no_lambda = prev;
                if (!guard) return NULL;
            }
            if (!at(p, TOK_ARROW)) {
                set_err(p, DECK_LOAD_PARSE,
                        "expected '->' in match arm (spec 01-deck-lang §8)");
                return NULL;
            }
            advance(p);
            /* Body expression — lambda lookahead off so it stops at the
             * next `|` rather than consuming it as a binop expression. */
            bool prev = p->no_lambda;
            p->no_lambda = true;
            ast_node_t *body = parse_expr_prec(p, 0);
            p->no_lambda = prev;
            if (!body) return NULL;
            arms[n_arms].pattern = pat;
            arms[n_arms].guard   = guard;
            arms[n_arms].body    = body;
            n_arms++;
        }
        m->as.match.arms = deck_arena_memdup(p->arena, arms,
                                              n_arms * sizeof(ast_arm_t));
        m->as.match.n_arms = n_arms;
        return m;
    }
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
        if (at(p, TOK_KW_WHEN)) {
            advance(p);
            bool prev = p->no_lambda;
            p->no_lambda = true;
            guard = parse_expr_prec(p, 0);
            p->no_lambda = prev;
            if (!guard) return NULL;
        }
        if (!at(p, TOK_ARROW)) {
            set_err(p, DECK_LOAD_PARSE,
                    "expected '->' in match arm (spec 01-deck-lang §8); "
                    "the legacy '=>' arrow is no longer accepted");
            return NULL;
        }
        advance(p); /* -> */
        /* Spec §8 — arm body can be a single inline expression, a single
         * expression wrapped to the next line, or a multi-statement
         * suite if the continuation opens an INDENT block:
         *     | :err e ->
         *         log.error("fail")
         *         log.info("DECK_CONF_FAIL:x")
         * The indented form is parsed as a DO block so side-effects
         * execute in order and the final expression becomes the arm's
         * value. */
        ast_node_t *body = NULL;
        bool consumed_indent = false;
        if (at(p, TOK_NEWLINE)) {
            advance(p);
            while (at(p, TOK_NEWLINE)) advance(p);
            if (at(p, TOK_INDENT)) { advance(p); consumed_indent = true; }
        }
        if (consumed_indent) {
            ast_node_t *d = mknode(p, AST_DO); if (!d) return NULL;
            ast_list_init(&d->as.do_.exprs);
            while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
                if (at(p, TOK_NEWLINE)) { advance(p); continue; }
                ast_node_t *s = parse_stmt(p); if (!s) return NULL;
                ast_list_push(p->arena, &d->as.do_.exprs, s);
                while (at(p, TOK_NEWLINE)) advance(p);
            }
            body = d;
        } else {
            body = parse_expr_prec(p, 0);
            if (!body) return NULL;
        }
        while (at(p, TOK_NEWLINE)) advance(p);
        if (consumed_indent) {
            if (at(p, TOK_DEDENT)) advance(p);
        }
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

/* Spec §5.1 tuple destructuring `let (a, b) = expr`. Desugared at
 * parse-time into a sequence of plain lets wrapped in an AST_DO that
 * shares the enclosing env:
 *
 *     let _dest$L$C$N = expr
 *     let a = _dest$L$C$N.0
 *     let b = _dest$L$C$N.1
 *
 * Arena-allocated name (not interned) — evaporates with the per-load
 * arena reset, so rerunning the fixture doesn't leak intern entries.
 * env lookup already falls back to strcmp, so pointer identity across
 * the holder binding and the field-access idents doesn't matter. */
static uint32_t s_let_dest_seq = 0;

static ast_node_t *parse_let_destructure(deck_parser_t *p, uint32_t ln, uint32_t co)
{
    advance(p); /* ( */
    const char *names[16];
    uint32_t n_names = 0;
    if (at(p, TOK_RPAREN)) {
        set_err(p, DECK_LOAD_PARSE,
                "let destructuring pattern cannot be empty");
        return NULL;
    }
    for (;;) {
        if (n_names >= 16) {
            set_err(p, DECK_LOAD_PARSE,
                    "let destructuring: too many elements (max 16)");
            return NULL;
        }
        if (!at(p, TOK_IDENT)) {
            set_err(p, DECK_LOAD_PARSE,
                    "let destructuring: only plain ident binders supported at this layer");
            return NULL;
        }
        names[n_names++] = p->cur.text;
        advance(p);
        if (!at(p, TOK_COMMA)) break;
        advance(p);
    }
    if (!expect(p, TOK_RPAREN, "expected ')' closing let destructuring pattern")) return NULL;
    if (n_names == 1) {
        set_err(p, DECK_LOAD_PARSE,
                "let destructuring pattern must contain at least 2 elements");
        return NULL;
    }
    if (at(p, TOK_COLON)) {
        advance(p);
        if (!skip_type_annotation(p)) return NULL;
    }
    if (!expect(p, TOK_ASSIGN, "expected '=' in let binding")) return NULL;
    ast_node_t *val = parse_expr_prec(p, 0);
    if (!val) return NULL;

    char tmp[40];
    uint32_t seq = ++s_let_dest_seq;
    snprintf(tmp, sizeof(tmp), "_dest$%u$%u$%u",
             (unsigned)ln, (unsigned)co, (unsigned)seq);
    char *dest_name = deck_arena_strdup(p->arena, tmp);
    if (!dest_name) return NULL;

    ast_node_t *d = ast_new(p->arena, AST_DO, ln, co); if (!d) return NULL;
    ast_list_init(&d->as.do_.exprs);

    ast_node_t *holder = ast_new(p->arena, AST_LET, ln, co); if (!holder) return NULL;
    holder->as.let.name  = dest_name;
    holder->as.let.value = val;
    holder->as.let.body  = NULL;
    ast_list_push(p->arena, &d->as.do_.exprs, holder);

    for (uint32_t i = 0; i < n_names; i++) {
        ast_node_t *ident = ast_new(p->arena, AST_IDENT, ln, co); if (!ident) return NULL;
        ident->as.s = dest_name;
        ast_node_t *get = ast_new(p->arena, AST_TUPLE_GET, ln, co); if (!get) return NULL;
        get->as.tuple_get.obj = ident;
        get->as.tuple_get.idx = i;
        ast_node_t *bind = ast_new(p->arena, AST_LET, ln, co); if (!bind) return NULL;
        bind->as.let.name  = names[i];
        bind->as.let.value = get;
        bind->as.let.body  = NULL;
        ast_list_push(p->arena, &d->as.do_.exprs, bind);
    }
    return d;
}

static ast_node_t *parse_let_stmt(deck_parser_t *p)
{
    uint32_t ln = p->cur.line, co = p->cur.col;
    advance(p); /* let */
    /* Spec §5.1 destructuring path — `let (a, b) = …`. */
    if (at(p, TOK_LPAREN)) return parse_let_destructure(p, ln, co);
    ast_node_t *n = ast_new(p->arena, AST_LET, ln, co); if (!n) return NULL;
    if (!at(p, TOK_IDENT)) { set_err(p, DECK_LOAD_PARSE, "expected name after 'let'"); return NULL; }
    n->as.let.name = p->cur.text;
    advance(p);
    /* Spec §5.1 — optional `: Type` annotation on let bindings. The
     * runtime is dynamically typed; we parse-and-discard the annotation
     * so the author's documentation survives the parser. */
    if (at(p, TOK_COLON)) {
        advance(p);
        if (!skip_type_annotation(p)) return NULL;
    }
    if (!expect(p, TOK_ASSIGN, "expected '=' in let binding")) return NULL;
    n->as.let.value = parse_expr_prec(p, 0); if (!n->as.let.value) return NULL;
    n->as.let.body  = NULL; /* body attached by enclosing suite */
    return n;
}

static ast_node_t *parse_send_stmt(deck_parser_t *p)
{
    ast_node_t *n = mknode(p, AST_SEND); if (!n) return NULL;
    advance(p); /* send */
    if (!at(p, TOK_ATOM)) { set_err(p, DECK_LOAD_PARSE, "expected atom after 'send'"); return NULL; }
    n->as.send.event = p->cur.text;
    advance(p);
    return n;
}

static ast_node_t *parse_transition_stmt(deck_parser_t *p)
{
    ast_node_t *n = mknode(p, AST_TRANSITION); if (!n) return NULL;
    advance(p); /* transition */
    if (!at(p, TOK_ATOM)) { set_err(p, DECK_LOAD_PARSE, "expected atom after 'transition'"); return NULL; }
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
static ast_node_t *parse_needs_decl(deck_parser_t *p);
static ast_node_t *parse_use_decl(deck_parser_t *p);
static ast_node_t *parse_on_decl(deck_parser_t *p);
static ast_node_t *parse_machine_decl(deck_parser_t *p);
static ast_node_t *parse_machine_hook_decl(deck_parser_t *p, const char *event);
static ast_node_t *parse_assets_decl(deck_parser_t *p);
static ast_node_t *parse_flow_decl(deck_parser_t *p);
static ast_node_t *parse_migrate_decl(deck_parser_t *p);

/* @app
 *   name:    "..."
 *   id:      "..."
 *   version: "..."
 *   edition: 2026
 *
 * Spec 02-deck-app §3: @app is identity-only. Every field is a scalar
 * expression (str/int/list literal). Version contracts live in a sibling
 * `@needs` block (§4A), never nested inside @app. The parser rejects
 * nested blocks inside @app with a clear spec pointer so authors migrate.
 */
static bool parse_scalar_fields(deck_parser_t   *p,
                                ast_app_field_t **out,
                                uint32_t         *out_n,
                                const char       *owner);
static bool parse_needs_fields(deck_parser_t   *p,
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

/* @needs
 *   deck_level: 2
 *   deck_os:    ">= 2"
 *   runtime:    ">= 1.0"
 *   capabilities:
 *     network.http: ">= 2"
 *     storage.local: "any"
 *
 * Top-level per 02-deck-app §4A. Fields may be scalars or a single-level
 * nested block (`capabilities:`). The parser produces an AST_NEEDS
 * node reusing AST_APP's ast_app_field_t layout; the nested block is
 * stored as a child AST_NEEDS value. The loader reads whichever
 * arrangement the source uses. */
static ast_node_t *parse_needs_decl(deck_parser_t *p)
{
    ast_node_t *n = mknode(p, AST_NEEDS); if (!n) return NULL;
    advance(p); /* @needs */
    if (!expect(p, TOK_NEWLINE, "expected newline after @needs")) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);
    if (!expect(p, TOK_INDENT, "expected indented @needs body")) return NULL;
    if (!parse_needs_fields(p, &n->as.app.fields, &n->as.app.n_fields)) return NULL;
    if (!expect(p, TOK_DEDENT, "expected dedent closing @needs")) return NULL;
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
            set_err(p, DECK_LOAD_PARSE, "expected field name");
            return false;
        }
        const char *name = p->cur.text;
        advance(p);
        /* Keep the error message owner-specific so tests can pin the
         * exact wording ("app field name" vs generic "field name"). */
        const char *emsg = (owner && strcmp(owner, "@app") == 0)
            ? "expected ':' after app field name"
            : "expected ':' after field name";
        if (!expect(p, TOK_COLON, emsg)) return false;
        if (at(p, TOK_NEWLINE)) {
            /* Reject nested blocks — @app is identity-only.
             * The common mistake is writing `needs:` nested inside
             * @app; point authors at the top-level @needs form. */
            if (strcmp(owner, "@app") == 0 &&
                (strcmp(name, "needs") == 0 ||
                 strcmp(name, "requires") == 0)) {
                set_err(p, DECK_LOAD_PARSE,
                        "`needs:` must be a top-level `@needs` "
                        "annotation (LANG §8), not a nested "
                        "field inside @app");
                return false;
            }
            /* G1 — accept an indented YAML-style bullet list:
             *   serves:
             *     - "endpoint/foo"
             *     - "endpoint/bar"
             * Synthesise an AST_LIT_LIST so the loader treats it as if
             * the source had written `serves: ["endpoint/foo", …]`. */
            advance(p); /* NEWLINE */
            while (at(p, TOK_NEWLINE)) advance(p);
            if (!at(p, TOK_INDENT)) {
                set_err(p, DECK_LOAD_PARSE,
                        "nested blocks are not allowed in this context");
                return false;
            }
            advance(p); /* INDENT */
            ast_node_t *list_lit = mknode(p, AST_LIT_LIST);
            if (!list_lit) return false;
            ast_list_init(&list_lit->as.list.items);
            while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
                if (at(p, TOK_NEWLINE)) { advance(p); continue; }
                if (!at(p, TOK_MINUS)) {
                    set_err(p, DECK_LOAD_PARSE,
                            "expected '-' starting list item in @app field");
                    return false;
                }
                advance(p); /* - */
                ast_node_t *item = parse_expr_prec(p, 0);
                if (!item) return false;
                ast_list_push(p->arena, &list_lit->as.list.items, item);
                while (at(p, TOK_NEWLINE)) advance(p);
            }
            if (!expect(p, TOK_DEDENT, "expected dedent closing yaml list")) return false;
            buf[n].name  = name;
            buf[n].value = list_lit;
            n++;
            continue;
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

static bool parse_needs_fields(deck_parser_t   *p,
                                  ast_app_field_t **out,
                                  uint32_t         *out_n)
{
    ast_app_field_t buf[32];
    uint32_t n = 0;
    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF) && n < 32) {
        const char *full_name = NULL;
        if (at(p, TOK_STRING)) {
            /* LANG §8 / SERVICES §7.2 — @needs.services uses quoted
             * service IDs ("storage.fs", "network.http"). Accept the
             * string form here so the services: sub-block parses. */
            full_name = p->cur.text;
            advance(p);
        } else if (at(p, TOK_IDENT)) {
            const char *name = p->cur.text;
            advance(p);
            /* Support dotted keys for capability paths like `network.http:`. */
            char scratch[128];
            uint32_t k = (uint32_t)snprintf(scratch, sizeof(scratch), "%s", name);
            while (at(p, TOK_DOT)) {
                advance(p);
                if (!at(p, TOK_IDENT)) {
                    set_err(p, DECK_LOAD_PARSE,
                            "expected ident after '.' in @needs key");
                    return false;
                }
                if (k < sizeof(scratch) - 1) scratch[k++] = '.';
                k += (uint32_t)snprintf(scratch + k, sizeof(scratch) - k,
                                        "%s", p->cur.text);
                advance(p);
            }
            full_name = deck_intern(scratch, k);
        } else {
            set_err(p, DECK_LOAD_PARSE, "expected field name in @needs");
            return false;
        }
        if (!expect(p, TOK_COLON, "expected ':' after @needs field name")) return false;
        ast_node_t *val = NULL;
        if (at(p, TOK_NEWLINE)) {
            /* Nested block (capabilities:). One level of nesting only. */
            advance(p);
            while (at(p, TOK_NEWLINE)) advance(p);
            if (!expect(p, TOK_INDENT, "expected indented nested block in @needs")) return false;
            ast_node_t *nested = ast_new(p->arena, AST_NEEDS,
                                         p->cur.line, p->cur.col);
            if (!nested) return false;
            if (!parse_needs_fields(p, &nested->as.app.fields,
                                       &nested->as.app.n_fields)) return false;
            if (!expect(p, TOK_DEDENT, "expected dedent in @needs nested block")) return false;
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
                set_err(p, DECK_LOAD_PARSE,
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
        set_err(p, DECK_LOAD_PARSE,
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
        set_err(p, DECK_LOAD_PARSE,
                "relative module paths in @use (spec §4.2) are not yet "
                "supported by this runtime; use dotted capability paths");
        return false;
    }
    if (!at(p, TOK_IDENT)) {
        set_err(p, DECK_LOAD_PARSE,
                "expected capability path in @use entry");
        return false;
    }
    k += (uint32_t)snprintf(scratch + k, cap - k, "%s", p->cur.text);
    advance(p);
    while (at(p, TOK_DOT)) {
        advance(p);
        if (!at(p, TOK_IDENT)) {
            set_err(p, DECK_LOAD_PARSE,
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

/* Spec 02-deck-app §11 — `@on <dotted.event>` with optional parameter
 * clause. Event names are dotted paths (e.g., `os.wifi_changed`), and
 * the clause `(field: pattern, field: pattern)` after the name binds
 * or pattern-matches payload fields. The trailing colon is optional
 * (spec examples omit it; older fixtures include it — both accepted). */
static ast_node_t *parse_on_decl(deck_parser_t *p)
{
    ast_node_t *n = mknode(p, AST_ON); if (!n) return NULL;
    advance(p); /* @on */
    if (!at(p, TOK_IDENT)) {
        set_err(p, DECK_LOAD_PARSE, "expected event name after @on");
        return NULL;
    }
    /* Dotted path: os.wifi_changed, hardware.button, etc. */
    char scratch[128];
    uint32_t k = (uint32_t)snprintf(scratch, sizeof(scratch), "%s", p->cur.text);
    advance(p);
    while (at(p, TOK_DOT)) {
        advance(p);
        if (!at(p, TOK_IDENT)) {
            set_err(p, DECK_LOAD_PARSE,
                    "expected ident after '.' in @on event path");
            return NULL;
        }
        if (k < sizeof(scratch) - 1) scratch[k++] = '.';
        k += (uint32_t)snprintf(scratch + k, sizeof(scratch) - k,
                                "%s", p->cur.text);
        advance(p);
    }
    n->as.on.event = deck_intern(scratch, k);

    /* Optional parameter clause `(field: pattern, field: pattern, ...)`.
     * Each entry's pattern is parsed via parse_pattern so `s` is a
     * binder, `0` is a literal-value filter, and `_` is accept-any. */
    if (at(p, TOK_LPAREN)) {
        advance(p);
        ast_on_param_t buf[16];
        uint32_t np = 0;
        if (!at(p, TOK_RPAREN)) {
            for (;;) {
                if (np >= 16) {
                    set_err(p, DECK_LOAD_PARSE,
                            "@on parameter clause: too many fields (max 16)");
                    return NULL;
                }
                if (!at(p, TOK_IDENT)) {
                    set_err(p, DECK_LOAD_PARSE,
                            "expected field name in @on parameter clause");
                    return NULL;
                }
                const char *field = p->cur.text;
                advance(p);
                if (!expect(p, TOK_COLON,
                            "expected ':' after field name in @on clause"))
                    return NULL;
                ast_node_t *pat = parse_pattern(p);
                if (!pat) return NULL;
                buf[np].field   = field;
                buf[np].pattern = pat;
                np++;
                if (!at(p, TOK_COMMA)) break;
                advance(p);
            }
        }
        if (!expect(p, TOK_RPAREN, "expected ')' closing @on parameter clause"))
            return NULL;
        n->as.on.params   = deck_arena_memdup(p->arena, buf,
                                              np * sizeof(ast_on_param_t));
        n->as.on.n_params = np;
    }

    /* Trailing `:` is optional (spec shows both forms). */
    if (at(p, TOK_COLON)) advance(p);

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
            set_err(p, DECK_LOAD_PARSE, "too many @assets entries (max 32)");
            return NULL;
        }
        if (!at(p, TOK_IDENT)) {
            set_err(p, DECK_LOAD_PARSE, "expected asset name");
            return NULL;
        }
        names[k] = p->cur.text;
        advance(p);
        if (!expect(p, TOK_COLON, "expected ':' after asset name")) return NULL;
        /* Accept any of:
         *   name: "path"                                   (legacy)
         *   name: <expr>                                   (e.g. ASSETS["x"])
         *   name: <expr> mod1: v1  mod2: v2 ...
         *   name: mod1: v1  mod2: v2 ...                   (no path, only modifiers)
         * The leading value is optional iff the next token is an ident
         * immediately followed by `:` (i.e. starts a modifier). */
        bool is_modifier_first = at(p, TOK_IDENT) && peek_next_tok(p) == TOK_COLON;
        if (is_modifier_first) {
            paths[k] = "";
        } else if (at(p, TOK_STRING)) {
            paths[k] = p->cur.text;
            advance(p);
        } else {
            paths[k] = "";
            ast_node_t *discard = parse_expr_prec(p, 0);
            if (!discard) return NULL;
        }
        while (at(p, TOK_IDENT) && peek_next_tok(p) == TOK_COLON) {
            advance(p); /* mod name */
            advance(p); /* : */
            /* Single-token literal value only — explicit list to avoid
             * parse_primary's variant-constructor heuristic (`:atom IDENT`
             * → `:atom(IDENT)`) which would eat the next modifier
             * name as the atom's payload. */
            switch (p->cur.type) {
                case TOK_STRING: case TOK_INT: case TOK_FLOAT:
                case TOK_ATOM:   case TOK_KW_TRUE: case TOK_KW_FALSE:
                case TOK_KW_UNIT:case TOK_KW_NONE:
                    advance(p);
                    break;
                case TOK_LBRACKET: case TOK_LBRACE: {
                    /* Eat balanced bracket span — value is parsed-and-
                     * discarded by the runtime today, so the structure
                     * doesn't need to land in the AST. */
                    int depth = 0;
                    do {
                        if (at(p, TOK_LBRACKET) || at(p, TOK_LBRACE)) depth++;
                        else if (at(p, TOK_RBRACKET) || at(p, TOK_RBRACE)) depth--;
                        advance(p);
                    } while (depth > 0 && !at(p, TOK_EOF));
                    break;
                }
                default:
                    set_err(p, DECK_LOAD_PARSE,
                            "expected literal value for asset modifier");
                    return NULL;
            }
        }
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
    if (!at(p, TOK_IDENT)) { set_err(p, DECK_LOAD_PARSE, "expected flow name"); return NULL; }
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
            set_err(p, DECK_LOAD_PARSE, "too many @flow steps (max 32)");
            return NULL;
        }
        /* `step <name>:` */
        if (!at(p, TOK_IDENT) || strcmp(p->cur.text, "step") != 0) {
            set_err(p, DECK_LOAD_PARSE,
                    "expected 'step' in @flow body");
            return NULL;
        }
        advance(p); /* step */
        if (!at(p, TOK_IDENT)) {
            set_err(p, DECK_LOAD_PARSE, "expected step name"); return NULL;
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
        set_err(p, DECK_LOAD_PARSE, "@flow must have at least one step");
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

/* DL2 F28.4 — `@migrate from N: <body>` blocks.
 *
 *   @migrate
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
static ast_node_t *parse_migrate_decl(deck_parser_t *p)
{
    ast_node_t *n = mknode(p, AST_MIGRATE); if (!n) return NULL;
    advance(p); /* @migrate */
    if (!expect(p, TOK_NEWLINE, "expected newline after @migrate")) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);
    if (!expect(p, TOK_INDENT, "expected indented @migrate body")) return NULL;

    int64_t      versions[DECK_MIGRATION_MAX];
    ast_node_t  *bodies[DECK_MIGRATION_MAX];
    uint32_t     k = 0;

    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
        if (k >= DECK_MIGRATION_MAX) {
            set_err(p, DECK_LOAD_PARSE, "too many @migrate entries (max 16)");
            return NULL;
        }
        if (!at(p, TOK_IDENT) || strcmp(p->cur.text, "from") != 0) {
            set_err(p, DECK_LOAD_PARSE, "expected 'from' in @migrate body");
            return NULL;
        }
        advance(p); /* from */
        if (!at(p, TOK_INT)) {
            set_err(p, DECK_LOAD_PARSE, "expected integer version after 'from'");
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
    if (!expect(p, TOK_DEDENT, "expected dedent closing @migrate body")) return NULL;

    if (k > 0) {
        n->as.migrate.from_versions = deck_arena_memdup(p->arena, versions, k * sizeof(int64_t));
        n->as.migrate.bodies        = deck_arena_memdup(p->arena, bodies,   k * sizeof(ast_node_t *));
        if (!n->as.migrate.from_versions || !n->as.migrate.bodies) return NULL;
    }
    n->as.migrate.n_entries = k;
    return n;
}

/* Concept #57 — parse inline `key: expr` option pairs after a content
 * item's header. Stops at NEWLINE / DEDENT / EOF, or when the next two
 * tokens are not `IDENT COLON`. Values are parsed as full expressions
 * so `options: [:dark, :light]`, `badge: unread_count`, `min: 0` all
 * work. Options are stashed in the parser arena; the walker evaluates
 * each value at render time. */
static void parse_content_options(deck_parser_t *p, ast_node_t *ci)
{
    uint32_t cap = 0;
    while (!at(p, TOK_NEWLINE) && !at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
        if (!at(p, TOK_IDENT)) break;
        if (peek_next_tok(p) != TOK_COLON) break;
        const char *key = p->cur.text;
        advance(p); /* IDENT */
        advance(p); /* COLON */
        ast_node_t *val = parse_expr_prec(p, 0);
        if (!val) break;
        if (ci->as.content_item.n_options == cap) {
            uint32_t new_cap = cap ? cap * 2 : 4;
            ast_content_option_t *arr = deck_arena_alloc(
                p->arena, sizeof(ast_content_option_t) * new_cap);
            if (!arr) return;
            if (ci->as.content_item.options)
                memcpy(arr, ci->as.content_item.options,
                       sizeof(ast_content_option_t) * ci->as.content_item.n_options);
            ci->as.content_item.options = arr;
            cap = new_cap;
        }
        ci->as.content_item.options[ci->as.content_item.n_options].key = key;
        ci->as.content_item.options[ci->as.content_item.n_options].value = val;
        ci->as.content_item.n_options++;
    }
}

static ast_node_t *parse_state_decl(deck_parser_t *p)
{
    ast_node_t *st = mknode(p, AST_STATE); if (!st) return NULL;
    advance(p); /* state */
    /* Spec 02-deck-app §8.3 — canonical name form is `state :atom`.
     * Legacy form `state IDENT:` (bare identifier + trailing colon)
     * is also accepted so existing conformance fixtures keep working.
     * The trailing colon is optional when the name is an atom. */
    if (at(p, TOK_ATOM)) {
        st->as.state.name = p->cur.text;
        advance(p);
    } else if (at(p, TOK_IDENT)) {
        st->as.state.name = p->cur.text;
        advance(p);
    } else {
        set_err(p, DECK_LOAD_PARSE, "expected state name");
        return NULL;
    }
    /* Spec 02-deck-app §8.3 — optional payload clause `(field: Type, …)`.
     * Parser eats it and discards (the runtime doesn't yet bind state
     * payloads across transitions). Up to 16 fields. */
    if (at(p, TOK_LPAREN)) {
        advance(p);
        uint32_t depth = 1;
        while (depth > 0 && !at(p, TOK_EOF)) {
            if (at(p, TOK_LPAREN)) depth++;
            else if (at(p, TOK_RPAREN)) { if (--depth == 0) break; }
            advance(p);
        }
        if (!expect(p, TOK_RPAREN, "expected ')' closing state payload clause")) return NULL;
    }
    /* Spec §8.3 — optional state composition `machine: Name` / `flow: Name`.
     * Recorded as an IDENT-value hook for later runtime support; today the
     * node is built but the interp ignores it (no state nesting yet). */
    if (at(p, TOK_IDENT) && p->cur.text &&
        (strcmp(p->cur.text, "machine") == 0 ||
         strcmp(p->cur.text, "flow") == 0)) {
        advance(p);
        if (!expect(p, TOK_COLON, "expected ':' after `machine`/`flow` in state decl")) return NULL;
        if (at(p, TOK_IDENT)) advance(p);
    }
    /* Trailing colon — legacy `state IDENT:` and the `state :x:` tolerance
     * from concept #14 both collapse here. Optional. */
    if (at(p, TOK_COLON)) advance(p);

    ast_list_init(&st->as.state.hooks);

    /* Bodyless state declarations (`state :welcome` on its own line) are
     * allowed per §8.3 — a state with no on-enter/leave/transition hooks
     * is terminal / composed-via-parent. */
    if (!at(p, TOK_NEWLINE) && !at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
        set_err(p, DECK_LOAD_PARSE,
                "unexpected token after state declaration");
        return NULL;
    }
    while (at(p, TOK_NEWLINE)) advance(p);
    if (!at(p, TOK_INDENT)) return st;   /* no body — OK */
    advance(p); /* INDENT */

    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
        if (at(p, TOK_KW_ON)) {
            advance(p);
            if (!at(p, TOK_KW_ENTER) && !at(p, TOK_KW_LEAVE)) {
                set_err(p, DECK_LOAD_PARSE, "expected 'enter' or 'leave'"); return NULL;
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
        } else if (at(p, TOK_IDENT) && p->cur.text &&
                   strcmp(p->cur.text, "content") == 0) {
            /* Spec 02-deck-app §8.2 — `content = <declarative nodes>` inside
             * a state body declares what the user perceives when the state
             * is active. Nodes are the §12 semantic primitives (list, group,
             * form, trigger, navigate, media, status, markdown, …); the
             * bridge infers presentation (§10-deck-bridge-ui).
             *
             * Concept #45 — parser now stores the body as an
             * AST_CONTENT_BLOCK with AST_CONTENT_ITEM children, so the
             * runtime content-evaluator (concept #46+) can walk it and
             * emit a DVC tree. Today each item captures its kind name
             * (first token, e.g. `trigger`/`navigate`/`label`) + an
             * optional string literal label + an optional arrow body.
             * Unknown shapes are stored as kind="raw" so they're at least
             * round-trippable. */
            advance(p); /* content */
            if (!expect(p, TOK_ASSIGN, "expected '=' after `content` in state body")) return NULL;
            ast_node_t *cb = mknode(p, AST_CONTENT_BLOCK); if (!cb) return NULL;
            ast_list_init(&cb->as.content_block.items);
            if (at(p, TOK_NEWLINE)) {
                advance(p);
                while (at(p, TOK_NEWLINE)) advance(p);
                if (at(p, TOK_INDENT)) {
                    advance(p);
                    /* Parse each line at this indent level as one content item.
                     * Nested indents (e.g. list body with `item x ->` lines) are
                     * collected into the parent item's action_expr as a raw opaque
                     * span for concept-#46 to unpack. */
                    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
                        if (at(p, TOK_NEWLINE)) { advance(p); continue; }
                        ast_node_t *ci = mknode(p, AST_CONTENT_ITEM); if (!ci) return NULL;
                        ci->as.content_item.kind = NULL;
                        ci->as.content_item.label = NULL;
                        ci->as.content_item.action_expr = NULL;
                        ci->as.content_item.data_expr = NULL;
                        ci->as.content_item.item_binder = NULL;
                        ast_list_init(&ci->as.content_item.item_body);
                        ast_list_init(&ci->as.content_item.empty_body);
                        ci->as.content_item.on_submit = NULL;
                        ci->as.content_item.options = NULL;
                        ci->as.content_item.n_options = 0;
                        /* Kind — first token on the line, if it's an ident/atom. */
                        if (at(p, TOK_IDENT)) {
                            ci->as.content_item.kind = p->cur.text;
                            advance(p);
                        } else if (at(p, TOK_ATOM)) {
                            ci->as.content_item.kind = p->cur.text;
                            advance(p);
                        } else {
                            ci->as.content_item.kind = "raw";
                        }
                        /* Optional string literal label (e.g. trigger "Search"). */
                        if (at(p, TOK_STRING)) {
                            ci->as.content_item.label = p->cur.text;
                            advance(p);
                        }
                        /* Optional arrow + action expression (trigger / navigate). */
                        if (at(p, TOK_ARROW)) {
                            advance(p);
                            ast_node_t *action = parse_expr_prec(p, 0);
                            if (action) ci->as.content_item.action_expr = action;
                        }
                        /* Trailing content on the header line is the data_expr
                         * for primitives like `list xs` / `media expr` / bare value.
                         * Parse as an expression if anything is left. Skip when
                         * the next two tokens are `IDENT COLON` — that's a
                         * concept-#57 option (badge:, placeholder:, etc.) —
                         * or when the next token is `on` (concept #58 trailing
                         * `on [atom]? -> action` handler, handled below). */
                        if (!at(p, TOK_NEWLINE) && !at(p, TOK_DEDENT) && !at(p, TOK_EOF) &&
                            !(at(p, TOK_IDENT) && peek_next_tok(p) == TOK_COLON) &&
                            !at(p, TOK_KW_ON)) {
                            ast_node_t *data = parse_expr_prec(p, 0);
                            if (data) {
                                if (!ci->as.content_item.action_expr)
                                    ci->as.content_item.action_expr = data;
                                else
                                    ci->as.content_item.data_expr = data;
                            }
                        }
                        /* Concept #57 — inline per-widget options. */
                        parse_content_options(p, ci);
                        /* Concept #58 — inline trailing `[on] -> action`
                         * after options, per spec §12.4
                         * (`toggle :x state: s on -> action` form) and
                         * annex-a `trigger L badge: B -> action`. */
                        if (at(p, TOK_KW_ON)) { advance(p); if (at(p, TOK_IDENT) || at(p, TOK_ATOM)) advance(p); }
                        if (at(p, TOK_ARROW)) {
                            advance(p);
                            ast_node_t *tail = parse_expr_prec(p, 0);
                            if (tail) {
                                if (ci->as.content_item.action_expr &&
                                    !ci->as.content_item.data_expr)
                                    ci->as.content_item.data_expr =
                                        ci->as.content_item.action_expr;
                                ci->as.content_item.action_expr = tail;
                            }
                        }
                        /* Consume any still-unread tokens on this line (unknown
                         * shapes fall through to here). */
                        while (!at(p, TOK_NEWLINE) && !at(p, TOK_DEDENT) && !at(p, TOK_EOF))
                            advance(p);
                        while (at(p, TOK_NEWLINE)) advance(p);
                        /* Concept #49 — if this is a list and it has a
                         * nested indent block starting with `item IDENT ->`,
                         * parse the per-item template. Other kinds' nested
                         * blocks are still absorbed without unpacking. */
                        if (at(p, TOK_INDENT)) {
                            advance(p);
                            /* Concept #52 — optional `empty -> body` clause
                             * consumed before the `item` template. The body
                             * renders when the iterable is empty. */
                            if (ci->as.content_item.kind &&
                                strcmp(ci->as.content_item.kind, "list") == 0 &&
                                at(p, TOK_IDENT) && p->cur.text &&
                                strcmp(p->cur.text, "empty") == 0) {
                                advance(p);
                                if (at(p, TOK_ARROW)) advance(p);
                                /* Simple case: single inline expression. */
                                if (!at(p, TOK_NEWLINE) && !at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
                                    ast_node_t *sub = mknode(p, AST_CONTENT_ITEM);
                                    if (sub) {
                                        sub->as.content_item.kind = "raw";
                                        sub->as.content_item.label = NULL;
                                        sub->as.content_item.data_expr = NULL;
                                        sub->as.content_item.item_binder = NULL;
                                        ast_list_init(&sub->as.content_item.item_body);
                                        ast_list_init(&sub->as.content_item.empty_body);
                                        sub->as.content_item.on_submit = NULL;
                                        sub->as.content_item.options = NULL;
                                        sub->as.content_item.n_options = 0;
                                        sub->as.content_item.action_expr = parse_expr_prec(p, 0);
                                        ast_list_push(p->arena, &ci->as.content_item.empty_body, sub);
                                    }
                                }
                                while (at(p, TOK_NEWLINE)) advance(p);
                            }
                            bool is_list_template =
                                ci->as.content_item.kind &&
                                strcmp(ci->as.content_item.kind, "list") == 0 &&
                                at(p, TOK_IDENT) && p->cur.text &&
                                strcmp(p->cur.text, "item") == 0;
                            if (is_list_template) {
                                ast_list_init(&ci->as.content_item.item_body);
                                advance(p); /* item */
                                if (at(p, TOK_IDENT)) {
                                    ci->as.content_item.item_binder = p->cur.text;
                                    advance(p);
                                }
                                if (at(p, TOK_ARROW)) advance(p);
                                while (at(p, TOK_NEWLINE)) advance(p);
                                /* Body may be on the same line (single item)
                                 * or an indented suite of items. */
                                if (at(p, TOK_INDENT)) {
                                    advance(p);
                                    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
                                        if (at(p, TOK_NEWLINE)) { advance(p); continue; }
                                        ast_node_t *sub = mknode(p, AST_CONTENT_ITEM);
                                        if (!sub) return NULL;
                                        sub->as.content_item.kind = NULL;
                                        sub->as.content_item.label = NULL;
                                        sub->as.content_item.action_expr = NULL;
                                        sub->as.content_item.data_expr = NULL;
                                        sub->as.content_item.item_binder = NULL;
                                        ast_list_init(&sub->as.content_item.item_body);
                                        ast_list_init(&sub->as.content_item.empty_body);
                                        sub->as.content_item.on_submit = NULL;
                                        sub->as.content_item.options = NULL;
                                        sub->as.content_item.n_options = 0;
                                        if (at(p, TOK_IDENT)) {
                                            sub->as.content_item.kind = p->cur.text;
                                            advance(p);
                                        } else if (at(p, TOK_ATOM)) {
                                            sub->as.content_item.kind = p->cur.text;
                                            advance(p);
                                        } else {
                                            sub->as.content_item.kind = "raw";
                                        }
                                        if (at(p, TOK_STRING)) {
                                            sub->as.content_item.label = p->cur.text;
                                            advance(p);
                                        }
                                        if (at(p, TOK_ARROW)) {
                                            advance(p);
                                            ast_node_t *action = parse_expr_prec(p, 0);
                                            if (action) sub->as.content_item.action_expr = action;
                                        }
                                        if (!at(p, TOK_NEWLINE) && !at(p, TOK_DEDENT) && !at(p, TOK_EOF) &&
                                            !(at(p, TOK_IDENT) && peek_next_tok(p) == TOK_COLON)) {
                                            ast_node_t *d = parse_expr_prec(p, 0);
                                            if (d) {
                                                if (!sub->as.content_item.action_expr)
                                                    sub->as.content_item.action_expr = d;
                                                else
                                                    sub->as.content_item.data_expr = d;
                                            }
                                        }
                                        /* Concept #57 — per-item options (e.g.
                                         * `trigger x.name badge: x.count`). */
                                        parse_content_options(p, sub);
                                        /* Concept #58 — inline trailing
                                         * `[on] -> action` for per-item shapes
                                         * like `trigger x.name badge: N -> act`. */
                                        if (at(p, TOK_KW_ON)) { advance(p); if (at(p, TOK_IDENT) || at(p, TOK_ATOM)) advance(p); }
                                        if (at(p, TOK_ARROW)) {
                                            advance(p);
                                            ast_node_t *tail = parse_expr_prec(p, 0);
                                            if (tail) {
                                                if (sub->as.content_item.action_expr &&
                                                    !sub->as.content_item.data_expr)
                                                    sub->as.content_item.data_expr =
                                                        sub->as.content_item.action_expr;
                                                sub->as.content_item.action_expr = tail;
                                            }
                                        }
                                        while (!at(p, TOK_NEWLINE) && !at(p, TOK_DEDENT) && !at(p, TOK_EOF))
                                            advance(p);
                                        while (at(p, TOK_NEWLINE)) advance(p);
                                        ast_list_push(p->arena, &ci->as.content_item.item_body, sub);
                                    }
                                    if (at(p, TOK_DEDENT)) advance(p);
                                } else {
                                    /* Inline item body on the same line. */
                                    while (!at(p, TOK_NEWLINE) && !at(p, TOK_EOF) && !at(p, TOK_DEDENT))
                                        advance(p);
                                    while (at(p, TOK_NEWLINE)) advance(p);
                                }
                                /* Consume trailing tokens + any still-open
                                 * outer depth of the list's indent block. */
                                uint32_t depth = 1;
                                while (depth > 0 && !at(p, TOK_EOF)) {
                                    if (at(p, TOK_INDENT))      { depth++; advance(p); }
                                    else if (at(p, TOK_DEDENT)) { depth--; advance(p); }
                                    else                        { advance(p); }
                                }
                            } else {
                                /* Concept #57 — non-list indented block.
                                 * Parse continuation-line options
                                 * (`badge: expr`, `value: expr`, etc.)
                                 * and a trailing `-> action` line as
                                 * part of the current item. Unknown
                                 * lines and deeper nested indents are
                                 * absorbed to preserve the legacy
                                 * round-trip behaviour. */
                                while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
                                    while (at(p, TOK_NEWLINE)) advance(p);
                                    if (at(p, TOK_DEDENT) || at(p, TOK_EOF)) break;
                                    if (at(p, TOK_IDENT) && peek_next_tok(p) == TOK_COLON) {
                                        parse_content_options(p, ci);
                                        while (at(p, TOK_NEWLINE)) advance(p);
                                        continue;
                                    }
                                    if (at(p, TOK_KW_ON)) { advance(p); if (at(p, TOK_IDENT) || at(p, TOK_ATOM)) advance(p); }
                                    if (at(p, TOK_ARROW)) {
                                        advance(p);
                                        ast_node_t *action = parse_expr_prec(p, 0);
                                        if (action) {
                                            /* If action_expr already
                                             * held a label expression
                                             * (e.g. `trigger app.name`
                                             * captured it as data),
                                             * preserve the old one in
                                             * data_expr so the walker
                                             * can use it as the label
                                             * while the new one is the
                                             * real tap action. */
                                            if (ci->as.content_item.action_expr &&
                                                !ci->as.content_item.data_expr)
                                                ci->as.content_item.data_expr =
                                                    ci->as.content_item.action_expr;
                                            ci->as.content_item.action_expr = action;
                                        }
                                        while (at(p, TOK_NEWLINE)) advance(p);
                                        continue;
                                    }
                                    /* Fallback — absorb unrecognised
                                     * shapes. Deeper indents (blocks
                                     * inside blocks) counted with a
                                     * local depth so we resync on the
                                     * matching dedent. */
                                    uint32_t depth = 0;
                                    while (!at(p, TOK_EOF)) {
                                        if (at(p, TOK_INDENT))      { depth++; advance(p); }
                                        else if (at(p, TOK_DEDENT)) {
                                            if (depth == 0) break;
                                            depth--; advance(p);
                                        } else if (at(p, TOK_NEWLINE) && depth == 0) {
                                            advance(p); break;
                                        } else {
                                            advance(p);
                                        }
                                    }
                                }
                                if (at(p, TOK_DEDENT)) advance(p);
                            }
                        }
                        ast_list_push(p->arena, &cb->as.content_block.items, ci);
                    }
                    if (at(p, TOK_DEDENT)) advance(p);
                }
            } else {
                /* Inline RHS — a single expression. Capture as a single item
                 * with kind="raw" and action_expr = the expression. */
                ast_node_t *ci = mknode(p, AST_CONTENT_ITEM); if (!ci) return NULL;
                ci->as.content_item.kind = "raw";
                ci->as.content_item.label = NULL;
                ci->as.content_item.data_expr = NULL;
                ci->as.content_item.item_binder = NULL;
                ast_list_init(&ci->as.content_item.item_body);
                ast_list_init(&ci->as.content_item.empty_body);
                ci->as.content_item.on_submit = NULL;
                ci->as.content_item.options = NULL;
                ci->as.content_item.n_options = 0;
                ast_node_t *expr = parse_expr_prec(p, 0);
                ci->as.content_item.action_expr = expr;
                ast_list_push(p->arena, &cb->as.content_block.items, ci);
                while (!at(p, TOK_NEWLINE) && !at(p, TOK_EOF) && !at(p, TOK_DEDENT))
                    advance(p);
                while (at(p, TOK_NEWLINE)) advance(p);
            }
            /* Attach the content block to the state so the runtime can
             * retrieve it at state entry. Stored in `state.hooks` alongside
             * the existing on-enter / on-leave / transition nodes; the
             * evaluator distinguishes by AST kind. */
            ast_list_push(p->arena, &st->as.state.hooks, cb);
        } else {
            set_err(p, DECK_LOAD_PARSE,
                    "expected `on`, `transition`, or `content =` in state body");
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
    if (!at(p, TOK_IDENT)) { set_err(p, DECK_LOAD_PARSE, "expected machine name"); return NULL; }
    m->as.machine.name = p->cur.text;
    advance(p);
    if (!expect(p, TOK_NEWLINE, "expected newline after machine name")) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);
    if (!expect(p, TOK_INDENT, "expected indented machine body")) return NULL;
    ast_list_init(&m->as.machine.states);
    ast_list_init(&m->as.machine.transitions);
    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
        if (at(p, TOK_KW_STATE)) {
            ast_node_t *st = parse_state_decl(p); if (!st) return NULL;
            ast_list_push(p->arena, &m->as.machine.states, st);
            while (at(p, TOK_NEWLINE)) advance(p);
            continue;
        }
        /* Spec 02-deck-app §8.2 — top-level `initial :atom` declaration.
         * `initial` is a bare ident, not a reserved keyword. */
        if (at(p, TOK_IDENT) && p->cur.text &&
            strcmp(p->cur.text, "initial") == 0) {
            advance(p); /* initial */
            if (!at(p, TOK_ATOM)) {
                set_err(p, DECK_LOAD_PARSE,
                        "expected `:atom` after `initial` in @machine body");
                return NULL;
            }
            if (m->as.machine.initial_state) {
                set_err(p, DECK_LOAD_PARSE,
                        "@machine declares `initial` more than once");
                return NULL;
            }
            m->as.machine.initial_state = p->cur.text;
            advance(p);
            while (at(p, TOK_NEWLINE)) advance(p);
            continue;
        }
        /* Spec 02-deck-app §8.4 — top-level `transition :event …` with
         * optional `(args)` and an indented clause block of
         * from:/to:/when:/before:/after:/watch:. Concept #44 — store the
         * transition as AST_MACHINE_TRANSITION. Deferred: (args) payload
         * binding, `from *` wildcard, multi-from, `watch:` reactive,
         * `to history`, before/after as full expression bodies (currently
         * parsed as a single expression on the same line for simplicity). */
        if (at(p, TOK_KW_TRANSITION)) {
            ast_node_t *tn = mknode(p, AST_MACHINE_TRANSITION); if (!tn) return NULL;
            tn->as.machine_transition.event = NULL;
            tn->as.machine_transition.from_state = NULL;
            tn->as.machine_transition.to_state = NULL;
            tn->as.machine_transition.when_expr = NULL;
            tn->as.machine_transition.before_body = NULL;
            tn->as.machine_transition.after_body = NULL;
            advance(p); /* transition */
            /* `:event_atom` */
            if (at(p, TOK_ATOM)) { tn->as.machine_transition.event = p->cur.text; advance(p); }
            /* Optional `(args)` — eat matching parens (payload binding deferred). */
            if (at(p, TOK_LPAREN)) {
                advance(p);
                uint32_t depth = 1;
                while (depth > 0 && !at(p, TOK_EOF)) {
                    if (at(p, TOK_LPAREN)) depth++;
                    else if (at(p, TOK_RPAREN)) { if (--depth == 0) break; }
                    advance(p);
                }
                if (!expect(p, TOK_RPAREN,
                            "expected ')' closing top-level transition args"))
                    return NULL;
            }
            /* Trailing text on the header line up to newline. Consume. */
            while (!at(p, TOK_NEWLINE) && !at(p, TOK_EOF) && !at(p, TOK_DEDENT))
                advance(p);
            while (at(p, TOK_NEWLINE)) advance(p);
            /* Clause block — parse known keys, discard unknowns. */
            if (at(p, TOK_INDENT)) {
                advance(p);
                while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
                    /* Each clause is `ident: value` on its own line. */
                    if (at(p, TOK_IDENT) && p->cur.text) {
                        const char *key = p->cur.text;
                        advance(p);
                        if (at(p, TOK_COLON)) advance(p);
                        if (strcmp(key, "from") == 0) {
                            if (at(p, TOK_ATOM)) { tn->as.machine_transition.from_state = p->cur.text; advance(p); }
                            else if (at(p, TOK_STAR)) { tn->as.machine_transition.from_state = NULL; advance(p); }
                        } else if (strcmp(key, "to") == 0) {
                            if (at(p, TOK_ATOM)) { tn->as.machine_transition.to_state = p->cur.text; advance(p); }
                        } else if (strcmp(key, "when") == 0) {
                            ast_node_t *e = parse_expr_prec(p, 0);
                            if (e) tn->as.machine_transition.when_expr = e;
                        } else if (strcmp(key, "before") == 0) {
                            ast_node_t *e = parse_expr_prec(p, 0);
                            if (e) tn->as.machine_transition.before_body = e;
                        } else if (strcmp(key, "after") == 0) {
                            ast_node_t *e = parse_expr_prec(p, 0);
                            if (e) tn->as.machine_transition.after_body = e;
                        }
                        /* Skip any trailing tokens on this clause line. */
                        while (!at(p, TOK_NEWLINE) && !at(p, TOK_DEDENT) && !at(p, TOK_EOF))
                            advance(p);
                    }
                    while (at(p, TOK_NEWLINE)) advance(p);
                }
                if (at(p, TOK_DEDENT)) advance(p);
            }
            /* Only store if we got at least event + to_state. from is optional
             * (defaults to wildcard when the clause was absent). */
            if (tn->as.machine_transition.event && tn->as.machine_transition.to_state) {
                ast_list_push(p->arena, &m->as.machine.transitions, tn);
            }
            continue;
        }
        /* LANG §13.3 — `on :event from :src to :dst` top-level form.
         * Lowered to AST_MACHINE_TRANSITION with optional indented
         * before:/after: body identical to the `transition` form. */
        if (at(p, TOK_KW_ON)) {
            ast_node_t *tn = mknode(p, AST_MACHINE_TRANSITION); if (!tn) return NULL;
            tn->as.machine_transition.event = NULL;
            tn->as.machine_transition.from_state = NULL;
            tn->as.machine_transition.to_state = NULL;
            tn->as.machine_transition.when_expr = NULL;
            tn->as.machine_transition.before_body = NULL;
            tn->as.machine_transition.after_body = NULL;
            advance(p); /* on */
            if (!at(p, TOK_ATOM)) {
                set_err(p, DECK_LOAD_PARSE,
                        "expected `:event` after `on` in @machine body");
                return NULL;
            }
            tn->as.machine_transition.event = p->cur.text;
            advance(p);
            /* `from :src` */
            if (at(p, TOK_IDENT) && p->cur.text &&
                strcmp(p->cur.text, "from") == 0) {
                advance(p);
                if (at(p, TOK_ATOM)) {
                    tn->as.machine_transition.from_state = p->cur.text;
                    advance(p);
                } else if (at(p, TOK_STAR)) {
                    advance(p);
                }
            }
            /* `to :dst` */
            if (at(p, TOK_IDENT) && p->cur.text &&
                strcmp(p->cur.text, "to") == 0) {
                advance(p);
                if (at(p, TOK_ATOM)) {
                    tn->as.machine_transition.to_state = p->cur.text;
                    advance(p);
                }
            }
            while (!at(p, TOK_NEWLINE) && !at(p, TOK_EOF) && !at(p, TOK_DEDENT))
                advance(p);
            while (at(p, TOK_NEWLINE)) advance(p);
            /* Optional indented before:/after:/when: clause block. */
            if (at(p, TOK_INDENT)) {
                advance(p);
                while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
                    if (at(p, TOK_IDENT) && p->cur.text) {
                        const char *key = p->cur.text;
                        advance(p);
                        if (at(p, TOK_COLON)) advance(p);
                        if (strcmp(key, "before") == 0) {
                            tn->as.machine_transition.before_body = parse_suite(p);
                        } else if (strcmp(key, "after") == 0) {
                            tn->as.machine_transition.after_body = parse_suite(p);
                        } else if (strcmp(key, "when") == 0) {
                            tn->as.machine_transition.when_expr = parse_expr_prec(p, 0);
                        } else {
                            /* Unknown clause — skip to next NEWLINE. */
                            while (!at(p, TOK_NEWLINE) && !at(p, TOK_EOF) &&
                                   !at(p, TOK_DEDENT))
                                advance(p);
                        }
                        while (at(p, TOK_NEWLINE)) advance(p);
                    } else {
                        advance(p);
                    }
                }
                if (at(p, TOK_DEDENT)) advance(p);
            }
            if (tn->as.machine_transition.event &&
                tn->as.machine_transition.to_state) {
                ast_list_push(p->arena, &m->as.machine.transitions, tn);
            }
            continue;
        }
        set_err(p, DECK_LOAD_PARSE,
                "expected `state`, `initial`, or `transition` in @machine body");
        return NULL;
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
/* Spec §5 — type annotations may be ident, dotted path, `[T]`,
 * `(A, B, ...)`, `{K: V}`, `T?`, `Result T E`, etc. At F21.1 the runtime
 * is dynamic, so types are parsed and discarded. This helper eats a
 * balanced type expression up to the next parameter separator
 * (`,`, `)`, `!`, `->`, `=`, NEWLINE, DEDENT). Brackets/parens/braces
 * nest freely inside. */
static bool skip_type_annotation(deck_parser_t *p)
{
    uint32_t bd = 0;   /* bracket depth: () [] {} */
    while (!at(p, TOK_EOF)) {
        if (bd == 0) {
            /* LANG §4.4 — function-type annotations like `f: (int) -> int`
             * include `->` after the parens. Recognise that pattern by
             * looking for `->` AFTER we've already closed the function
             * signature paren (bd==0 with previous content); the
             * shortcut: never treat `->` as a terminator — let it
             * continue, and walk past the result type. The downside is
             * a `let x = a -> b` would absorb the `->` too, but `->`
             * outside a fn-type or arm body has no other meaning in
             * this grammar, so this is safe. */
            if (at(p, TOK_COMMA) || at(p, TOK_RPAREN) ||
                at(p, TOK_BANG)  ||
                at(p, TOK_ASSIGN) || at(p, TOK_NEWLINE) ||
                at(p, TOK_DEDENT))
                return true;
        }
        if (at(p, TOK_LPAREN) || at(p, TOK_LBRACKET) || at(p, TOK_LBRACE)) bd++;
        else if (at(p, TOK_RBRACKET) || at(p, TOK_RBRACE))                bd--;
        else if (at(p, TOK_RPAREN) && bd > 0)                             bd--;
        advance(p);
    }
    return true;
}

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
        set_err(p, DECK_LOAD_PARSE,
                "expected function name or '(' after 'fn'");
        return NULL;
    }
    if (!expect(p, TOK_LPAREN, "expected '(' after fn name")) return NULL;

    const char *params_buf[16];
    uint32_t n_params = 0;
    if (!at(p, TOK_RPAREN)) {
        for (;;) {
            if (n_params >= 16) {
                set_err(p, DECK_LOAD_PARSE, "too many parameters (max 16)");
                return NULL;
            }
            if (!at(p, TOK_IDENT)) {
                set_err(p, DECK_LOAD_PARSE, "expected parameter name");
                return NULL;
            }
            params_buf[n_params++] = p->cur.text;
            advance(p);
            /* Optional `: Type` — spec §5 supports complex types ([T],
             * (A,B), {K:V}, T?, Result T E). Runtime is dynamic at F21.1
             * so we parse-and-discard via a balanced skip. */
            if (at(p, TOK_COLON)) {
                advance(p);
                if (!skip_type_annotation(p)) return NULL;
                /* LANG §2.6 — fn-type annotations carry an optional
                 * purity bit (`(int) -> unit !`). Consume it so the
                 * outer comma/rparen check sees the right token. */
                if (at(p, TOK_BANG)) advance(p);
            }
            if (!at(p, TOK_COMMA)) break;
            advance(p);
        }
    }
    if (!expect(p, TOK_RPAREN, "expected ')' after fn parameters")) return NULL;

    /* Optional `-> Type`. Spec §5 complex types — parse-and-discard. */
    if (at(p, TOK_ARROW)) {
        advance(p);
        if (!skip_type_annotation(p)) return NULL;
    }

    /* LANG §2.6 — `!` on a fn signature is a single purity bit. A bare
     * `!` declares the fn impure; the loader derives which capabilities
     * it touches from the body (no alias list required). DL2 F23 also
     * accepts legacy `!alias` annotations as a refinement and carries
     * them forward for loader cross-check. */
    const char *effects_buf[8];
    uint32_t n_effects = 0;
    bool has_bang = false;
    while (at(p, TOK_BANG)) {
        has_bang = true;
        advance(p);
        if (at(p, TOK_IDENT)) {
            if (n_effects >= 8) {
                set_err(p, DECK_LOAD_PARSE, "too many effect annotations (max 8)");
                return NULL;
            }
            effects_buf[n_effects++] = p->cur.text;
            advance(p);
        }
        /* No ident after `!` → bare purity bit; no alias captured. */
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
    n->as.fndef.has_bang = has_bang;
    n->as.fndef.body     = body;
    return n;
}

/* ================================================================
 * Stage 5b — structured parsers for @grants / @config / @errors /
 *            @handles / @service
 *
 * All five previously routed to parse_opaque_block (bodies discarded)
 * or parse_metadata_block (best-effort skip). These replace the skip
 * with real AST binding per LANG.md §10 / §12 / §2.4 / §19 / §18, so
 * downstream stages (6b stub services, 7 @on back routing, 8 bridge
 * diff using @handles URL matching) have structured data to consume.
 * ================================================================ */

/* Small option-pair accumulator reused across @grants / @config. Each
 * option is `key: expr` where key is an interned ident and expr is a
 * full Deck expression. */
typedef struct {
    ast_content_option_t *items;
    uint32_t              len;
    uint32_t              cap;
} opt_buf_t;

static bool opt_buf_push(deck_parser_t *p, opt_buf_t *b, const char *key, ast_node_t *val)
{
    if (b->len >= b->cap) {
        uint32_t nc = b->cap ? b->cap * 2 : 8;
        ast_content_option_t *ni = deck_arena_alloc(p->arena, nc * sizeof(*ni));
        if (!ni) { set_err(p, DECK_LOAD_RESOURCE, "option buf alloc"); return false; }
        if (b->items) memcpy(ni, b->items, b->len * sizeof(*ni));
        b->items = ni; b->cap = nc;
    }
    b->items[b->len].key = key;
    b->items[b->len].value = val;
    b->len++;
    return true;
}

/* Accept an inline dotted-ident chain and intern it as a single string
 * ("services.fs", "location", "system.security"). Used by @grants for
 * entry names. Does NOT consume the trailing colon. */
static const char *parse_dotted_name(deck_parser_t *p)
{
    if (!at(p, TOK_IDENT)) return NULL;
    char buf[128];
    uint32_t n = (uint32_t)snprintf(buf, sizeof(buf), "%s", p->cur.text);
    advance(p);
    while (at(p, TOK_DOT)) {
        advance(p);
        if (!at(p, TOK_IDENT)) return NULL;
        n += (uint32_t)snprintf(buf + n, sizeof(buf) - n, ".%s", p->cur.text);
        advance(p);
    }
    return deck_intern(buf, n);
}

/* Parse one `key: expr` pair at the current indent level. Consumes
 * trailing NEWLINE(s) if present. Returns true on success. */
static bool parse_kv_option(deck_parser_t *p, const char **out_key, ast_node_t **out_val)
{
    if (!at(p, TOK_IDENT)) { set_err(p, DECK_LOAD_PARSE, "expected key name"); return false; }
    *out_key = p->cur.text;
    advance(p);
    if (!expect(p, TOK_COLON, "expected ':' after option key")) return false;
    ast_node_t *expr = deck_parser_parse_expr(p);
    if (!expr) return false;
    *out_val = expr;
    skip_newlines(p);
    return true;
}

/* ---- @grants (LANG §10) -----------------------------------------
 *   @grants
 *     <dotted_name>:
 *       <key>: <expr>
 *       ...
 *     <dotted_name>:
 *       ...
 */
static ast_node_t *parse_grants_decl(deck_parser_t *p)
{
    advance(p); /* @grants */
    if (!expect(p, TOK_NEWLINE, "expected newline after @grants")) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);
    ast_node_t *n = mknode(p, AST_GRANTS); if (!n) return NULL;
    n->as.grants.n_entries = 0;
    n->as.grants.options_total = 0;
    if (!at(p, TOK_INDENT)) return n;  /* empty block */
    advance(p); /* INDENT */

    const char *names[32];
    uint32_t    offs[32];
    uint32_t    lens[32];
    uint32_t    ne = 0;
    opt_buf_t   opts = {0};

    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
        if (ne >= 32) { set_err(p, DECK_LOAD_PARSE, "too many @grants entries (max 32)"); return NULL; }
        const char *name = parse_dotted_name(p);
        if (!name) { set_err(p, DECK_LOAD_PARSE, "expected grant entry name"); return NULL; }
        if (!expect(p, TOK_COLON, "expected ':' after grant entry name")) return NULL;
        if (!expect(p, TOK_NEWLINE, "expected newline after grant entry ':'")) return NULL;
        while (at(p, TOK_NEWLINE)) advance(p);

        /* J8 — `services:` followed by a quoted-string-keyed sub-block
         * is the spec-first form: each `"<id>": <alias>` line becomes
         * a separate `services.<id>` grant entry with an `alias` option
         * carrying the alias ident. Symmetric to @needs.services. */
        if (strcmp(name, "services") == 0 && at(p, TOK_INDENT)) {
            advance(p); /* INDENT */
            while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
                if (at(p, TOK_NEWLINE)) { advance(p); continue; }
                if (!at(p, TOK_STRING)) {
                    set_err(p, DECK_LOAD_PARSE,
                            "expected quoted service id in @grants services block");
                    return NULL;
                }
                if (ne >= 32) { set_err(p, DECK_LOAD_PARSE, "too many @grants entries (max 32)"); return NULL; }
                const char *svc_id = p->cur.text;
                advance(p);
                if (!expect(p, TOK_COLON, "expected ':' after service id")) return NULL;
                /* RHS may be an ident (alias) or a full expression. */
                ast_node_t *rhs = parse_expr_prec(p, 0);
                if (!rhs) return NULL;
                /* Build a fully-qualified entry name. */
                char qbuf[160];
                snprintf(qbuf, sizeof(qbuf), "services.%s", svc_id);
                names[ne] = deck_intern(qbuf, (uint32_t)strlen(qbuf));
                offs[ne]  = opts.len;
                if (!opt_buf_push(p, &opts, deck_intern_cstr("alias"), rhs)) return NULL;
                lens[ne] = opts.len - offs[ne];
                ne++;
                while (at(p, TOK_NEWLINE)) advance(p);
            }
            if (!expect(p, TOK_DEDENT, "expected dedent closing services block")) return NULL;
            while (at(p, TOK_NEWLINE)) advance(p);
            continue;
        }

        names[ne] = name;
        offs[ne]  = opts.len;

        if (at(p, TOK_INDENT)) {
            advance(p);
            while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
                const char *k = NULL; ast_node_t *v = NULL;
                if (!parse_kv_option(p, &k, &v)) return NULL;
                if (!opt_buf_push(p, &opts, k, v)) return NULL;
            }
            if (!expect(p, TOK_DEDENT, "expected dedent inside grant entry")) return NULL;
        }
        lens[ne] = opts.len - offs[ne];
        ne++;
        while (at(p, TOK_NEWLINE)) advance(p);
    }
    if (!expect(p, TOK_DEDENT, "expected dedent closing @grants")) return NULL;

    n->as.grants.n_entries = ne;
    n->as.grants.options_total = opts.len;
    if (ne) {
        n->as.grants.names = deck_arena_memdup(p->arena, names, ne * sizeof(*names));
        n->as.grants.options_offset = deck_arena_memdup(p->arena, offs, ne * sizeof(*offs));
        n->as.grants.n_options = deck_arena_memdup(p->arena, lens, ne * sizeof(*lens));
    }
    if (opts.len) {
        n->as.grants.options = deck_arena_memdup(p->arena, opts.items,
                                                  opts.len * sizeof(*opts.items));
    }
    return n;
}

/* ---- @config (LANG §12) -----------------------------------------
 *   @config
 *     <name> : <type> = <default> [key: expr ...]
 *
 * We parse-and-discard the type annotation (use the existing
 * skip_type_annotation helper), then demand `=` followed by a default
 * expression. Constraint options (min/max/min_length/...) are captured
 * into the same ast_content_option_t pool shape used by @grants.
 */
static ast_node_t *parse_config_decl(deck_parser_t *p)
{
    advance(p); /* @config */
    if (!expect(p, TOK_NEWLINE, "expected newline after @config")) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);
    ast_node_t *n = mknode(p, AST_CONFIG); if (!n) return NULL;
    n->as.config.n_entries = 0;
    n->as.config.options_total = 0;
    if (!at(p, TOK_INDENT)) return n;
    advance(p);

    const char *names[32];
    ast_node_t *defs[32];
    uint32_t    offs[32];
    uint32_t    lens[32];
    uint32_t    ne = 0;
    opt_buf_t   opts = {0};

    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
        if (ne >= 32) { set_err(p, DECK_LOAD_PARSE, "too many @config entries (max 32)"); return NULL; }
        if (!at(p, TOK_IDENT)) { set_err(p, DECK_LOAD_PARSE, "expected config field name"); return NULL; }
        names[ne] = p->cur.text;
        advance(p);
        if (!expect(p, TOK_COLON, "expected ':' after @config field")) return NULL;
        if (!skip_type_annotation(p)) return NULL;
        if (!expect(p, TOK_ASSIGN, "expected '=' before @config default")) return NULL;
        defs[ne] = deck_parser_parse_expr(p);
        if (!defs[ne]) return NULL;
        offs[ne] = opts.len;
        /* Inline modifiers (`min: 0  max: 10`) — must stay on the same
         * line as the default value; a NEWLINE means the next entry
         * starts. parse_kv_option's internal skip_newlines was eating
         * the line terminator, so the next field name was being
         * consumed as a modifier value. */
        while (!at(p, TOK_NEWLINE) && !at(p, TOK_DEDENT) && !at(p, TOK_EOF)
               && at(p, TOK_IDENT) && peek_next_tok(p) == TOK_COLON) {
            const char *kk = p->cur.text;
            advance(p); /* key */
            advance(p); /* : */
            /* Same single-token literal restriction as @assets to
             * avoid the variant-constructor heuristic. */
            ast_node_t *v = NULL;
            switch (p->cur.type) {
                case TOK_STRING: case TOK_INT: case TOK_FLOAT:
                case TOK_ATOM:   case TOK_KW_TRUE: case TOK_KW_FALSE:
                case TOK_KW_UNIT:case TOK_KW_NONE:
                    v = mknode(p, AST_LIT_NONE); /* placeholder shape */
                    advance(p);
                    break;
                default:
                    set_err(p, DECK_LOAD_PARSE,
                            "expected literal value for @config modifier");
                    return NULL;
            }
            if (!opt_buf_push(p, &opts, kk, v)) return NULL;
            if (at(p, TOK_COMMA)) advance(p);
        }
        lens[ne] = opts.len - offs[ne];
        ne++;
        while (at(p, TOK_NEWLINE)) advance(p);
    }
    if (!expect(p, TOK_DEDENT, "expected dedent closing @config")) return NULL;

    n->as.config.n_entries = ne;
    n->as.config.options_total = opts.len;
    if (ne) {
        n->as.config.names = deck_arena_memdup(p->arena, names, ne * sizeof(*names));
        n->as.config.defaults = deck_arena_memdup(p->arena, defs, ne * sizeof(*defs));
        n->as.config.options_offset = deck_arena_memdup(p->arena, offs, ne * sizeof(*offs));
        n->as.config.n_options = deck_arena_memdup(p->arena, lens, ne * sizeof(*lens));
    }
    if (opts.len) {
        n->as.config.options = deck_arena_memdup(p->arena, opts.items,
                                                  opts.len * sizeof(*opts.items));
    }
    return n;
}

/* ---- @errors <domain> (LANG §2.4) -------------------------------
 *   @errors <domain_ident>
 *     :variant1 "description"
 *     :variant2 "description"
 *     ...
 */
static ast_node_t *parse_errors_decl(deck_parser_t *p)
{
    advance(p); /* @errors */
    const char *domain = parse_dotted_name(p);
    if (!domain) { set_err(p, DECK_LOAD_PARSE, "expected @errors <domain>"); return NULL; }
    if (!expect(p, TOK_NEWLINE, "expected newline after @errors domain")) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);

    ast_node_t *n = mknode(p, AST_ERRORS); if (!n) return NULL;
    n->as.errors.domain = domain;
    n->as.errors.n_variants = 0;

    if (!at(p, TOK_INDENT)) return n;
    advance(p);

    const char *vs[64];
    const char *ds[64];
    uint32_t    nv = 0;

    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
        if (nv >= 64) { set_err(p, DECK_LOAD_PARSE, "too many @errors variants (max 64)"); return NULL; }
        if (!at(p, TOK_ATOM)) { set_err(p, DECK_LOAD_PARSE, "expected :atom variant in @errors"); return NULL; }
        vs[nv] = p->cur.text;
        advance(p);
        if (at(p, TOK_STRING)) {
            ds[nv] = p->cur.text;
            advance(p);
        } else {
            ds[nv] = NULL;
        }
        nv++;
        while (at(p, TOK_NEWLINE)) advance(p);
    }
    if (!expect(p, TOK_DEDENT, "expected dedent closing @errors")) return NULL;

    n->as.errors.n_variants = nv;
    if (nv) {
        n->as.errors.variants     = deck_arena_memdup(p->arena, vs, nv * sizeof(*vs));
        n->as.errors.descriptions = deck_arena_memdup(p->arena, ds, nv * sizeof(*ds));
    }
    return n;
}

/* ---- @handles (LANG §19) ----------------------------------------
 *   @handles
 *     "bsky://profile/{handle}"
 *     "https://bsky.app/profile/{handle}"
 */
static ast_node_t *parse_handles_decl(deck_parser_t *p)
{
    advance(p); /* @handles */
    if (!expect(p, TOK_NEWLINE, "expected newline after @handles")) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);

    ast_node_t *n = mknode(p, AST_HANDLES); if (!n) return NULL;
    n->as.handles.n_patterns = 0;
    if (!at(p, TOK_INDENT)) return n;
    advance(p);

    const char *ps[32];
    uint32_t np = 0;
    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
        if (np >= 32) { set_err(p, DECK_LOAD_PARSE, "too many @handles patterns (max 32)"); return NULL; }
        if (!at(p, TOK_STRING)) { set_err(p, DECK_LOAD_PARSE, "expected URL pattern string in @handles"); return NULL; }
        ps[np++] = p->cur.text;
        advance(p);
        while (at(p, TOK_NEWLINE)) advance(p);
    }
    if (!expect(p, TOK_DEDENT, "expected dedent closing @handles")) return NULL;

    n->as.handles.n_patterns = np;
    if (np) n->as.handles.patterns = deck_arena_memdup(p->arena, ps, np * sizeof(*ps));
    return n;
}

/* ---- @service "<id>" (LANG §18) ---------------------------------
 *   @service "<service-id>"
 *     allow: <expr>
 *     keep:  <bool>
 *
 *     on :method_name (params)
 *       -> ReturnType !
 *       = body
 *
 * Methods reuse parse_on_decl since the grammar is identical ("on :atom
 * (params) -> T ! = body"). Only the containing construct differs.
 */
static ast_node_t *parse_service_decl(deck_parser_t *p)
{
    advance(p); /* @service */
    if (!at(p, TOK_STRING)) { set_err(p, DECK_LOAD_PARSE, "expected quoted service id after @service"); return NULL; }
    const char *sid = p->cur.text;
    advance(p);
    if (!expect(p, TOK_NEWLINE, "expected newline after @service id")) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);

    ast_node_t *n = mknode(p, AST_SERVICE); if (!n) return NULL;
    n->as.service.service_id = sid;
    n->as.service.allow_expr = NULL;
    n->as.service.keep = false;
    ast_list_init(&n->as.service.methods);

    if (!at(p, TOK_INDENT)) return n;
    advance(p);

    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
        /* allow: / keep: */
        if (at(p, TOK_IDENT) && peek_next_tok(p) == TOK_COLON) {
            const char *k = p->cur.text;
            advance(p); advance(p); /* ident : */
            if (strcmp(k, "allow") == 0) {
                ast_node_t *e = deck_parser_parse_expr(p);
                if (!e) return NULL;
                n->as.service.allow_expr = e;
            } else if (strcmp(k, "keep") == 0) {
                if (at(p, TOK_KW_TRUE))       { n->as.service.keep = true;  advance(p); }
                else if (at(p, TOK_KW_FALSE)) { n->as.service.keep = false; advance(p); }
                else { set_err(p, DECK_LOAD_PARSE, "keep: expects true/false"); return NULL; }
            } else {
                /* Unknown key — accept with expression to stay forward-
                 * compatible; discard. */
                ast_node_t *e = deck_parser_parse_expr(p);
                if (!e) return NULL;
            }
            while (at(p, TOK_NEWLINE)) advance(p);
            continue;
        }
        /* on :method — reuse parse_on_decl. The parser sees TOK_DECORATOR
         * for "@on", but inside @service methods the spec syntax is
         * `on :name (params) ... = body` (no leading @). parse_on_decl
         * expects a leading "@on" decorator token. For minimal structural
         * coverage, skip any `on ...` body at the correct indent depth. */
        if (at(p, TOK_IDENT) && strcmp(p->cur.text, "on") == 0) {
            /* Consume `on :atom (...) ... = body`. We're not fully
             * validating the method here — Stage 5b ensures the
             * envelope parses; method-level binding lands when service
             * dispatch is wired (later stage). */
            int depth = 0;
            while (!at(p, TOK_EOF)) {
                if (at(p, TOK_INDENT)) { depth++; advance(p); continue; }
                if (at(p, TOK_DEDENT)) {
                    if (depth == 0) break;
                    depth--; advance(p); continue;
                }
                if (depth == 0 && at(p, TOK_NEWLINE)) {
                    advance(p);
                    if (!at(p, TOK_INDENT)) break;
                    continue;
                }
                advance(p);
            }
            continue;
        }
        /* Stray content — skip to next newline to stay resilient. */
        while (!at(p, TOK_NEWLINE) && !at(p, TOK_DEDENT) && !at(p, TOK_EOF)) advance(p);
        if (at(p, TOK_NEWLINE)) advance(p);
    }
    if (!expect(p, TOK_DEDENT, "expected dedent closing @service")) return NULL;
    return n;
}

/* DL2 F23.6 / F23.7 — `@grants` and `@errors` are documented as
 * indented blocks of `key: value` entries. F23 minimum: parse and
 * discard (metadata for future shell prompts / runtime cataloging). */
/* DL2 F28 — opaque indented block: consumes tokens until matching DEDENT,
 * returns a benign stub. Used for decorators whose body is not
 * interpreted today (@machine.before/.after, @flow, @flow.step,
 * @migrate, @assets). The decorator name is logged for the loader
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

/* DL2 F22.2 — `@type Name`<NEWLINE><INDENT> field: TypeName ... <DEDENT>.
 * Type annotations on fields are parsed and discarded (the runtime is
 * dynamic). Union types `T1 | T2` are also accepted and discarded. */
/* Skip a balanced parenthesised list, e.g. `(A, B, C)` or
 * `(left: Tree, right: Tree, value: int)`. Used to discard type-
 * parameter clauses and variant payload fields — the runtime is
 * dynamic so the structure is forgotten after parse. */
static bool skip_balanced_parens(deck_parser_t *p)
{
    if (!at(p, TOK_LPAREN)) return true;
    advance(p);
    int depth = 1;
    while (depth > 0 && !at(p, TOK_EOF)) {
        if (at(p, TOK_LPAREN)) depth++;
        else if (at(p, TOK_RPAREN)) {
            depth--;
            if (depth == 0) { advance(p); return true; }
        }
        advance(p);
    }
    set_err(p, DECK_LOAD_PARSE, "unterminated `(` in type clause");
    return false;
}

static ast_node_t *parse_type_decl(deck_parser_t *p)
{
    advance(p); /* @type */
    if (!at(p, TOK_IDENT)) {
        set_err(p, DECK_LOAD_PARSE, "expected type name after @type");
        return NULL;
    }
    ast_node_t *n = mknode(p, AST_TYPE_DEF); if (!n) return NULL;
    n->as.typedef_.name = p->cur.text;
    advance(p);

    /* LANG §2.5 — optional type-parameter clause `(A, B, ...)`. The
     * runtime is dynamic so parameters are parse-and-discard; we just
     * consume the balanced parens. */
    if (at(p, TOK_LPAREN)) {
        if (!skip_balanced_parens(p)) return NULL;
    }

    /* Three body forms after the (optional) type-parameter clause:
     *   1. `@type Name\n  field : T\n  field : T\n`           (record body, indented)
     *   2. `@type Name = | :variant\n               | :variant (payload)` (variant body)
     *   3. `@type Name = T`                                    (alias body — single line)
     * The variant + alias forms start with `=`; the record form goes
     * straight to NEWLINE + INDENT. */
    if (at(p, TOK_ASSIGN)) {
        advance(p); /* = */
        /* Variant body opens with `|` (possibly preceded by NEWLINE +
         * INDENT). Alias body is a single type expression on the same
         * line (or indented across lines). Both are parse-and-discard. */
        bool any_indent = false;
        while (at(p, TOK_NEWLINE)) advance(p);
        if (at(p, TOK_INDENT)) { advance(p); any_indent = true; }
        /* Eat the body until DEDENT/EOF or — for the inline alias form —
         * NEWLINE at the top level. */
        int paren_depth = 0;
        while (!at(p, TOK_EOF)) {
            if (any_indent && at(p, TOK_DEDENT) && paren_depth == 0) break;
            if (!any_indent && at(p, TOK_NEWLINE) && paren_depth == 0) break;
            if (at(p, TOK_LPAREN) || at(p, TOK_LBRACKET) || at(p, TOK_LBRACE)) paren_depth++;
            else if (at(p, TOK_RPAREN) || at(p, TOK_RBRACKET) || at(p, TOK_RBRACE)) {
                if (paren_depth > 0) paren_depth--;
            }
            advance(p);
        }
        if (any_indent && at(p, TOK_DEDENT)) advance(p);
        while (at(p, TOK_NEWLINE)) advance(p);
        n->as.typedef_.fields   = NULL;
        n->as.typedef_.n_fields = 0;
        return n;
    }

    if (!expect(p, TOK_NEWLINE, "expected newline after @type name")) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);
    /* Body-less marker type (`@type Empty` with no fields). */
    if (!at(p, TOK_INDENT)) {
        n->as.typedef_.fields   = NULL;
        n->as.typedef_.n_fields = 0;
        return n;
    }
    advance(p); /* INDENT */

    const char *fields[32];
    uint32_t nf = 0;
    while (!at(p, TOK_DEDENT) && !at(p, TOK_EOF)) {
        if (nf >= 32) { set_err(p, DECK_LOAD_PARSE, "too many fields (max 32)"); return NULL; }
        if (!at(p, TOK_IDENT)) { set_err(p, DECK_LOAD_PARSE, "expected field name"); return NULL; }
        fields[nf++] = p->cur.text;
        advance(p);
        if (!expect(p, TOK_COLON, "expected ':' after field name")) return NULL;
        /* Type annotation — spec §5 complex types plus §4.3 union types
         * (`T | U`). Parse-and-discard via the bracket-balanced helper.
         * Union `|` between balanced type expressions is eaten too. */
        if (!skip_type_annotation(p)) return NULL;
        while (at(p, TOK_OR_OR) || at(p, TOK_PIPE) || at(p, TOK_BAR)) {
            advance(p);
            if (!skip_type_annotation(p)) return NULL;
        }
        while (at(p, TOK_NEWLINE)) advance(p);
    }
    if (!expect(p, TOK_DEDENT, "expected dedent closing @type body")) return NULL;
    n->as.typedef_.fields   = nf > 0 ? deck_arena_memdup(p->arena, fields, nf * sizeof(char *)) : NULL;
    n->as.typedef_.n_fields = nf;
    return n;
}

/* Bare-keyword `type T = <type-expr>` top-level alias (LANG §5.4
 * spec form). DL2 doesn't enforce types at runtime, so we parse-and-
 * discard: consume the name, the '=', and the balanced type
 * expression up to the next top-level boundary, returning a
 * placeholder AST node (AST_TYPE_DEF with zero fields). */
static ast_node_t *parse_type_alias_decl(deck_parser_t *p)
{
    advance(p); /* 'type' */
    if (!at(p, TOK_IDENT)) {
        set_err(p, DECK_LOAD_PARSE, "expected type name after 'type'");
        return NULL;
    }
    ast_node_t *n = mknode(p, AST_TYPE_DEF); if (!n) return NULL;
    n->as.typedef_.name     = p->cur.text;
    n->as.typedef_.fields   = NULL;
    n->as.typedef_.n_fields = 0;
    advance(p); /* name */
    if (!expect(p, TOK_ASSIGN, "expected '=' after type name")) return NULL;
    /* Skip the entire type expression. skip_type_annotation already
     * stops at top-level NEWLINE / DEDENT / EOF and handles balanced
     * brackets, so it's a perfect fit for the body. */
    if (!skip_type_annotation(p)) return NULL;
    while (at(p, TOK_NEWLINE)) advance(p);
    return n;
}

static ast_node_t *parse_top_item(deck_parser_t *p)
{
    /* Bare-keyword `type T = ...` lexes as IDENT IDENT '=' ... */
    if (at(p, TOK_IDENT) && p->cur.text &&
        strcmp(p->cur.text, "type") == 0 &&
        peek_next_tok(p) == TOK_IDENT) {
        return parse_type_alias_decl(p);
    }
    if (at(p, TOK_KW_FN)) {
        ast_node_t *fnn = parse_fn_decl(p);
        if (!fnn) return NULL;
        if (!fnn->as.fndef.name) {
            set_err(p, DECK_LOAD_PARSE,
                    "anonymous fn (lambda) is not allowed at top level");
            return NULL;
        }
        return fnn;
    }
    if (at(p, TOK_DECORATOR)) {
        if      (dec_is(&p->cur, "app"))            return parse_app_block(p);
        else if (dec_is(&p->cur, "needs"))          return parse_needs_decl(p); /* LANG §8 */
        else if (dec_is(&p->cur, "use"))            return parse_use_decl(p);
        else if (dec_is(&p->cur, "use.optional"))   return parse_use_decl(p);   /* DL2 F23.4 */
        else if (dec_is(&p->cur, "on"))             return parse_on_decl(p);
        else if (dec_is(&p->cur, "machine"))        return parse_machine_decl(p);
        else if (dec_is(&p->cur, "machine.before")) return parse_machine_hook_decl(p, "__machine_before");  /* F28.1 */
        else if (dec_is(&p->cur, "machine.after"))  return parse_machine_hook_decl(p, "__machine_after");   /* F28.1 */
        else if (dec_is(&p->cur, "flow"))           return parse_flow_decl(p);     /* F28.2 */
        else if (dec_is(&p->cur, "flow.step"))      return parse_opaque_block(p);  /* F28.2 — unused outside @flow */
        else if (dec_is(&p->cur, "migrate"))        return parse_migrate_decl(p); /* LANG §17 */
        else if (dec_is(&p->cur, "assets"))         return parse_assets_decl(p);   /* F28.5 */
        else if (dec_is(&p->cur, "type"))           return parse_type_decl(p);
        else if (dec_is(&p->cur, "grants"))         return parse_grants_decl(p);   /* Stage 5b */
        else if (dec_is(&p->cur, "errors"))         return parse_errors_decl(p);   /* Stage 5b */
        else if (dec_is(&p->cur, "handles"))        return parse_handles_decl(p);  /* Stage 5b */
        else if (dec_is(&p->cur, "config"))         return parse_config_decl(p);   /* Stage 5b */
        else if (dec_is(&p->cur, "service"))        return parse_service_decl(p);  /* Stage 5b */
        /* Spec-declared top-level annotations the runtime parses-and-
         * discards until real semantics land:
         *   @stream  — reactive data sources (§10)
         *   @task    — background tasks (§14)
         *   @doc     — module / fn documentation (§17)
         *   @example — executable doctest assertion (§17)
         *   @test    — named test block (§17)
         */
        else if (dec_is(&p->cur, "stream"))         return parse_opaque_block(p);
        else if (dec_is(&p->cur, "task"))           return parse_opaque_block(p);
        else if (dec_is(&p->cur, "doc"))            return parse_opaque_block(p);
        else if (dec_is(&p->cur, "example"))        return parse_opaque_block(p);
        else if (dec_is(&p->cur, "test"))           return parse_opaque_block(p);
        else if (dec_is(&p->cur, "private")) {
            /* DL2 F22.9 — @private prefixes a fn declaration. */
            advance(p);
            while (at(p, TOK_NEWLINE)) advance(p);
            if (!at(p, TOK_KW_FN)) {
                set_err(p, DECK_LOAD_PARSE, "@private must precede a fn declaration");
                return NULL;
            }
            ast_node_t *fnn = parse_fn_decl(p);
            if (!fnn) return NULL;
            if (!fnn->as.fndef.name) {
                set_err(p, DECK_LOAD_PARSE, "@private cannot precede an anonymous fn");
                return NULL;
            }
            fnn->as.fndef.is_private = true;
            return fnn;
        }
        set_err(p, DECK_LOAD_PARSE, "unknown top-level decorator");
        return NULL;
    }
    set_err(p, DECK_LOAD_PARSE, "expected @app, @use, @on, @machine, or fn at top level");
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
