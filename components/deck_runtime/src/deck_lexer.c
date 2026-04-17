#include "deck_lexer.h"
#include "deck_intern.h"

#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* ================================================================
 * Token name table
 * ================================================================ */

static const char *const s_tok_names[TOK_COUNT] = {
    [TOK_EOF]          = "EOF",
    [TOK_NEWLINE]      = "NEWLINE",
    [TOK_INDENT]       = "INDENT",
    [TOK_DEDENT]       = "DEDENT",
    [TOK_INT]          = "INT",
    [TOK_FLOAT]        = "FLOAT",
    [TOK_STRING]       = "STRING",
    [TOK_ATOM]         = "ATOM",
    [TOK_IDENT]        = "IDENT",
    [TOK_DECORATOR]    = "DECORATOR",
    [TOK_KW_LET]       = "let",
    [TOK_KW_MATCH]     = "match",
    [TOK_KW_WHEN]      = "when",
    [TOK_KW_THEN]      = "then",
    [TOK_KW_IF]        = "if",
    [TOK_KW_ELSE]      = "else",
    [TOK_KW_AND]       = "and",
    [TOK_KW_OR]        = "or",
    [TOK_KW_NOT]       = "not",
    [TOK_KW_TRUE]      = "true",
    [TOK_KW_FALSE]     = "false",
    [TOK_KW_UNIT]      = "unit",
    [TOK_KW_NONE]      = "none",
    [TOK_KW_SOME]      = "some",
    [TOK_KW_DO]        = "do",
    [TOK_KW_WHERE]     = "where",
    [TOK_KW_IS]        = "is",
    [TOK_KW_STATE]     = "state",
    [TOK_KW_ON]        = "on",
    [TOK_KW_ENTER]     = "enter",
    [TOK_KW_LEAVE]     = "leave",
    [TOK_KW_SEND]      = "send",
    [TOK_KW_USE]       = "use",
    [TOK_KW_TRANSITION]= "transition",
    [TOK_PLUS]         = "+",
    [TOK_MINUS]        = "-",
    [TOK_STAR]         = "*",
    [TOK_SLASH]        = "/",
    [TOK_PERCENT]      = "%",
    [TOK_POW]          = "**",
    [TOK_LT]           = "<",
    [TOK_LE]           = "<=",
    [TOK_GT]           = ">",
    [TOK_GE]           = ">=",
    [TOK_EQ]           = "==",
    [TOK_NE]           = "!=",
    [TOK_AND_AND]      = "&&",
    [TOK_OR_OR]        = "||",
    [TOK_BANG]         = "!",
    [TOK_ASSIGN]       = "=",
    [TOK_ARROW]        = "->",
    [TOK_FAT_ARROW]    = "=>",
    [TOK_PIPE]         = "|>",
    [TOK_PIPE_OPT]     = "|>?",
    [TOK_CONCAT]       = "<>",
    [TOK_LPAREN]       = "(",
    [TOK_RPAREN]       = ")",
    [TOK_LBRACKET]     = "[",
    [TOK_RBRACKET]     = "]",
    [TOK_LBRACE]       = "{",
    [TOK_RBRACE]       = "}",
    [TOK_COMMA]        = ",",
    [TOK_SEMI]         = ";",
    [TOK_DOT]          = ".",
    [TOK_COLON]        = ":",
    [TOK_QUESTION]     = "?",
    [TOK_ERROR]        = "ERROR",
};

const char *deck_tok_name(deck_tok_t t)
{
    if (t < 0 || t >= TOK_COUNT) return "?";
    return s_tok_names[t] ? s_tok_names[t] : "?";
}

/* ================================================================
 * Lexer core
 * ================================================================ */

void deck_lexer_init(deck_lexer_t *lx, const char *src, uint32_t len)
{
    if (!lx) return;
    memset(lx, 0, sizeof(*lx));
    lx->src = src;
    lx->len = len;
    lx->line = 1;
    lx->col  = 1;
    lx->indent_stack[0] = 0;
    lx->indent_top = 0;
    lx->at_line_start = true;
}

const char *deck_lexer_err(const deck_lexer_t *lx)
{
    return lx ? lx->err_msg : NULL;
}

static int peek(deck_lexer_t *lx)
{
    return lx->pos < lx->len ? (unsigned char)lx->src[lx->pos] : -1;
}

