#pragma once

/* deck_lexer — tokenizer for Deck source.
 *
 * DL1 surface (see deck-lang/01-deck-lang.md §2):
 *   - UTF-8 source; 8-bit clean for ASCII identifiers and literals
 *   - # line comments
 *   - indent-sensitive (2-space convention) → TOK_INDENT / TOK_DEDENT
 *   - literals: int (dec/hex/bin/oct), float, bool, unit, string, atom
 *   - identifiers + keywords + @decorators
 *   - operators: + - * / % ** < <= > >= == != && || ! = -> => |> |>? <>
 *   - punctuation: ( ) [ ] { } , ; . : ?
 *
 * String interpolation (`${expr}` inside "...") lands in a later commit;
 * F2.4 emits TOK_STRING with the literal body as-is. No escape sequences
 * beyond \n, \t, \\, \", \0 at F2.4.
 */

#include "deck_types.h"
#include "deck_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TOK_EOF = 0,
    TOK_NEWLINE,
    TOK_INDENT,
    TOK_DEDENT,

    /* Literals */
    TOK_INT,
    TOK_FLOAT,
    TOK_STRING,
    TOK_STRING_INTERP, /* DL2 F21.7 — string with ${...} placeholders, raw source */
    TOK_ATOM,
    TOK_IDENT,
    TOK_DECORATOR,    /* @ident — e.g. @app, @machine */

    /* Keywords */
    TOK_KW_LET, TOK_KW_MATCH, TOK_KW_WHEN, TOK_KW_THEN, TOK_KW_IF,
    TOK_KW_ELSE, TOK_KW_AND, TOK_KW_OR, TOK_KW_NOT, TOK_KW_TRUE,
    TOK_KW_FALSE, TOK_KW_UNIT, TOK_KW_NONE, TOK_KW_SOME, TOK_KW_DO,
    TOK_KW_WHERE, TOK_KW_IS, TOK_KW_STATE, TOK_KW_ON, TOK_KW_ENTER,
    TOK_KW_LEAVE, TOK_KW_SEND, TOK_KW_USE, TOK_KW_TRANSITION,
    TOK_KW_FN,

    /* Operators */
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT, TOK_POW,
    TOK_LT, TOK_LE, TOK_GT, TOK_GE, TOK_EQ, TOK_NE,
    TOK_AND_AND, TOK_OR_OR, TOK_BANG,
    TOK_ASSIGN, TOK_ARROW, TOK_FAT_ARROW,
    TOK_PIPE, TOK_PIPE_OPT, TOK_CONCAT,

    /* Punctuation */
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACKET, TOK_RBRACKET,
    TOK_LBRACE, TOK_RBRACE, TOK_COMMA, TOK_SEMI, TOK_DOT,
    TOK_COLON, TOK_QUESTION,

    TOK_ERROR,
    TOK_COUNT,
} deck_tok_t;

const char *deck_tok_name(deck_tok_t t);

typedef struct {
    deck_tok_t  type;
    uint32_t    line;    /* 1-based */
    uint32_t    col;     /* 1-based */
    const char *text;    /* interned — identifier/atom/string content; NULL otherwise */
    uint32_t    text_len;
    union {
        int64_t i;
        double  f;
    } as;
} deck_token_t;

#define DECK_LEXER_MAX_INDENT  32

typedef struct {
    const char *src;
    uint32_t    len;
    uint32_t    pos;
    uint32_t    line;
    uint32_t    col;

    uint32_t    indent_stack[DECK_LEXER_MAX_INDENT];
    uint8_t     indent_top;
    int16_t     pending_dedents;

    bool        at_line_start;
    bool        emitted_eof;

    const char *err_msg;
    uint32_t    err_line;
    uint32_t    err_col;
} deck_lexer_t;

/* Initialize. src must outlive the lexer (not copied). */
void deck_lexer_init(deck_lexer_t *lx, const char *src, uint32_t len);

/* Pull the next token. Returns true with out populated; false on EOF
 * or error. On error, out->type == TOK_ERROR and deck_lexer_err()
 * returns a human-readable message.
 */
bool deck_lexer_next(deck_lexer_t *lx, deck_token_t *out);

/* Last error message, or NULL if none. */
const char *deck_lexer_err(const deck_lexer_t *lx);

#ifdef __cplusplus
}
#endif
