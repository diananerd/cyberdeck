/* Interpreter self-tests. */

#include "deck_interp.h"
#include "deck_parser.h"
#include "deck_runtime.h"

#include "esp_log.h"
#include <string.h>

static const char *TAG = "deck_interp";

static deck_value_t *run_expr(const char *src, deck_err_t *err_out)
{
    deck_arena_t  arena;
    deck_parser_t p;
    deck_arena_init(&arena, 0);
    deck_parser_init(&p, &arena, src, (uint32_t)strlen(src));
    ast_node_t *root = deck_parser_parse_expr(&p);
    if (!root) { *err_out = DECK_LOAD_PARSE_ERROR; deck_arena_reset(&arena); return NULL; }
    deck_interp_ctx_t c;
    deck_interp_init(&c, &arena);
    deck_value_t *r = deck_interp_run(&c, c.global, root);
    *err_out = c.err;
    deck_arena_reset(&arena);
    return r;
}

#define CHECK(cond, msg) do { if (!(cond)) { \
    ESP_LOGE(TAG, "  [%s] FAIL: %s", name, msg); return false; } } while (0)

static bool t_int_literal(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("42", &e); CHECK(e == DECK_RT_OK, "err"); CHECK(v && v->type == DECK_T_INT, "type"); CHECK(v->as.i == 42, "value"); deck_release(v); return true; }
static bool t_add(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("2 + 3", &e); CHECK(v && v->as.i == 5, "5"); deck_release(v); (void)e; return true; }
static bool t_prec(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("2 + 3 * 4", &e); CHECK(v && v->as.i == 14, "14"); deck_release(v); (void)e; return true; }
static bool t_paren(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("(2 + 3) * 4", &e); CHECK(v && v->as.i == 20, "20"); deck_release(v); (void)e; return true; }
static bool t_float(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("3.0 + 0.14", &e); CHECK(v && v->type == DECK_T_FLOAT, "float"); CHECK(v->as.f > 3.139 && v->as.f < 3.141, "3.14"); deck_release(v); (void)e; return true; }
static bool t_div_zero(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("1 / 0", &e); CHECK(e == DECK_RT_DIVIDE_BY_ZERO, "div0"); (void)v; return true; }
static bool t_cmp(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("5 < 10", &e); CHECK(v && v->type == DECK_T_BOOL && v->as.b, "true"); deck_release(v); (void)e; return true; }
static bool t_logic(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("true && false", &e); CHECK(v && !v->as.b, "false"); deck_release(v); (void)e; return true; }
static bool t_short_circuit(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("false && (1 / 0 == 0)", &e); CHECK(e == DECK_RT_OK, "no div0"); CHECK(v && !v->as.b, "false"); deck_release(v); return true; }
static bool t_concat(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("\"foo\" <> \"bar\"", &e); CHECK(v && v->type == DECK_T_STR, "str"); CHECK(v->as.s.len == 6 && memcmp(v->as.s.ptr, "foobar", 6) == 0, "foobar"); deck_release(v); (void)e; return true; }
static bool t_neg(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("-(5)", &e); CHECK(v && v->as.i == -5, "-5"); deck_release(v); (void)e; return true; }
static bool t_if(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("if 1 < 2 then 10 else 20", &e); CHECK(v && v->as.i == 10, "10"); deck_release(v); (void)e; return true; }
static bool t_pow(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("2 ** 10", &e); CHECK(v && v->as.i == 1024, "1024"); deck_release(v); (void)e; return true; }

#define APP_HDR_DL1 \
    "@app\n" \
    "  name: \"X\"\n" \
    "  id: \"y\"\n" \
    "  version: \"1.0.0\"\n" \
    "  edition: 2026\n" \
    "  requires:\n" \
    "    deck_level: 1\n"

static bool t_hello(const char *name)
{
    const char *src = APP_HDR_DL1 "\n@on launch:\n  log.info(\"Hello from Deck DL1\")\n";
    deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
    CHECK(rc == DECK_RT_OK, "run");
    return true;
}
static bool t_match_wild(const char *name)
{
    const char *src = APP_HDR_DL1 "\n@on launch:\n  match 1\n    0 => log.info(\"z\")\n    _ => log.info(\"match wild branch taken\")\n";
    deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
    CHECK(rc == DECK_RT_OK, "run");
    return true;
}

typedef bool (*tfn_t)(const char *);
typedef struct { const char *name; tfn_t fn; } case_t;

static const case_t CASES[] = {
    { "int_literal",   t_int_literal },
    { "add",           t_add },
    { "prec",          t_prec },
    { "paren",         t_paren },
    { "float",         t_float },
    { "div_zero",      t_div_zero },
    { "cmp",           t_cmp },
    { "logic",         t_logic },
    { "short_circuit", t_short_circuit },
    { "concat",        t_concat },
    { "neg",           t_neg },
    { "if",            t_if },
    { "pow",           t_pow },
    { "hello",         t_hello },
    { "match_wild",    t_match_wild },
};
#define N_CASES (sizeof(CASES) / sizeof(CASES[0]))

deck_err_t deck_interp_run_selftest(void)
{
    uint32_t pass = 0, fail = 0;
    for (size_t i = 0; i < N_CASES; i++) {
        if (CASES[i].fn(CASES[i].name)) pass++; else fail++;
    }
    if (fail) {
        ESP_LOGE(TAG, "interp selftest: %u/%u pass, %u fail",
                 (unsigned)pass, (unsigned)N_CASES, (unsigned)fail);
        return DECK_RT_INTERNAL;
    }
    ESP_LOGI(TAG, "interp selftest: PASS (%u cases)", (unsigned)N_CASES);
    return DECK_RT_OK;
}