static int peek_at(deck_lexer_t *lx, uint32_t off)
{
    uint32_t p = lx->pos + off;
    return p < lx->len ? (unsigned char)lx->src[p] : -1;
}

static int advance(deck_lexer_t *lx)
{
    if (lx->pos >= lx->len) return -1;
    int c = (unsigned char)lx->src[lx->pos++];
    if (c == '\n') { lx->line++; lx->col = 1; }
    else           { lx->col++; }
    return c;
}

static void set_error(deck_lexer_t *lx, const char *msg)
{
    if (!lx->err_msg) {
        lx->err_msg  = msg;
        lx->err_line = lx->line;
        lx->err_col  = lx->col;
    }
}

static void emit(deck_token_t *out, deck_tok_t t, uint32_t line, uint32_t col)
{
    memset(out, 0, sizeof(*out));
    out->type = t;
    out->line = line;
    out->col  = col;
}

/* ================================================================
 * Helpers
 * ================================================================ */

static bool is_ident_start(int c) { return c == '_' || isalpha(c); }
static bool is_ident_cont(int c)  { return c == '_' || isalnum(c); }

typedef struct { const char *name; deck_tok_t tok; } kw_t;
static const kw_t s_keywords[] = {
    {"let",        TOK_KW_LET},
    {"match",      TOK_KW_MATCH},
    {"when",       TOK_KW_WHEN},
    {"then",       TOK_KW_THEN},
    {"if",         TOK_KW_IF},
    {"else",       TOK_KW_ELSE},
    {"and",        TOK_KW_AND},
    {"or",         TOK_KW_OR},
    {"not",        TOK_KW_NOT},
    {"true",       TOK_KW_TRUE},
    {"false",      TOK_KW_FALSE},
    {"unit",       TOK_KW_UNIT},
    {"none",       TOK_KW_NONE},
    {"some",       TOK_KW_SOME},
    {"do",         TOK_KW_DO},
    {"where",      TOK_KW_WHERE},
    {"is",         TOK_KW_IS},
    {"state",      TOK_KW_STATE},
    {"on",         TOK_KW_ON},
    {"enter",      TOK_KW_ENTER},
    {"leave",      TOK_KW_LEAVE},
    {"send",       TOK_KW_SEND},
    {"use",        TOK_KW_USE},
    {"transition", TOK_KW_TRANSITION},
    {NULL, TOK_EOF},
};

static deck_tok_t keyword_lookup(const char *s, uint32_t len)
{
    for (const kw_t *k = s_keywords; k->name; k++) {
        if (strlen(k->name) == len && memcmp(k->name, s, len) == 0)
            return k->tok;
    }
    return TOK_IDENT;
}

/* ================================================================
 * Literal scanners
 * ================================================================ */

/* Returns true on success. Fills out->as.i (int) or out->as.f (float),
 * out->type accordingly. Consumes the number from the input. */
static bool scan_number(deck_lexer_t *lx, deck_token_t *out)
{
    uint32_t start = lx->pos;
    uint32_t line = lx->line, col = lx->col;

    /* Hex/bin/oct prefixes. */
    if (peek(lx) == '0' && (peek_at(lx, 1) == 'x' || peek_at(lx, 1) == 'X' ||
                             peek_at(lx, 1) == 'b' || peek_at(lx, 1) == 'B' ||
                             peek_at(lx, 1) == 'o' || peek_at(lx, 1) == 'O')) {
        int prefix = peek_at(lx, 1);
        advance(lx); advance(lx);  /* consume 0x / 0b / 0o */
        uint64_t acc = 0;
        uint32_t digit_count = 0;
        int base = (prefix == 'x' || prefix == 'X') ? 16
                 : (prefix == 'b' || prefix == 'B') ? 2 : 8;
        for (;;) {
            int c = peek(lx);
            int d = -1;
            if (c >= '0' && c <= '9')              d = c - '0';
            else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else if (c == '_')                      { advance(lx); continue; }
            if (d < 0 || d >= base) break;
            acc = acc * (uint64_t)base + (uint64_t)d;
            digit_count++;
            advance(lx);
        }
        if (digit_count == 0) { set_error(lx, "empty integer literal"); return false; }
        emit(out, TOK_INT, line, col);
        out->as.i = (int64_t)acc;
        return true;
    }

    bool seen_dot = false;
    bool seen_exp = false;
    while (lx->pos < lx->len) {
        int c = peek(lx);
        if (isdigit(c) || c == '_') { advance(lx); }
        else if (c == '.' && !seen_dot && !seen_exp && isdigit(peek_at(lx, 1))) {
            seen_dot = true; advance(lx);
        } else if ((c == 'e' || c == 'E') && !seen_exp) {
            seen_exp = true; seen_dot = true; /* becomes float */
            advance(lx);
            if (peek(lx) == '+' || peek(lx) == '-') advance(lx);
        } else break;
    }

    /* Copy into a scratch buffer without '_' separators for strtoll/strtod. */
    char scratch[64];
    uint32_t src_len = lx->pos - start;
    if (src_len >= sizeof(scratch)) {
        set_error(lx, "numeric literal too long");
        return false;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < src_len; i++) {
        char c = lx->src[start + i];
        if (c != '_') scratch[k++] = c;
    }
    scratch[k] = '\0';

    emit(out, seen_dot ? TOK_FLOAT : TOK_INT, line, col);
    if (seen_dot) {
        out->as.f = strtod(scratch, NULL);
    } else {
        out->as.i = (int64_t)strtoll(scratch, NULL, 10);
    }
    return true;
}

