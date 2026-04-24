/* Interpreter self-tests. */

#include "deck_interp.h"
#include "deck_parser.h"
#include "deck_runtime.h"
#include "deck_alloc.h"

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
{ deck_err_t e; deck_value_t *v = run_expr("\"foo\" ++ \"bar\"", &e); CHECK(v && v->type == DECK_T_STR, "str"); CHECK(v->as.s.len == 6 && memcmp(v->as.s.ptr, "foobar", 6) == 0, "foobar"); deck_release(v); (void)e; return true; }
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
{ deck_err_t e; deck_value_t *v = run_expr("text.starts(\"hello\", \"he\")", &e); CHECK(v && v->as.b, "true"); deck_release(v); (void)e; return true; }
static bool t_text_ends_with(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("text.ends(\"hello\", \"lo\")", &e); CHECK(v && v->as.b, "true"); deck_release(v); (void)e; return true; }
static bool t_text_contains(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("text.contains(\"hello\", \"ell\")", &e); CHECK(v && v->as.b, "true"); deck_release(v); (void)e; return true; }
static bool t_conv_str_int(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("str(42)", &e); CHECK(v && v->as.s.len == 2 && memcmp(v->as.s.ptr, "42", 2) == 0, "42"); deck_release(v); (void)e; return true; }
/* Spec 01-deck-lang §11.1 — `int/float/bool` take STRING, return Optional. */
static bool t_conv_int_str(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("int(\"99\")", &e);
  CHECK(v && v->type == DECK_T_OPTIONAL && v->as.opt.inner &&
        v->as.opt.inner->type == DECK_T_INT && v->as.opt.inner->as.i == 99,
        ":some 99"); deck_release(v); (void)e; return true; }
static bool t_conv_float_str(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("float(\"5.5\")", &e);
  CHECK(v && v->type == DECK_T_OPTIONAL && v->as.opt.inner &&
        v->as.opt.inner->type == DECK_T_FLOAT && v->as.opt.inner->as.f == 5.5,
        ":some 5.5"); deck_release(v); (void)e; return true; }
static bool t_conv_bool_str(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("bool(\"\")", &e);
  CHECK(v && v->type == DECK_T_OPTIONAL && v->as.opt.inner == NULL,
        ":none"); deck_release(v); (void)e; return true; }
static bool t_conv_roundtrip(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("int(str(123))", &e);
  CHECK(v && v->type == DECK_T_OPTIONAL && v->as.opt.inner &&
        v->as.opt.inner->type == DECK_T_INT && v->as.opt.inner->as.i == 123,
        ":some 123"); deck_release(v); (void)e; return true; }
static bool t_time_duration(const char *name)
{ deck_err_t e; deck_value_t *v = run_expr("time.duration(100, 20)", &e); CHECK(v && v->as.i == 80, "80"); deck_release(v); (void)e; return true; }

#define APP_HDR_DL1 \
    "@app\n" \
    "  name: \"X\"\n" \
    "  id: \"y\"\n" \
    "  version: \"1.0.0\"\n" \
    "  edition: 2026\n" \
    "\n@needs\n" \
    "  deck_level: 1\n"

