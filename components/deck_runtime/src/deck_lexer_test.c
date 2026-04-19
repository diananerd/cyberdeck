/* Lexer self-tests. Each case runs a source string through the lexer
 * and asserts the resulting token sequence matches the expected one.
 * On mismatch the failing case + actual/expected are logged.
 */

#include "deck_lexer.h"
#include "deck_runtime.h"

#include "esp_log.h"
#include <string.h>

static const char *TAG = "deck_lexer";

typedef struct {
    const char *name;
    const char *src;
    /* Expected token types, terminated by TOK_EOF. Include the final EOF. */
    deck_tok_t  expect[32];
} lex_case_t;

static const lex_case_t CASES[] = {
    /* 1. Empty input */
    { "empty",         "",
      { TOK_EOF } },

    /* 2. Single int */
    { "int_dec",       "42",
      { TOK_INT, TOK_EOF } },

    /* 3. Hex */
    { "int_hex",       "0xDEADBEEF",
      { TOK_INT, TOK_EOF } },

    /* 4. Binary */
    { "int_bin",       "0b1010",
      { TOK_INT, TOK_EOF } },

    /* 5. Octal */
    { "int_oct",       "0o77",
      { TOK_INT, TOK_EOF } },

    /* 6. Underscore separator */
    { "int_underscore","1_000_000",
      { TOK_INT, TOK_EOF } },

    /* 7. Float */
    { "float_basic",   "3.14",
      { TOK_FLOAT, TOK_EOF } },

    /* 8. Float exp */
    { "float_exp",     "1.5e-3",
      { TOK_FLOAT, TOK_EOF } },

    /* 9. Keywords true/false */
    { "kw_bools",      "true false",
      { TOK_KW_TRUE, TOK_KW_FALSE, TOK_EOF } },

    /* 10. Keywords let / match / when / then */
    { "kw_control",    "let match when then",
      { TOK_KW_LET, TOK_KW_MATCH, TOK_KW_WHEN, TOK_KW_THEN, TOK_EOF } },

    /* 11. Identifiers */
    { "ident",         "foo_bar42",
      { TOK_IDENT, TOK_EOF } },

    /* 12. Atom */
    { "atom",          ":ok",
      { TOK_ATOM, TOK_EOF } },

    /* 13. Decorator */
    { "decorator",     "@app",
      { TOK_DECORATOR, TOK_EOF } },

    /* 14. Decorator dotted */
    { "decorator_dot", "@machine.state",
      { TOK_DECORATOR, TOK_EOF } },

    /* 15. String simple */
    { "string_basic",  "\"hello\"",
      { TOK_STRING, TOK_EOF } },

    /* 16. String with escapes */
    { "string_esc",    "\"a\\nb\\tc\"",
      { TOK_STRING, TOK_EOF } },

    /* 17. Arithmetic */
    { "arith",         "1 + 2 * 3",
      { TOK_INT, TOK_PLUS, TOK_INT, TOK_STAR, TOK_INT, TOK_EOF } },

    /* 18. Comparison */
    { "cmp",           "a <= b",
      { TOK_IDENT, TOK_LE, TOK_IDENT, TOK_EOF } },

    /* 19. Equality */
    { "eq_ne",         "x == y  z != w",
      { TOK_IDENT, TOK_EQ, TOK_IDENT, TOK_IDENT, TOK_NE, TOK_IDENT, TOK_EOF } },

    /* 20. Logical */
    { "logic",         "!flag && other || third",
      { TOK_BANG, TOK_IDENT, TOK_AND_AND, TOK_IDENT, TOK_OR_OR, TOK_IDENT, TOK_EOF } },

    /* 21. Arrow + fat arrow */
    { "arrows",        "-> =>",
      { TOK_ARROW, TOK_FAT_ARROW, TOK_EOF } },

    /* 22. Pipes */
    { "pipes",         "|> |>?",
      { TOK_PIPE, TOK_PIPE_OPT, TOK_EOF } },

    /* 22b. Standalone `|` (match-arm bar) vs `||` vs `|>` */
    { "bar_vs_or",     "| || |>",
      { TOK_BAR, TOK_OR_OR, TOK_PIPE, TOK_EOF } },

    /* 23. Concat */
    { "concat",        "a ++ b",
      { TOK_IDENT, TOK_CONCAT, TOK_IDENT, TOK_EOF } },

    /* 24. Parens and brackets */
    { "brackets",      "( [ { } ] )",
      { TOK_LPAREN, TOK_LBRACKET, TOK_LBRACE, TOK_RBRACE, TOK_RBRACKET, TOK_RPAREN, TOK_EOF } },

    /* 25. Comment */
    { "comment",       "x # the answer\ny",
      { TOK_IDENT, TOK_NEWLINE, TOK_IDENT, TOK_EOF } },

    /* 26. Blank line between statements */
    { "blank_line",    "a\n\nb",
      { TOK_IDENT, TOK_NEWLINE, TOK_NEWLINE, TOK_IDENT, TOK_EOF } },

    /* 27. Single indent + dedent */
    { "indent_basic",  "a\n  b\nc",
      { TOK_IDENT, TOK_NEWLINE, TOK_INDENT, TOK_IDENT, TOK_NEWLINE, TOK_DEDENT, TOK_IDENT, TOK_EOF } },

    /* 28. Two-level indent then full dedent */
    { "indent_two",    "a\n  b\n    c\nd",
      { TOK_IDENT, TOK_NEWLINE,
        TOK_INDENT, TOK_IDENT, TOK_NEWLINE,
        TOK_INDENT, TOK_IDENT, TOK_NEWLINE,
        TOK_DEDENT, TOK_DEDENT,
        TOK_IDENT, TOK_EOF } },

    /* 29. Power op */
    { "power",         "2 ** 10",
      { TOK_INT, TOK_POW, TOK_INT, TOK_EOF } },

    /* 30. Minus vs arrow */
    { "minus",         "x - y -> z",
      { TOK_IDENT, TOK_MINUS, TOK_IDENT, TOK_ARROW, TOK_IDENT, TOK_EOF } },

    /* 31. App header line fragment — EOF implies one DEDENT to close the indent level */
    { "app_header",    "@app\n  name: \"Hello\"",
      { TOK_DECORATOR, TOK_NEWLINE,
        TOK_INDENT, TOK_IDENT, TOK_COLON, TOK_STRING,
        TOK_DEDENT, TOK_EOF } },

    /* 32. Atom list */
    { "atoms_list",    "[:ok, :err]",
      { TOK_LBRACKET, TOK_ATOM, TOK_COMMA, TOK_ATOM, TOK_RBRACKET, TOK_EOF } },

    /* 33. Keyword none / some / unit */
    { "nullables",     "none some unit",
      { TOK_KW_NONE, TOK_KW_SOME, TOK_KW_UNIT, TOK_EOF } },

    /* 34. Error: unterminated string */
    { "err_untermstr", "\"abc",
      { TOK_ERROR } },

    /* 35. Question mark */
    { "question",      "x? y",
      { TOK_IDENT, TOK_QUESTION, TOK_IDENT, TOK_EOF } },

    /* 36. fn keyword (DL2 F21.1) */
    { "kw_fn",         "fn add (a, b) = a + b",
      { TOK_KW_FN, TOK_IDENT, TOK_LPAREN, TOK_IDENT, TOK_COMMA, TOK_IDENT,
        TOK_RPAREN, TOK_ASSIGN, TOK_IDENT, TOK_PLUS, TOK_IDENT, TOK_EOF } },
};