static bool scan_string(deck_lexer_t *lx, deck_token_t *out)
{
    uint32_t line = lx->line, col = lx->col;
    advance(lx); /* opening " */

    /* Scratch buffer — we process escapes into it. 1 KB is plenty for DL1. */
    char scratch[1024];
    uint32_t k = 0;

    for (;;) {
        int c = peek(lx);
        if (c < 0 || c == '\n') {
            set_error(lx, "unterminated string literal");
            emit(out, TOK_ERROR, line, col);
            return true;
        }
        if (c == '"') { advance(lx); break; }
        if (c == '\\') {
            advance(lx);
            int e = advance(lx);
            char out_c;
            switch (e) {
                case 'n':  out_c = '\n'; break;
                case 't':  out_c = '\t'; break;
                case 'r':  out_c = '\r'; break;
                case '\\': out_c = '\\'; break;
                case '"':  out_c = '"';  break;
                case '0':  out_c = '\0'; break;
                default:
                    set_error(lx, "unknown escape sequence");
                    emit(out, TOK_ERROR, line, col);
                    return false;
            }
            if (k + 1 >= sizeof(scratch)) { set_error(lx, "string too long"); return false; }
            scratch[k++] = out_c;
            continue;
        }
        if (k + 1 >= sizeof(scratch)) { set_error(lx, "string too long"); return false; }
        scratch[k++] = (char)advance(lx);
    }

    emit(out, TOK_STRING, line, col);
    out->text     = deck_intern(scratch, k);
    out->text_len = k;
    return true;
}

static bool scan_identifier(deck_lexer_t *lx, deck_token_t *out)
{
    uint32_t start = lx->pos;
    uint32_t line = lx->line, col = lx->col;
    advance(lx); /* first char already known valid */
    while (is_ident_cont(peek(lx))) advance(lx);
    uint32_t len = lx->pos - start;
    const char *src = lx->src + start;

    deck_tok_t kw = keyword_lookup(src, len);
    if (kw != TOK_IDENT) {
        emit(out, kw, line, col);
        return true;
    }
    emit(out, TOK_IDENT, line, col);
    out->text     = deck_intern(src, len);
    out->text_len = len;
    return true;
}

static bool scan_decorator(deck_lexer_t *lx, deck_token_t *out)
{
    uint32_t line = lx->line, col = lx->col;
    advance(lx); /* @ */
    if (!is_ident_start(peek(lx))) {
        set_error(lx, "@ must be followed by an identifier");
        return false;
    }
    uint32_t start = lx->pos;
    advance(lx);
    while (is_ident_cont(peek(lx)) || peek(lx) == '.') advance(lx);
    emit(out, TOK_DECORATOR, line, col);
    out->text     = deck_intern(lx->src + start, lx->pos - start);
    out->text_len = lx->pos - start;
    return true;
}

