#pragma once

/* deck_parser — LL(1) parser producing deck_ast trees in an arena.
 *
 * Grammar (DL1 subset):
 *   module   := (top_item (NEWLINE+ top_item)*)?
 *   top_item := app_decl | use_decl | on_decl | machine_decl | let_decl
 *   app_decl := "@app" NEWLINE INDENT app_field+ DEDENT
 *   app_field:= IDENT ":" expr NEWLINE
 *            |  IDENT ":" NEWLINE INDENT app_field+ DEDENT       # requires: ...
 *   use_decl := "@use" IDENT ("." IDENT)* NEWLINE
 *   on_decl  := "@on" IDENT ":" suite
 *   machine_decl := "@machine" IDENT NEWLINE INDENT state+ DEDENT
 *   state    := "state" IDENT ":" NEWLINE INDENT state_hook+ DEDENT
 *   state_hook := "on" "enter" ":" suite
 *              |  "on" "leave" ":" suite
 *              |  "transition" ATOM NEWLINE
 *   suite    := simple_stmt NEWLINE
 *            |  NEWLINE INDENT stmt+ DEDENT
 *   stmt     := let_stmt | expr_stmt | send_stmt | transition_stmt
 *   let_stmt := "let" IDENT "=" expr NEWLINE
 *   send_stmt := "send" ATOM NEWLINE
 *   transition_stmt := "transition" ATOM NEWLINE
 *   expr     := pratt-parsed expression
 *            |  "match" expr NEWLINE INDENT match_arm+ DEDENT
 *            |  "if" expr "then" expr "else" expr
 *   match_arm:= pattern ("when" expr)? "=>" expr NEWLINE
 *   pattern  := literal | ATOM | IDENT | "_"
 */

#include "deck_ast.h"
#include "deck_lexer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    deck_arena_t *arena;
    deck_lexer_t  lx;
    deck_token_t  cur;
    deck_token_t  peek;
    bool          have_peek;

    deck_err_t    err;
    const char   *err_msg;
    uint32_t      err_line;
    uint32_t      err_col;
} deck_parser_t;

/* Initialize parser over source. arena must outlive the parser. */
void deck_parser_init(deck_parser_t *p, deck_arena_t *arena,
                      const char *src, uint32_t len);

/* Parse a whole module. Returns NULL on error (see deck_parser_err*). */
ast_node_t *deck_parser_parse_module(deck_parser_t *p);

/* Parse a single expression (used by the harness for granular tests). */
ast_node_t *deck_parser_parse_expr(deck_parser_t *p);

deck_err_t  deck_parser_err_code(const deck_parser_t *p);
const char *deck_parser_err_msg(const deck_parser_t *p);
uint32_t    deck_parser_err_line(const deck_parser_t *p);
uint32_t    deck_parser_err_col(const deck_parser_t *p);

#ifdef __cplusplus
}
#endif