#define N_CASES (sizeof(CASES) / sizeof(CASES[0]))

static uint32_t expected_len(const deck_tok_t *expect)
{
    uint32_t n = 0;
    /* Stop at either the EOF sentinel or a TOK_ERROR terminator. */
    while (n < 32 && expect[n] != TOK_EOF && expect[n] != TOK_ERROR) n++;
    /* Include the terminator (EOF or ERROR) in the count. */
    return (n < 32) ? n + 1 : n;
}

static bool run_case(const lex_case_t *c)
{
    deck_lexer_t lx;
    deck_lexer_init(&lx, c->src, (uint32_t)strlen(c->src));

    deck_token_t tok;
    uint32_t i = 0;
    uint32_t n_expect = expected_len(c->expect);

    while (deck_lexer_next(&lx, &tok)) {
        if (i >= n_expect) {
            ESP_LOGE(TAG, "  [%s] extra token: %s", c->name, deck_tok_name(tok.type));
            return false;
        }
        deck_tok_t want = c->expect[i];
        if (tok.type != want) {
            ESP_LOGE(TAG, "  [%s] pos %u: got %s want %s",
                     c->name, (unsigned)i,
                     deck_tok_name(tok.type), deck_tok_name(want));
            return false;
        }
        i++;
        /* Stop after we see the expected TOK_ERROR or TOK_EOF. */
        if (want == TOK_EOF || want == TOK_ERROR) break;
    }

    if (i < n_expect) {
        ESP_LOGE(TAG, "  [%s] lexer stopped at %u/%u tokens",
                 c->name, (unsigned)i, (unsigned)n_expect);
        return false;
    }
    return true;
}

deck_err_t deck_lexer_run_selftest(void)
{
    uint32_t pass = 0, fail = 0;
    for (size_t i = 0; i < N_CASES; i++) {
        if (run_case(&CASES[i])) pass++;
        else                     fail++;
    }
    if (fail) {
        ESP_LOGE(TAG, "lexer selftest: %u/%u pass, %u fail",
                 (unsigned)pass, (unsigned)N_CASES, (unsigned)fail);
        return DECK_LOAD_LEX_ERROR;
    }
    ESP_LOGI(TAG, "lexer selftest: PASS (%u cases)", (unsigned)N_CASES);
    return DECK_RT_OK;
}