static bool scan_atom(deck_lexer_t *lx, deck_token_t *out)
{
    uint32_t line = lx->line, col = lx->col;
    advance(lx); /* : */
    if (!is_ident_start(peek(lx))) {
        /* Not an atom — standalone colon. Back up and emit COLON. */
        /* But we already consumed ':'; roll back is expensive. Instead,
         * the caller guards: only call scan_atom when : is followed by
         * ident start. If we get here, treat as error (shouldn't happen). */
        set_error(lx, "atom must start with identifier character");
        return false;
    }
    uint32_t start = lx->pos;
    advance(lx);
    while (is_ident_cont(peek(lx))) advance(lx);
    emit(out, TOK_ATOM, line, col);
    out->text     = deck_intern(lx->src + start, lx->pos - start);
    out->text_len = lx->pos - start;
    return true;
}

/* ================================================================
 * Indent handling
 * ================================================================ */

static bool handle_line_start(deck_lexer_t *lx, deck_token_t *out)
{
    /* Count leading spaces (reject tabs). */
    uint32_t spaces = 0;
    while (peek(lx) == ' ') { advance(lx); spaces++; }
    if (peek(lx) == '\t') {
        set_error(lx, "tabs are not allowed for indentation; use spaces");
        emit(out, TOK_ERROR, lx->line, lx->col);
        return true;
    }

    /* Blank or comment-only line — don't emit indent changes. */
    if (peek(lx) == '\n' || peek(lx) == '#' || peek(lx) < 0) {
        return false;
    }

    uint32_t cur = lx->indent_stack[lx->indent_top];
    if (spaces > cur) {
        if (lx->indent_top + 1 >= DECK_LEXER_MAX_INDENT) {
            set_error(lx, "indentation too deep");
            emit(out, TOK_ERROR, lx->line, lx->col);
            return true;
        }
        lx->indent_stack[++lx->indent_top] = spaces;
        emit(out, TOK_INDENT, lx->line, 1);
        lx->at_line_start = false;
        return true;
    }
    if (spaces < cur) {
        /* Emit as many DEDENTs as needed, then one more pass will pick up content. */
        while (lx->indent_top > 0 &&
               lx->indent_stack[lx->indent_top] > spaces) {
            lx->indent_top--;
            lx->pending_dedents++;
        }
        if (lx->indent_stack[lx->indent_top] != spaces) {
            set_error(lx, "inconsistent dedent");
            emit(out, TOK_ERROR, lx->line, lx->col);
            return true;
        }
        /* Pull one of the pending dedents right now. */
        if (lx->pending_dedents > 0) {
            lx->pending_dedents--;
            emit(out, TOK_DEDENT, lx->line, 1);
            if (lx->pending_dedents == 0) lx->at_line_start = false;
            return true;
        }
    }
    lx->at_line_start = false;
    return false;
}

/* ================================================================
 * Main driver
 * ================================================================ */