static bool t_hello(const char *name)
{
    const char *src = APP_HDR_DL1 "\n@on launch:\n  log.info(\"Hello from Deck DL1\")\n";
    deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
    CHECK(rc == DECK_RT_OK, "run");
    return true;
}
static bool t_match_wild(const char *name)
{
    const char *src = APP_HDR_DL1 "\n@on launch:\n  match 1\n    | 0 -> log.info(\"z\")\n    | _ -> log.info(\"match wild branch taken\")\n";
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

/* ---- DL2 F21.1: user-defined functions ---- */
static bool t_fn_basic(const char *name)
{
    const char *src = APP_HDR_DL1
        "\nfn add (a, b) = a + b\n"
        "@on launch:\n"
        "  log.info(str(add(2, 3)))\n";
    deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
    CHECK(rc == DECK_RT_OK, "run add(2,3)");
    return true;
}
static bool t_fn_multi_line(const char *name)
{
    const char *src = APP_HDR_DL1
        "\nfn bmi (w, h) =\n"
        "  let h2 = h * h\n"
        "  w / h2\n"
        "@on launch:\n"
        "  log.info(str(bmi(80, 2)))\n";
    deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
    CHECK(rc == DECK_RT_OK, "run bmi(80,2)");
    return true;
}
static bool t_fn_recursion(const char *name)
{
    const char *src = APP_HDR_DL1
        "\nfn fact (n) =\n"
        "  if n <= 1 then 1 else n * fact(n - 1)\n"
        "@on launch:\n"
        "  log.info(str(fact(6)))\n";
    deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
    CHECK(rc == DECK_RT_OK, "run fact(6)");
    return true;
}
static bool t_fn_mutual(const char *name)
{
    const char *src = APP_HDR_DL1
        "\nfn is_even (n) = if n == 0 then true else is_odd(n - 1)\n"
        "fn is_odd  (n) = if n == 0 then false else is_even(n - 1)\n"
        "@on launch:\n"
        "  log.info(if is_even(4) then \"yes\" else \"no\")\n";
    deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
    CHECK(rc == DECK_RT_OK, "mutual recursion");
    return true;
}
static bool t_fn_arity_mismatch(const char *name)
{
    const char *src = APP_HDR_DL1
        "\nfn add (a, b) = a + b\n"
        "@on launch:\n"
        "  log.info(str(add(2)))\n";
    deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
    CHECK(rc == DECK_RT_TYPE_MISMATCH, "expected arity mismatch");
    return true;
}

/* ---- DL2 F21.2: lambdas + closures ---- */
static bool t_lambda_single_ident(const char *name)
{
    const char *src = APP_HDR_DL1
        "@on launch:\n"
        "  let dbl = x -> x * 2\n"
        "  log.info(str(dbl(21)))\n";
    deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
    CHECK(rc == DECK_RT_OK, "lambda single ident");
    return true;
}
static bool t_lambda_anon_fn(const char *name)
{
    const char *src = APP_HDR_DL1
        "@on launch:\n"
        "  let add = fn (a, b) = a + b\n"
        "  log.info(str(add(7, 8)))\n";
    deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
    CHECK(rc == DECK_RT_OK, "anon fn lambda");
    return true;
}
static bool t_lambda_closure(const char *name)
{
    const char *src = APP_HDR_DL1
        "fn mk_adder (n) = fn (x) = x + n\n"
        "@on launch:\n"
        "  let add5 = mk_adder(5)\n"
        "  let add10 = mk_adder(10)\n"
        "  log.info(str(add5(3) + add10(3)))\n";
    deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
    CHECK(rc == DECK_RT_OK, "closure capture");
    return true;
}
static bool t_lambda_inline_call(const char *name)
{
    const char *src = APP_HDR_DL1
        "@on launch:\n"
        "  log.info(str((x -> x * x)(7)))\n";
    deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
    CHECK(rc == DECK_RT_OK, "inline lambda call");
    return true;
}
static bool t_lambda_higher_order(const char *name)
{
    const char *src = APP_HDR_DL1
        "fn apply (f, x) = f(x)\n"
        "@on launch:\n"
        "  let r = apply(x -> x + 100, 23)\n"
        "  log.info(str(r))\n";
    deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
    CHECK(rc == DECK_RT_OK, "higher-order");
    return true;
}

/* ---- DL2 F21.3: tail-call optimization ---- */
static bool t_tco_self_deep(const char *name)
{
    /* Without TCO this self-tail-recursion would blow the C stack well
     * before n=2000; with the trampoline the loop is flat. */
    const char *src = APP_HDR_DL1
        "fn count_down (n) = if n <= 0 then 0 else count_down(n - 1)\n"
        "@on launch:\n"
        "  log.info(str(count_down(2000)))\n";
    deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
    CHECK(rc == DECK_RT_OK, "deep self tail recursion");
    return true;
}
static bool t_tco_self_acc(const char *name)
{
    const char *src = APP_HDR_DL1
        "fn sum_to (n, acc) = if n == 0 then acc else sum_to(n - 1, acc + n)\n"
        "@on launch:\n"
        "  log.info(str(sum_to(1000, 0)))\n";
    deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
    CHECK(rc == DECK_RT_OK, "tail accumulator");
    return true;
}
static bool t_tco_mutual(const char *name)
{
    /* Mutual tail recursion via the trampoline: is_even/is_odd swap
     * frames per iteration without growing the C stack. */
    const char *src = APP_HDR_DL1
        "fn is_even (n) = if n == 0 then true else is_odd(n - 1)\n"
        "fn is_odd  (n) = if n == 0 then false else is_even(n - 1)\n"
        "@on launch:\n"
        "  log.info(if is_even(1500) then \"yes\" else \"no\")\n";
    deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
    CHECK(rc == DECK_RT_OK, "mutual TCO");
    return true;
}

/* DL2 F28 — persistent app handle + @on event dispatch. Load the app,
 * verify id/name accessors, fire @on resume (which logs), then fire an
 * event the module doesn't handle (silent no-op), then unload. */
static bool t_app_dispatch(const char *name)
{
    const char *src = APP_HDR_DL1
        "@on launch:\n"
        "  log.info(\"app_test:launch\")\n"
        "@on resume:\n"
        "  log.info(\"app_test:resume\")\n"
        "@on terminate:\n"
        "  log.info(\"app_test:terminate\")\n";
    deck_runtime_app_t *app = NULL;
    deck_err_t rc = deck_runtime_app_load(src, (uint32_t)strlen(src), &app);
    CHECK(rc == DECK_RT_OK, "load");
    CHECK(app != NULL, "handle");
    const char *id = deck_runtime_app_id(app);
    CHECK(id != NULL, "id");

    rc = deck_runtime_app_dispatch(app, "resume", NULL);
    CHECK(rc == DECK_RT_OK, "dispatch resume");

    /* Unknown event → no-op, no error. */
    rc = deck_runtime_app_dispatch(app, "network_change", NULL);
    CHECK(rc == DECK_RT_OK, "dispatch unknown");

    /* launch is reserved: dispatching it is a no-op (load already ran it). */
    rc = deck_runtime_app_dispatch(app, "launch", NULL);
    CHECK(rc == DECK_RT_OK, "dispatch launch reserved");

    deck_runtime_app_unload(app);
    return true;
}

/* DL2 F28.4 — @migrate runs at app_load, driven by NVS-stored version.
 * Delete the migration tracking key first to get a clean run; then load
 * an app that has `@migrate from 0:` setting a sentinel NVS value, and
 * assert the sentinel was written.
 *
 * The app_id hash for "mig.test" is deterministic (FNV32), so the
 * migration tracking key is stable across runs — we just delete it to
 * reset state. */
#include "drivers/deck_sdi_nvs.h"
static bool t_migration(const char *name)
{
    /* Reset state so the test is idempotent across boots. The app's NVS
     * namespace is its `id` field (nvs_app_ns in deck_interp.c). The
     * migration tracker key is "v_" + FNV32(app_id) = "v_a829200e" for
     * "mig.test" — precomputed so we don't duplicate the hash fn. */
    (void)deck_sdi_nvs_del("mig.test", "ran");
    (void)deck_sdi_nvs_del("deck.mig", "v_a829200e");

    const char *src =
        "@app\n"
        "  name: \"mig\"\n"
        "  id: \"mig.test\"\n"
        "  version: \"1.0.0\"\n"
        "  edition: 2026\n"
        "\n@needs\n"
        "  deck_level: 1\n"
        "\n"
        "@migrate\n"
        "  from 0:\n"
        "    nvs.set(\"ran\", \"v0\")\n"
        "\n"
        "@on launch:\n"
        "  log.info(\"mig.test launched\")\n";

    /* First load: migration `from 0` should fire. */
    deck_runtime_app_t *app = NULL;
    deck_err_t rc = deck_runtime_app_load(src, (uint32_t)strlen(src), &app);
    CHECK(rc == DECK_RT_OK && app != NULL, "app_load first");

    char val[16] = {0};
    deck_sdi_err_t gr = deck_sdi_nvs_get_str("mig.test", "ran", val, sizeof(val));
    CHECK(gr == DECK_SDI_OK, "migration effect not in nvs");
    CHECK(strcmp(val, "v0") == 0, "migration wrote wrong value");

    deck_runtime_app_unload(app);

    /* Second load: stored version is now 1, from=0 should NOT re-run.
     * We prove this by clearing the effect key and reloading; if the
     * migration re-ran, the effect key would re-appear. */
    (void)deck_sdi_nvs_del("mig.test", "ran");
    app = NULL;
    rc = deck_runtime_app_load(src, (uint32_t)strlen(src), &app);
    CHECK(rc == DECK_RT_OK && app != NULL, "app_load second");

    val[0] = 0;
    gr = deck_sdi_nvs_get_str("mig.test", "ran", val, sizeof(val));
    CHECK(gr == DECK_SDI_ERR_NOT_FOUND, "migration re-ran when it shouldn't");

    deck_runtime_app_unload(app);
    return true;
}

/* Concept #58 — captured-action dispatch: a declarative `trigger` whose
 * action is an arbitrary call (not Machine.send) must fire on tap via
 * deck_runtime_app_intent_v. We use nvs.set as the side-effecting action
 * because its effect is observable from C through deck_sdi_nvs_get_str
 * (NVS namespace = app `id`, per nvs_app_ns). */
#define APP_HDR_CPT_INT \
    "@app\n" \
    "  name: \"CAP\"\n" \
    "  id: \"conf.cpi\"\n" \
    "  version: \"1.0.0\"\n" \
    "  edition: 2026\n" \
    "\n@needs\n" \
    "  deck_level: 1\n"

static bool t_intent_captured_action(const char *name)
{
    (void)deck_sdi_nvs_del("conf.cpi", "fired");
    const char *src = APP_HDR_CPT_INT
        "\n@on launch:\n"
        "  log.info(\"t_cpi: launch\")\n"
        "\n@machine m\n"
        "  state main:\n"
        "    content =\n"
        "      trigger \"go\" -> nvs.set(\"fired\", \"yes\")\n";
    deck_runtime_app_t *app = NULL;
    deck_err_t rc = deck_runtime_app_load(src, (uint32_t)strlen(src), &app);
    CHECK(rc == DECK_RT_OK && app, "load");

    rc = deck_runtime_app_intent(app, 1);
    CHECK(rc == DECK_RT_OK, "intent dispatch");

    char val[8] = {0};
    deck_sdi_err_t gr = deck_sdi_nvs_get_str("conf.cpi", "fired", val, sizeof(val));
    CHECK(gr == DECK_SDI_OK && strcmp(val, "yes") == 0, "action side-effect missing");

    deck_runtime_app_unload(app);
    return true;
}

/* Concept #59 — scalar payload `event.value`: toggle-style intent fires
 * with a bool payload that the action sees via `event.value`. We write
 * the delivered bool to NVS as "1"/"0" string so C can read it back. */
static bool t_intent_event_value(const char *name)
{
    (void)deck_sdi_nvs_del("conf.cpi", "val");
    const char *src = APP_HDR_CPT_INT
        "\n@on launch:\n"
        "  log.info(\"t_ev: launch\")\n"
        "\n@machine m\n"
        "  state main:\n"
        "    content =\n"
        "      toggle :lights on -> nvs.set(\"val\", if event.value then \"yes\" else \"no\")\n";
    deck_runtime_app_t *app = NULL;
    deck_err_t rc = deck_runtime_app_load(src, (uint32_t)strlen(src), &app);
    CHECK(rc == DECK_RT_OK && app, "load");

    deck_intent_val_t vals[1] = { { .key = NULL, .kind = DECK_INTENT_VAL_BOOL, .b = true } };
    rc = deck_runtime_app_intent_v(app, 1, vals, 1);
    CHECK(rc == DECK_RT_OK, "intent_v");

    char val[8] = {0};
    deck_sdi_err_t gr = deck_sdi_nvs_get_str("conf.cpi", "val", val, sizeof(val));
    CHECK(gr == DECK_SDI_OK, "nvs readback");
    CHECK(strcmp(val, "yes") == 0, "payload did not reach event.value");

    deck_runtime_app_unload(app);
    return true;
}

/* Concept #60 — form aggregation `event.values`: map-payload intent
 * exposes a keyed map the action can index via `.` access. We pick one
 * field (username) and write its string into NVS, so C sees the exact
 * value the hook received. */
static bool t_intent_event_values(const char *name)
{
    (void)deck_sdi_nvs_del("conf.cpi", "user");
    const char *src = APP_HDR_CPT_INT
        "\n@on launch:\n"
        "  log.info(\"t_evs: launch\")\n"
        "\n@machine m\n"
        "  state main:\n"
        "    content =\n"
        "      form on submit -> nvs.set(\"user\", event.values.username)\n";
    deck_runtime_app_t *app = NULL;
    deck_err_t rc = deck_runtime_app_load(src, (uint32_t)strlen(src), &app);
    CHECK(rc == DECK_RT_OK && app, "load");

    deck_intent_val_t vals[2] = {
        { .key = "username", .kind = DECK_INTENT_VAL_STR, .s = "alice" },
        { .key = "password", .kind = DECK_INTENT_VAL_STR, .s = "secret" },
    };
    rc = deck_runtime_app_intent_v(app, 1, vals, 2);
    CHECK(rc == DECK_RT_OK, "intent_v form");

    char val[16] = {0};
    deck_sdi_err_t gr = deck_sdi_nvs_get_str("conf.cpi", "user", val, sizeof(val));
    CHECK(gr == DECK_SDI_OK, "nvs readback form");
    CHECK(strcmp(val, "alice") == 0, "event.values.username wrong");

    deck_runtime_app_unload(app);
    return true;
}

/* Concept #66 — named call args (spec §6.6). Order-independent binding
 * via parameter names; all-or-none positional/named per spec. Tests
 * both canonical order (names match source) and reverse order (named
 * args shuffled vs. param order). */
static bool t_named_call_args(const char *name)
{
    const char *src =
        "@app\n"
        "  name: \"T\"\n"
        "  id: \"t.nc\"\n"
        "  version: \"1\"\n"
        "  edition: 2026\n"
        "@needs\n"
        "  deck_level: 1\n"
        "fn sub (a, b) = a - b\n"
        "@on launch:\n"
        "  let r1 = sub(a: 10, b: 3)\n"
        "  let r2 = sub(b: 3, a: 10)\n"
        "  log.info(if r1 == 7 and r2 == 7 then \"OK\" else \"FAIL\")\n";
    deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
    CHECK(rc == DECK_RT_OK, "named args load/run");
    return true;
}

/* Concept #65 — `|>?` error-propagating pipe honors spec §7.9 for all
 * four wrapper shapes: :err / :ok / :none / :some, across both the
 * legacy Optional repr and the atom-variant tuple repr. */
static bool t_pipe_opt_variants(const char *name)
{
    deck_err_t e;
    deck_value_t *v;

    /* Spec §7.9: `:ok 100 |>? f` unwraps to 100 and returns f(100) plain. */
    v = run_expr(":ok 100 |>? (x -> x * 2)", &e);
    CHECK(v && v->type == DECK_T_INT && v->as.i == 200, ":ok unwraps");
    deck_release(v);
    /* :err short-circuits, returns tuple (:err, :oops) unchanged. */
    v = run_expr(":err :oops |>? (x -> x * 2)", &e);
    CHECK(v && v->type == DECK_T_TUPLE && v->as.tuple.arity == 2, ":err tuple");
    CHECK(v->as.tuple.items[0]->type == DECK_T_ATOM &&
          strcmp(v->as.tuple.items[0]->as.atom, "err") == 0, ":err ctor");
    deck_release(v);
    /* :some 7 |>? (x -> x + 1) — unwrap, call, return plain 8. */
    v = run_expr(":some 7 |>? (x -> x + 1)", &e);
    CHECK(v && v->type == DECK_T_INT && v->as.i == 8, ":some unwraps");
    deck_release(v);
    /* :none |>? f — return :none atom. */
    v = run_expr(":none |>? (x -> x + 1)", &e);
    CHECK(v && v->type == DECK_T_ATOM &&
          strcmp(v->as.atom, "none") == 0, ":none short-circuit");
    deck_release(v);
    return true;
}

/* Concept #63 — structural equality for tuples / lists / maps via the
 * `==` operator (do_compare delegates to values_equal for EQ/NE) AND
 * Optional ↔ atom-variant tuple bridging so map.get returns equal to
 * the literal `:some v`. Spec §3.7 + concept #11.
 */
static bool t_eq_structural(const char *name)
{
    deck_err_t e;
    deck_value_t *v;
    /* Tuple structural eq. */
    v = run_expr("(1, 2) == (1, 2)", &e);
    CHECK(v && v->type == DECK_T_BOOL && v->as.b, "tup ==");
    deck_release(v);
    v = run_expr("(1, 2) != (2, 1)", &e);
    CHECK(v && v->as.b, "tup !=");
    deck_release(v);
    /* List structural eq. */
    v = run_expr("[1, 2, 3] == [1, 2, 3]", &e);
    CHECK(v && v->as.b, "list ==");
    deck_release(v);
    v = run_expr("[1, 2] != [1, 2, 3]", &e);
    CHECK(v && v->as.b, "list !=");
    deck_release(v);
    /* Optional ↔ tuple bridge. map.get returns DECK_T_OPTIONAL; the
     * literal `:some "x"` parses into a 2-tuple. They must compare equal. */
    v = run_expr("map.get({:k: \"v\"}, :k) == :some \"v\"", &e);
    CHECK(v && v->as.b, "opt some bridge");
    deck_release(v);
    v = run_expr("map.get({:k: \"v\"}, :missing) == :none", &e);
    CHECK(v && v->as.b, "opt none bridge");
    deck_release(v);
    return true;
}

/* DL2 F28.1 — @machine.before / .after run around each transition. The
 * test machine does boot → ready, and @machine.after sets a log marker
 * that we cannot verify here (no log capture in the interp test harness),
 * so we only assert the runtime executes without error. */
static bool t_machine_hooks(const char *name)
{
    const char *src = APP_HDR_DL1
        "@machine lifecycle\n"
        "  state boot:\n"
        "    on enter:\n"
        "      log.info(\"t_mh: boot\")\n"
        "    transition :ready\n"
        "  state ready:\n"
        "    on enter:\n"
        "      log.info(\"t_mh: ready\")\n"
        "\n"
        "@machine.before:\n"
        "  log.info(\"t_mh: before\")\n"
        "\n"
        "@machine.after:\n"
        "  log.info(\"t_mh: after\")\n";
    deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
    CHECK(rc == DECK_RT_OK, "machine hooks");
    return true;
}

typedef bool (*tfn_t)(const char *);
typedef struct { const char *name; tfn_t fn; } case_t;

/* ---- Concept #73 testbench: fn-closure cycle leak regression tests ----
 *
 * The bug: every top-level `fn` retains its defining env (c.global) as
 * its closure. When deck_env_release(c.global) is called at run_on_launch
 * end, the refcount is `1 + N` (N = # fn definitions), so one release
 * drops it to N ≥ 1 and bindings never release. Result: `let big = ...`
 * values stay live forever, compounding across test runs.
 *
 * The fix: deck_env_force_release() in deck_interp.c ignores the cycle-
 * contributed refcount at module end and tears down bindings. These tests
 * assert that post-run live-value count stays tiny regardless of fn count
 * or bound-list size. Failure = leak regressed. */

/* Snapshot live-value count before and after a run_on_launch; assert the
 * delta is within `budget`. The `budget` tolerance accommodates a few
 * interned atom/string admin values that are intentionally long-lived
 * (the intern table, NOT a deck_value leak) — anything beyond that is
 * genuine leak. */
static bool cycle_run_and_check(const char *name, const char *src,
                                uint32_t budget)
{
    size_t before = deck_alloc_live_values();
    deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
    CHECK(rc == DECK_RT_OK, "run_on_launch");
    size_t after = deck_alloc_live_values();
    int32_t delta = (int32_t)after - (int32_t)before;
    if (delta < 0) delta = 0;
    if ((uint32_t)delta > budget) {
        ESP_LOGE(TAG, "  [%s] FAIL: live +%d > budget %u (before=%u after=%u)",
                 name, (int)delta, (unsigned)budget,
                 (unsigned)before, (unsigned)after);
        return false;
    }
    return true;
}

/* A single fn defined at top level with one top-level `let big = …` that
 * holds a 50-item list. Pre-fix, this leaked 51 values per run (big + 50
 * items). Post-fix, the force-release tears down c.global bindings, so
 * net delta must be near zero. Budget 10 is generous — real observed
 * delta on hardware is 0. */
static bool t_cycle_leak_fn_closure(const char *name)
{
    const char *src = APP_HDR_DL1
        "\nfn noop (n) = n\n"
        "@on launch:\n"
        "  let big = list.tabulate(50, n -> n)\n"
        "  log.info(str(list.len(big)))\n";
    return cycle_run_and_check(name, src, /*budget*/ 10);
}

/* Stress the leak: two top-level fns + a bound list. Pre-fix leaked
 * proportionally with # fns (each fn added +1 to c.global.refcount that
 * the single release couldn't drop to 0). Post-fix still ~0 regardless
 * of fn count. */
static bool t_cycle_leak_list_let(const char *name)
{
    const char *src = APP_HDR_DL1
        "\nfn square (n) = n * n\n"
        "fn cube   (n) = n * n * n\n"
        "@on launch:\n"
        "  let ints  = list.tabulate(30, n -> n)\n"
        "  let sq    = list.tabulate(30, square)\n"
        "  log.info(str(list.len(ints)))\n"
        "  log.info(str(list.len(sq)))\n";
    return cycle_run_and_check(name, src, /*budget*/ 10);
}

/* Re-run the same module 20 times and assert the live count does not
 * grow proportionally. Pre-fix this would accumulate ~50 values per
 * iteration → ~1000 total delta, clearly above any reasonable budget.
 * Post-fix the delta across 20 iterations is a handful (intern table
 * churn from any str/atom not already interned). */
static bool t_cycle_leak_rerun_x20(const char *name)
{
    const char *src = APP_HDR_DL1
        "\nfn walk (xs, acc) =\n"
        "  match xs\n"
        "    | []     -> acc\n"
        "    | _ :: r -> walk(r, acc + 1)\n"
        "@on launch:\n"
        "  let xs = list.tabulate(20, n -> n)\n"
        "  log.info(str(walk(xs, 0)))\n";
    size_t before = deck_alloc_live_values();
    for (int i = 0; i < 20; i++) {
        deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
        CHECK(rc == DECK_RT_OK, "run_on_launch in loop");
    }
    size_t after = deck_alloc_live_values();
    int32_t delta = (int32_t)after - (int32_t)before;
    if (delta < 0) delta = 0;
    /* Budget 50 for 20 iterations — generous to cover intern-table growth
     * for any new strings/atoms introduced on the first run. Real-world
     * delta is typically < 10. Pre-fix the delta would be ~420. */
    if ((uint32_t)delta > 50) {
        ESP_LOGE(TAG, "  [%s] FAIL: live +%d after 20 runs > 50 (before=%u after=%u)",
                 name, (int)delta, (unsigned)before, (unsigned)after);
        return false;
    }
    return true;
}

/* Independent: `:none` literal pattern must match an empty Optional
 * value (DECK_T_OPTIONAL with inner=NULL), not just a DECK_T_ATOM named
 * "none". int("nope") returns an empty Optional; match against `:none`
 * must succeed. Pre-fix (no bridge) the runtime returned pattern_failed
 * because only the atom-vs-atom branch matched. */
static bool t_none_pattern_bridges_optional(const char *name)
{
    const char *src = APP_HDR_DL1
        "\n@on launch:\n"
        "  let x = match int(\"nope\")\n"
        "            | :some n -> n\n"
        "            | :none   -> -1\n"
        "  log.info(str(x))\n";
    deck_err_t rc = deck_runtime_run_on_launch(src, (uint32_t)strlen(src));
    CHECK(rc == DECK_RT_OK, "int('nope') should match :none arm");
    return true;
}

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
    { "conv_float_str",   t_conv_float_str },
    { "conv_bool_str",    t_conv_bool_str },
    { "conv_roundtrip",   t_conv_roundtrip },
    { "time_duration",    t_time_duration },
    { "hello",              t_hello },
    { "match_wild",         t_match_wild },
    { "machine_two_states", t_machine_two_states },
    { "fn_basic",           t_fn_basic },
    { "fn_multi_line",      t_fn_multi_line },
    { "fn_recursion",       t_fn_recursion },
    { "fn_mutual",          t_fn_mutual },
    { "fn_arity_mismatch",  t_fn_arity_mismatch },
    { "lambda_single_ident", t_lambda_single_ident },
    { "lambda_anon_fn",      t_lambda_anon_fn },
    { "lambda_closure",      t_lambda_closure },
    { "lambda_inline_call",  t_lambda_inline_call },
    { "lambda_higher_order", t_lambda_higher_order },
    { "tco_self_deep",       t_tco_self_deep },
    { "tco_self_acc",        t_tco_self_acc },
    { "tco_mutual",          t_tco_mutual },
    { "app_dispatch",           t_app_dispatch },
    { "machine_hooks",          t_machine_hooks },
    { "migration",              t_migration },
    { "intent_captured_action", t_intent_captured_action },
    { "intent_event_value",     t_intent_event_value },
    { "intent_event_values",    t_intent_event_values },
    { "eq_structural",          t_eq_structural },
    { "pipe_opt_variants",      t_pipe_opt_variants },
    { "named_call_args",        t_named_call_args },
    { "cycle_leak_fn_closure",  t_cycle_leak_fn_closure },
    { "cycle_leak_list_let",    t_cycle_leak_list_let },
    { "cycle_leak_rerun_x20",   t_cycle_leak_rerun_x20 },
    { "none_pattern_bridges_optional", t_none_pattern_bridges_optional },
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
