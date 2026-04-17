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

/* ---- builtin builtins: math + text + bytes + conversions ---- */
static bool t_math_abs(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("math.abs(-7)", &e); CHECK(v && v->as.i == 7, "7"); deck_release(v); (void)e; return true; }
static bool t_math_min(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("math.min(3, 5)", &e); CHECK(v && v->as.i == 3, "3"); deck_release(v); (void)e; return true; }
static bool t_math_max(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("math.max(3, 5)", &e); CHECK(v && v->as.i == 5, "5"); deck_release(v); (void)e; return true; }
static bool t_math_floor(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("math.floor(2.7)", &e); CHECK(v && v->as.i == 2, "2"); deck_release(v); (void)e; return true; }
static bool t_math_ceil(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("math.ceil(2.1)", &e); CHECK(v && v->as.i == 3, "3"); deck_release(v); (void)e; return true; }
static bool t_text_lower(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("text.lower(\"ABC\")", &e); CHECK(v && v->as.s.len == 3 && memcmp(v->as.s.ptr, "abc", 3) == 0, "abc"); deck_release(v); (void)e; return true; }
static bool t_text_len(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("text.len(\"hello\")", &e); CHECK(v && v->as.i == 5, "5"); deck_release(v); (void)e; return true; }
static bool t_text_starts_with(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("text.starts_with(\"hello\", \"he\")", &e); CHECK(v && v->as.b, "true"); deck_release(v); (void)e; return true; }
static bool t_text_ends_with(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("text.ends_with(\"hello\", \"lo\")", &e); CHECK(v && v->as.b, "true"); deck_release(v); (void)e; return true; }
static bool t_text_contains(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("text.contains(\"hello\", \"ell\")", &e); CHECK(v && v->as.b, "true"); deck_release(v); (void)e; return true; }
static bool t_conv_str_int(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("str(42)", &e); CHECK(v && v->as.s.len == 2 && memcmp(v->as.s.ptr, "42", 2) == 0, "42"); deck_release(v); (void)e; return true; }
static bool t_conv_int_str(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("int(\"99\")", &e); CHECK(v && v->as.i == 99, "99"); deck_release(v); (void)e; return true; }
static bool t_conv_float_int(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("float(5)", &e); CHECK(v && v->type == DECK_T_FLOAT && v->as.f == 5.0, "5.0"); deck_release(v); (void)e; return true; }
static bool t_conv_bool_str(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("bool(\"\")", &e); CHECK(v && !v->as.b, "false"); deck_release(v); (void)e; return true; }
static bool t_conv_roundtrip(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("int(str(123))", &e); CHECK(v && v->as.i == 123, "123"); deck_release(v); (void)e; return true; }
static bool t_time_duration(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("time.duration(100, 20)", &e); CHECK(v && v->as.i == 80, "80"); deck_release(v); (void)e; return true; }

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
static bool t_machine_two_states(const char *name)
{
    const char *src = APP_HDR_DL1
        "\n@machine m\n"
        "  state start:\n"
        "    on enter:\n"
        "      log.info(\"machine start\")\n"
        "    transition :finish\n"
        "  state finish:\n"
        "    on enter:\n"
        "      log.info(\"machine finish\")\n";
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
    { "pow",              t_pow },
    { "math_abs",         t_math_abs },
    { "math_min",         t_math_min },
    { "math_max",         t_math_max },
    { "math_floor",       t_math_floor },
    { "math_ceil",        t_math_ceil },
    { "text_lower",       t_text_lower },
    { "text_len",         t_text_len },
    { "text_starts_with", t_text_starts_with },
    { "text_ends_with",   t_text_ends_with },
    { "text_contains",    t_text_contains },
    { "conv_str_int",     t_conv_str_int },
    { "conv_int_str",     t_conv_int_str },
    { "conv_float_int",   t_conv_float_int },
    { "conv_bool_str",    t_conv_bool_str },
    { "conv_roundtrip",   t_conv_roundtrip },
    { "time_duration",    t_time_duration },
    { "hello",              t_hello },
    { "match_wild",         t_match_wild },
    { "machine_two_states", t_machine_two_states },
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