bool deck_lexer_next(deck_lexer_t *lx, deck_token_t *out)
{
    if (!lx || !out) return false;

    /* Drain pending DEDENTs first. */
    if (lx->pending_dedents > 0) {
        lx->pending_dedents--;
        emit(out, TOK_DEDENT, lx->line, 1);
        return true;
    }

    if (lx->emitted_eof) return false;

    /* Handle line start: emit INDENT/DEDENT if needed. */
    if (lx->at_line_start) {
        if (handle_line_start(lx, out)) return true;
    }

    /* Skip intra-line spaces and comments. */
    while (lx->pos < lx->len) {
        int c = peek(lx);
        if (c == ' ' || c == '\r') { advance(lx); continue; }
        if (c == '#') {
            while (lx->pos < lx->len && peek(lx) != '\n') advance(lx);
            continue;
        }
        break;
    }

    /* End of input → drain remaining dedents, then EOF. */
    if (lx->pos >= lx->len) {
        if (lx->indent_top > 0) {
            lx->indent_top--;
            emit(out, TOK_DEDENT, lx->line, 1);
            return true;
        }
        lx->emitted_eof = true;
        emit(out, TOK_EOF, lx->line, lx->col);
        return true;
    }

    int c = peek(lx);
    if (c == '\n') {
        uint32_t line = lx->line, col = lx->col;
        advance(lx);
        lx->at_line_start = true;
        emit(out, TOK_NEWLINE, line, col);
        return true;
    }

    /* Single / double-char operators and punctuation. */
    switch (c) {
        case '+': advance(lx); emit(out, TOK_PLUS,  lx->line, lx->col); return true;
        case '*':
            advance(lx);
            if (peek(lx) == '*') { advance(lx); emit(out, TOK_POW, lx->line, lx->col); }
            else                 { emit(out, TOK_STAR, lx->line, lx->col); }
            return true;
        case '/': advance(lx); emit(out, TOK_SLASH,  lx->line, lx->col); return true;
        case '%': advance(lx); emit(out, TOK_PERCENT, lx->line, lx->col); return true;
        case '<':
            advance(lx);
            if (peek(lx) == '=') { advance(lx); emit(out, TOK_LE, lx->line, lx->col); }
            else if (peek(lx) == '>') { advance(lx); emit(out, TOK_CONCAT, lx->line, lx->col); }
            else                 { emit(out, TOK_LT, lx->line, lx->col); }
            return true;
        case '>':
            advance(lx);
            if (peek(lx) == '=') { advance(lx); emit(out, TOK_GE, lx->line, lx->col); }
            else                 { emit(out, TOK_GT, lx->line, lx->col); }
            return true;
        case '=':
            advance(lx);
            if (peek(lx) == '=') { advance(lx); emit(out, TOK_EQ, lx->line, lx->col); }
            else if (peek(lx) == '>') { advance(lx); emit(out, TOK_FAT_ARROW, lx->line, lx->col); }
            else                 { emit(out, TOK_ASSIGN, lx->line, lx->col); }
            return true;
        case '!':
            advance(lx);
            if (peek(lx) == '=') { advance(lx); emit(out, TOK_NE, lx->line, lx->col); }
            else                 { emit(out, TOK_BANG, lx->line, lx->col); }
            return true;
        case '&':
            advance(lx);
            if (peek(lx) == '&') { advance(lx); emit(out, TOK_AND_AND, lx->line, lx->col); return true; }
            set_error(lx, "unexpected '&' (did you mean '&&'?)");
            emit(out, TOK_ERROR, lx->line, lx->col);
            return true;
        case '|':
            advance(lx);
            if (peek(lx) == '|') { advance(lx); emit(out, TOK_OR_OR, lx->line, lx->col); return true; }
            if (peek(lx) == '>') {
                advance(lx);
                if (peek(lx) == '?') { advance(lx); emit(out, TOK_PIPE_OPT, lx->line, lx->col); }
                else                 { emit(out, TOK_PIPE, lx->line, lx->col); }
                return true;
            }
            set_error(lx, "unexpected '|'");
            emit(out, TOK_ERROR, lx->line, lx->col);
            return true;
        case '-':
            advance(lx);
            if (peek(lx) == '>') { advance(lx); emit(out, TOK_ARROW, lx->line, lx->col); }
            else                 { emit(out, TOK_MINUS, lx->line, lx->col); }
            return true;
        case '(': advance(lx); emit(out, TOK_LPAREN,   lx->line, lx->col); return true;
        case ')': advance(lx); emit(out, TOK_RPAREN,   lx->line, lx->col); return true;
        case '[': advance(lx); emit(out, TOK_LBRACKET, lx->line, lx->col); return true;
        case ']': advance(lx); emit(out, TOK_RBRACKET, lx->line, lx->col); return true;
        case '{': advance(lx); emit(out, TOK_LBRACE,   lx->line, lx->col); return true;
        case '}': advance(lx); emit(out, TOK_RBRACE,   lx->line, lx->col); return true;
        case ',': advance(lx); emit(out, TOK_COMMA,    lx->line, lx->col); return true;
        case ';': advance(lx); emit(out, TOK_SEMI,     lx->line, lx->col); return true;
        case '.': advance(lx); emit(out, TOK_DOT,      lx->line, lx->col); return true;
        case '?': advance(lx); emit(out, TOK_QUESTION, lx->line, lx->col); return true;
        case ':':
            /* atom vs standalone colon */
            if (is_ident_start(peek_at(lx, 1))) return scan_atom(lx, out);
            advance(lx); emit(out, TOK_COLON, lx->line, lx->col); return true;
        default: break;
    }

    if (c == '"') return scan_string(lx, out);
    if (isdigit(c)) return scan_number(lx, out);
    if (c == '@') return scan_decorator(lx, out);
    if (is_ident_start(c)) return scan_identifier(lx, out);

    set_error(lx, "unexpected character");
    emit(out, TOK_ERROR, lx->line, lx->col);
    advance(lx);
    return true;
}
