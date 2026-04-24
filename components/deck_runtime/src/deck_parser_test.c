/* Parser self-tests. Each case parses a Deck source fragment and
 * compares the printed AST (s-expr form) against an expected string.
 */

#include "deck_parser.h"
#include "deck_runtime.h"

#include "esp_log.h"
#include <string.h>

static const char *TAG = "deck_parser";

typedef enum { TEST_EXPR, TEST_MODULE, TEST_ERROR } test_mode_t;

typedef struct {
    const char *name;
    test_mode_t mode;
    const char *src;
    /* For TEST_EXPR/TEST_MODULE: exact expected AST printer output.
     * For TEST_ERROR: substring that must appear in err_msg (NULL → any error). */
    const char *expect;
} parser_case_t;

static const parser_case_t CASES[] = {
    /* --- literals --- */
    { "int_dec",      TEST_EXPR, "42",             "(int 42)" },
    { "int_hex",      TEST_EXPR, "0xFF",           "(int 255)" },
    { "int_bin",      TEST_EXPR, "0b1010",         "(int 10)" },
    { "float",        TEST_EXPR, "3.14",           "(float 3.14)" },
    { "bool_true",    TEST_EXPR, "true",           "(bool true)" },
    { "bool_false",   TEST_EXPR, "false",          "(bool false)" },
    { "unit",         TEST_EXPR, "unit",           "(unit)" },
    { "none",         TEST_EXPR, "none",           "(none)" },
    { "string",       TEST_EXPR, "\"hello\"",      "(str \"hello\")" },
    { "atom",         TEST_EXPR, ":ok",            "(atom :ok)" },
    { "ident",        TEST_EXPR, "x",              "(ident x)" },

    /* --- binops + precedence --- */
    { "add",          TEST_EXPR, "1 + 2",          "(binop + (int 1) (int 2))" },
    { "prec_add_mul", TEST_EXPR, "1 + 2 * 3",      "(binop + (int 1) (binop * (int 2) (int 3)))" },
    { "left_assoc",   TEST_EXPR, "1 - 2 - 3",      "(binop - (binop - (int 1) (int 2)) (int 3))" },
    { "right_pow",    TEST_EXPR, "2 ** 3 ** 2",    "(binop ** (int 2) (binop ** (int 3) (int 2)))" },
    { "parens",       TEST_EXPR, "(1 + 2) * 3",    "(binop * (binop + (int 1) (int 2)) (int 3))" },
    { "cmp_logic",    TEST_EXPR, "x < y && y < z",
      "(binop && (binop < (ident x) (ident y)) (binop < (ident y) (ident z)))" },
    { "or_and",       TEST_EXPR, "a || b && c",
      "(binop || (ident a) (binop && (ident b) (ident c)))" },
    { "concat",       TEST_EXPR, "\"a\" ++ \"b\"",
      "(binop ++ (str \"a\") (str \"b\"))" },
    { "pipe",         TEST_EXPR, "x |> f",         "(binop |> (ident x) (ident f))" },
    { "eq",           TEST_EXPR, "x == 1",         "(binop == (ident x) (int 1))" },
    { "neq",          TEST_EXPR, "x != 1",         "(binop != (ident x) (int 1))" },

    /* --- unary --- */
    { "neg",          TEST_EXPR, "-5",             "(unary - (int 5))" },
    { "not",          TEST_EXPR, "!flag",          "(unary ! (ident flag))" },

    /* --- call + dot --- */
    { "call0",        TEST_EXPR, "f()",            "(call (ident f))" },
    { "call1",        TEST_EXPR, "f(1)",           "(call (ident f) (int 1))" },
    { "call2",        TEST_EXPR, "f(1, 2)",        "(call (ident f) (int 1) (int 2))" },
    { "dot",          TEST_EXPR, "log.info",       "(dot (ident log) info)" },
    { "dot_call",     TEST_EXPR, "log.info(\"hi\")",
      "(call (dot (ident log) info) (str \"hi\"))" },
    { "nested_call",  TEST_EXPR, "f(g(1), 2)",
      "(call (ident f) (call (ident g) (int 1)) (int 2))" },

    /* --- if --- */
    { "if_expr",      TEST_EXPR, "if x then 1 else 2",
      "(if (ident x) (int 1) (int 2))" },

    /* --- module-level --- */
    /* Spec 02-deck-app §4 — @use is a block annotation. A single
     * binding is expressed as a one-entry block. The alias defaults
     * to the last dotted segment when `as alias` is omitted. */
    { "mod_use",      TEST_MODULE, "@use\n  log as log\n",
      "(module (use (log as log)))" },
    { "mod_use_dot",  TEST_MODULE, "@use\n  system.time as systime\n",
      "(module (use (system.time as systime)))" },
    { "mod_use_block", TEST_MODULE,
      "@use\n"
      "  nvs as nvs\n"
      "  fs as fs\n",
      "(module (use (nvs as nvs) (fs as fs)))" },
    { "mod_use_optional", TEST_MODULE,
      "@use\n"
      "  crypto.aes as aes  optional\n",
      "(module (use (crypto.aes as aes optional)))" },
    { "mod_app_min",  TEST_MODULE,
      "@app\n"
      "  name: \"X\"\n"
      "  id: \"y\"\n",
      "(module (app (name (str \"X\")) (id (str \"y\"))))" },
    { "mod_app_req",  TEST_MODULE,
      "@app\n"
      "  name: \"X\"\n"
      "\n@needs\n"
      "  deck_level: 1\n",
      "(module (app (name (str \"X\"))) (requires (deck_level (int 1))))" },
    { "mod_on",       TEST_MODULE,
      "@on launch:\n"
      "  log.info(\"hi\")\n",
      "(module (on :launch (do (call (dot (ident log) info) (str \"hi\")))))" },
    /* Spec 02-deck-app §11 — dotted event path with named-binder clause
     * and no trailing colon (spec-canonical form). */
    { "mod_on_os_binders", TEST_MODULE,
      "@on os.wifi_changed (ssid: s, connected: c)\n"
      "  log.info(\"wifi\")\n",
      "(module (on :os.wifi_changed (ssid: (pat_ident s) connected: (pat_ident c)) (do (call (dot (ident log) info) (str \"wifi\")))))" },
    /* §11 — value-pattern clause (filters dispatch). */
    { "mod_on_hw_pattern", TEST_MODULE,
      "@on hardware.button (id: 0, action: :press):\n"
      "  log.info(\"pressed\")\n",
      "(module (on :hardware.button (id: (pat_lit (int 0)) action: (pat_lit (atom :press))) (do (call (dot (ident log) info) (str \"pressed\")))))" },
    { "mod_machine",  TEST_MODULE,
      "@machine m\n"
      "  state a:\n"
      "    transition :b\n",
      "(module (machine m (state a (transition :b))))" },
    /* Spec 02-deck-app §8.2 — `state :atom` form (no trailing colon)
     * plus top-level `initial :atom` declaration. */
    { "mod_machine_spec_form", TEST_MODULE,
      "@machine onboard\n"
      "  state :welcome\n"
      "    transition :collect\n"
      "  state :collect\n"
      "    transition :done\n"
      "  state :done\n"
      "    transition :welcome\n"
      "  initial :welcome\n",
      "(module (machine onboard (initial :welcome) (state welcome (transition :collect)) (state collect (transition :done)) (state done (transition :welcome))))" },
    { "mod_state_enter", TEST_MODULE,
      "@machine m\n"
      "  state idle:\n"
      "    on enter:\n"
      "      log.info(\"idle\")\n",
      "(module (machine m (state idle (state_hook enter (do (call (dot (ident log) info) (str \"idle\")))))))" },

    /* --- DL2 F21.1: fn declarations --- */
    { "mod_fn_inline",    TEST_MODULE, "fn add (a, b) = a + b\n",
      "(module (fn add (params a b) (binop + (ident a) (ident b))))" },
    { "mod_fn_no_args",   TEST_MODULE, "fn answer () = 42\n",
      "(module (fn answer (params) (int 42)))" },
    { "mod_fn_typed",     TEST_MODULE, "fn bmi (w: float, h: float) -> float = w / h\n",
      "(module (fn bmi (params w h) (binop / (ident w) (ident h))))" },
    { "mod_fn_block",     TEST_MODULE,
      "fn bmi (w, h) =\n"
      "  let h2 = h * h\n"
      "  w / h2\n",
      "(module (fn bmi (params w h) (do (let h2 (binop * (ident h) (ident h)) _) (binop / (ident w) (ident h2)))))" },
    /* Multi-line fn body: parse_suite always wraps in AST_DO even when
     * the body is a single expression — the printer mirrors that wrap. */
    { "mod_fn_recursion", TEST_MODULE,
      "fn fact (n) =\n"
      "  if n <= 1 then 1 else n * fact(n - 1)\n",
      "(module (fn fact (params n) (do (if (binop <= (ident n) (int 1)) (int 1) (binop * (ident n) (call (ident fact) (binop - (ident n) (int 1))))))))" },

    /* --- DL2 F28: opaque blocks (@flow/@migrate/@assets still parse-only,
     * @machine.before / @machine.after now produce AST_ON with reserved
     * event names and are executed around each machine transition) --- */
    { "mod_machine_before", TEST_MODULE,
      "@machine.before\n"
      "  log.info(\"about to transition\")\n",
      "(module (on :__machine_before (do (call (dot (ident log) info) (str \"about to transition\")))))" },
    { "mod_machine_after", TEST_MODULE,
      "@machine.after\n"
      "  log.info(\"transition complete\")\n",
      "(module (on :__machine_after (do (call (dot (ident log) info) (str \"transition complete\")))))" },
    /* @flow / @flow.step — sugar that desugars to @machine at parse time
     * (F28.2). Each step becomes a state; transitions chain in order. */
    { "mod_flow", TEST_MODULE,
      "@flow signup\n"
      "  step welcome:\n"
      "    log.info(\"hi\")\n",
      "(module (machine signup (state welcome (state_hook enter (do (call (dot (ident log) info) (str \"hi\")))))))" },
    /* @migrate — version migration block (F28.4: fully parsed; runtime
     * runs bodies whose N >= stored NVS version at app_load). */
    { "mod_migration", TEST_MODULE,
      "@migrate\n"
      "  from 1:\n"
      "    log.info(\"migrating\")\n",
      "(module (migration (from 1 (do (call (dot (ident log) info) (str \"migrating\"))))))" },
    /* @assets — asset bundle declarations (F28.5: now fully parsed and
     * exposed at runtime via asset.path(name)). */
    { "mod_assets", TEST_MODULE,
      "@assets\n"
      "  icon: \"icon.png\"\n"
      "  font: \"mono.ttf\"\n",
      "(module (assets (icon \"icon.png\") (font \"mono.ttf\")))" },

    /* --- concept #57 option bag + concept #58 `on [atom]? -> action` --- */
    /* Inline option after label: `trigger "go" badge: 3`. */
    { "content_opt_inline", TEST_MODULE,
      "@app\n  name: \"X\"\n  id: \"y\"\n  version: \"1.0.0\"\n  edition: 2026\n"
      "@needs\n  deck_level: 1\n"
      "@machine m\n"
      "  state main:\n"
      "    content =\n"
      "      trigger \"go\" badge: 3\n",
      "(module (app (name (str \"X\")) (id (str \"y\")) (version (str \"1.0.0\")) (edition (int 2026))) (requires (deck_level (int 1))) "
      "(machine m (state main (content_block (content_item :trigger \"go\" (badge (int 3)))))))" },
    /* Trailing `-> action` promotes earlier action_expr to data_expr
     * (label). Use a call-shaped option value so the TOK_IDENT `->`
     * lambda rule (§7.1) doesn't consume the arrow. */
    { "content_tail_arrow", TEST_MODULE,
      "@app\n  name: \"X\"\n  id: \"y\"\n  version: \"1.0.0\"\n  edition: 2026\n"
      "@needs\n  deck_level: 1\n"
      "@machine m\n"
      "  state main:\n"
      "    content =\n"
      "      trigger app.name badge: unread_badge(app.id) -> apps.launch(app.id)\n",
      "(module (app (name (str \"X\")) (id (str \"y\")) (version (str \"1.0.0\")) (edition (int 2026))) (requires (deck_level (int 1))) "
      "(machine m (state main (content_block (content_item :trigger (call (dot (ident apps) launch) (dot (ident app) id)) "
      "(data (dot (ident app) name)) (badge (call (ident unread_badge) (dot (ident app) id))))))))" },
    /* `on [atom]? -> action` form used by form submit + input intents. */
    { "content_on_arrow", TEST_MODULE,
      "@app\n  name: \"X\"\n  id: \"y\"\n  version: \"1.0.0\"\n  edition: 2026\n"
      "@needs\n  deck_level: 1\n"
      "@machine m\n"
      "  state main:\n"
      "    content =\n"
      "      form on submit -> log.info(\"ok\")\n",
      "(module (app (name (str \"X\")) (id (str \"y\")) (version (str \"1.0.0\")) (edition (int 2026))) (requires (deck_level (int 1))) "
      "(machine m (state main (content_block (content_item :form (call (dot (ident log) info) (str \"ok\")))))))" },

    /* --- errors --- */
    { "err_no_decorator", TEST_ERROR, "foo",        "expected @app" },
    { "err_unknown_dec",  TEST_ERROR, "@wtf",       "unknown top-level decorator" },
    /* `fn ()` is now valid (DL2 F21.2 anonymous lambda); the actual
     * error fires later when no `=` follows the empty param list. */
    { "err_fn_no_name",   TEST_ERROR, "fn 42",      "expected function name or '(' after 'fn'" },
    { "err_fn_no_paren",  TEST_ERROR, "fn add a, b", "expected '(' after fn name" },
    { "err_fn_no_assign", TEST_ERROR, "fn add (a, b) a + b", "expected '=' in fn declaration" },
    { "err_missing_colon", TEST_ERROR,
      "@app\n  name \"X\"\n",
      "expected ':' after app field name" },
    /* Expression-level errors surface through parse_module as
     * "expected @app..." because module top-level is decorator-only
     * in DL1. The nuanced expression errors emerge once we call
     * deck_parser_parse_expr — exercised by the non-error cases above. */
    { "err_bare_expr",    TEST_ERROR, "f(1",        "expected @app" },
    { "err_bare_match",   TEST_ERROR, "match x\n  1 2\n", "expected @app" },
};

#define N_CASES (sizeof(CASES) / sizeof(CASES[0]))

static bool run_case(const parser_case_t *c)
{
    deck_arena_t   arena;
    deck_parser_t  p;
    deck_arena_init(&arena, 0);
    deck_parser_init(&p, &arena, c->src, (uint32_t)strlen(c->src));

    ast_node_t *root =
        c->mode == TEST_EXPR ? deck_parser_parse_expr(&p)
                             : deck_parser_parse_module(&p);

    bool ok = true;
    if (c->mode == TEST_ERROR) {
        deck_err_t err = deck_parser_err_code(&p);
        if (err == DECK_LOAD_OK) {
            ESP_LOGE(TAG, "  [%s] expected error, got OK", c->name);
            ok = false;
        } else if (c->expect) {
            const char *msg = deck_parser_err_msg(&p);
            if (!msg || !strstr(msg, c->expect)) {
                ESP_LOGE(TAG, "  [%s] expected err msg to contain \"%s\", got \"%s\"",
                         c->name, c->expect, msg ? msg : "(null)");
                ok = false;
            }
        }
    } else {
        if (!root) {
            ESP_LOGE(TAG, "  [%s] parse failed: %s (%u:%u)",
                     c->name,
                     deck_parser_err_msg(&p) ? deck_parser_err_msg(&p) : "?",
                     (unsigned)deck_parser_err_line(&p),
                     (unsigned)deck_parser_err_col(&p));
            ok = false;
        } else {
            char buf[1024];
            ast_print(root, buf, sizeof(buf));
            /* Tolerate trailing spaces from do-block printers. */
            size_t blen = strlen(buf);
            while (blen > 0 && buf[blen - 1] == ' ') buf[--blen] = '\0';
            if (strcmp(buf, c->expect) != 0) {
                ESP_LOGE(TAG, "  [%s]", c->name);
                ESP_LOGE(TAG, "    got:  %s", buf);
                ESP_LOGE(TAG, "    want: %s", c->expect);
                ok = false;
            }
        }
    }

    deck_arena_reset(&arena);
    return ok;
}

deck_err_t deck_parser_run_selftest(void)
{
    uint32_t pass = 0, fail = 0;
    for (size_t i = 0; i < N_CASES; i++) {
        if (run_case(&CASES[i])) pass++;
        else                     fail++;
    }
    if (fail) {
        ESP_LOGE(TAG, "parser selftest: %u/%u pass, %u fail",
                 (unsigned)pass, (unsigned)N_CASES, (unsigned)fail);
        return DECK_LOAD_PARSE_ERROR;
    }
    ESP_LOGI(TAG, "parser selftest: PASS (%u cases)", (unsigned)N_CASES);
    return DECK_RT_OK;
}
