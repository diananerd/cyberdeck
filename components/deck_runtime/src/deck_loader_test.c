/* Loader self-tests: for each sample program, assert the loader
 * either succeeds with OK or fails with the expected structured
 * error code + stage.
 */

#include "deck_loader.h"
#include "deck_runtime.h"

#include "esp_log.h"
#include <string.h>

static const char *TAG = "deck_loader";

typedef struct {
    const char *name;
    const char *src;
    deck_err_t  expect_code;   /* DECK_LOAD_OK for the happy path */
    uint8_t     expect_stage;  /* ignored for OK */
} loader_case_t;

/* ---- valid DL1 apps ---- */
#define APP_HEADER_DL1 \
    "@app\n" \
    "  name: \"Hello\"\n" \
    "  id: \"sys.hello\"\n" \
    "  version: \"1.0.0\"\n" \
    "  edition: 2026\n" \
    "\n@requires\n" \
    "  deck_level: 1\n"

static const loader_case_t CASES[] = {

    /* --- happy path --- */
    { "ok_minimal", APP_HEADER_DL1, DECK_LOAD_OK, 0 },

    { "ok_with_on_launch",
      APP_HEADER_DL1
      "\n@on launch:\n"
      "  log.info(\"hi\")\n",
      DECK_LOAD_OK, 0 },

    { "ok_with_machine",
      APP_HEADER_DL1
      "\n@machine m\n"
      "  state a:\n"
      "    transition :b\n"
      "  state b:\n"
      "    on enter:\n"
      "      log.info(\"b\")\n",
      DECK_LOAD_OK, 0 },

    { "ok_match_wildcard",
      APP_HEADER_DL1
      "\n@on launch:\n"
      "  match 1\n"
      "    | 0 -> log.info(\"zero\")\n"
      "    | _ -> log.info(\"nonzero\")\n",
      DECK_LOAD_OK, 0 },

    { "ok_match_ident",
      APP_HEADER_DL1
      "\n@on launch:\n"
      "  match 1\n"
      "    | x -> log.info(\"bound\")\n",
      DECK_LOAD_OK, 0 },

    /* --- stage 1: parse errors --- */
    { "err_parse_bad",
      "@app\n  name \"X\"\n",
      DECK_LOAD_PARSE_ERROR, 1 },

    /* --- stage 3: missing metadata --- */
    { "err_no_app",
      "@on launch:\n"
      "  log.info(\"x\")\n",
      DECK_LOAD_TYPE_ERROR, 3 },

    { "err_missing_id",
      "@app\n"
      "  name: \"X\"\n"
      "  version: \"1.0.0\"\n"
      "  edition: 2026\n",
      DECK_LOAD_TYPE_ERROR, 3 },

    { "err_missing_edition",
      "@app\n"
      "  name: \"X\"\n"
      "  id: \"y\"\n"
      "  version: \"1.0.0\"\n",
      DECK_LOAD_TYPE_ERROR, 3 },

    /* --- stage 2: unresolved transition target --- */
    { "err_bad_transition",
      APP_HEADER_DL1
      "\n@machine m\n"
      "  state a:\n"
      "    transition :nowhere\n",
      DECK_LOAD_UNRESOLVED_SYMBOL, 2 },

    /* --- stage 4: unknown capability --- */
    { "err_unknown_cap",
      APP_HEADER_DL1
      "\n@on launch:\n"
      "  wifi.scan()\n",
      DECK_LOAD_CAPABILITY_MISSING, 4 },

    /* --- stage 5: non-exhaustive match --- */
    { "err_nonexhaustive",
      APP_HEADER_DL1
      "\n@on launch:\n"
      "  match 1\n"
      "    | 0 -> log.info(\"zero\")\n"
      "    | 1 -> log.info(\"one\")\n",
      DECK_LOAD_PATTERN_NOT_EXHAUSTIVE, 5 },

    /* --- stage 6: deck_level below required --- */
    { "err_level_below",
      "@app\n"
      "  name: \"Pro\"\n"
      "  id: \"x\"\n"
      "  version: \"1.0.0\"\n"
      "  edition: 2026\n"
      "\n@requires\n"
      "  deck_level: 3\n",
      DECK_LOAD_LEVEL_BELOW_REQUIRED, 6 },

    /* --- stage 6: unknown deck_level --- */
    { "err_level_unknown",
      "@app\n"
      "  name: \"X\"\n"
      "  id: \"y\"\n"
      "  version: \"1.0.0\"\n"
      "  edition: 2026\n"
      "\n@requires\n"
      "  deck_level: 99\n",
      DECK_LOAD_LEVEL_UNKNOWN, 6 },

    /* --- stage 6: incompatible edition --- */
    { "err_bad_edition",
      "@app\n"
      "  name: \"X\"\n"
      "  id: \"y\"\n"
      "  version: \"1.0.0\"\n"
      "  edition: 2099\n"
      "\n@requires\n"
      "  deck_level: 1\n",
      DECK_LOAD_INCOMPATIBLE_EDITION, 6 },

    /* --- stage 4: legal caps all green --- */
    { "ok_all_caps",
      APP_HEADER_DL1
      "\n@on launch:\n"
      "  log.info(\"x\")\n"
      "  nvs.set(\"k\", \"v\")\n"
      "  time.now()\n"
      "  system.info()\n"
      "  fs.read(\"/p\")\n",
      DECK_LOAD_OK, 0 },

    /* --- pattern exhaustive with multiple arms + wildcard --- */
    { "ok_match_three_arms",
      APP_HEADER_DL1
      "\n@on launch:\n"
      "  match 1\n"
      "    | 0 -> log.info(\"z\")\n"
      "    | 1 -> log.info(\"o\")\n"
      "    | _ -> log.info(\"x\")\n",
      DECK_LOAD_OK, 0 },

    /* --- dotted capability reaches through multiple dots --- */
    { "ok_system_info_deck_level",
      APP_HEADER_DL1
      "\n@on launch:\n"
      "  log.info(\"level\")\n",
      DECK_LOAD_OK, 0 },
};

#define N_CASES (sizeof(CASES) / sizeof(CASES[0]))

static bool run_case(const loader_case_t *c)
{
    deck_arena_t  arena;
    deck_loader_t ld;
    deck_arena_init(&arena, 0);
    deck_loader_init(&ld, &arena);

    deck_err_t rc = deck_loader_load(&ld, c->src, (uint32_t)strlen(c->src));

    bool ok = true;
    if (c->expect_code == DECK_LOAD_OK) {
        if (rc != DECK_LOAD_OK) {
            ESP_LOGE(TAG, "  [%s] expected OK, got %s @ stage %u: %s",
                     c->name, deck_err_name(rc), ld.err_stage, ld.err_msg);
            ok = false;
        }
    } else {
        if (rc != c->expect_code) {
            ESP_LOGE(TAG, "  [%s] expected %s @ stage %u, got %s @ stage %u: %s",
                     c->name, deck_err_name(c->expect_code), c->expect_stage,
                     deck_err_name(rc), ld.err_stage, ld.err_msg);
            ok = false;
        } else if (ld.err_stage != c->expect_stage) {
            ESP_LOGE(TAG, "  [%s] err code OK but stage %u (expected %u)",
                     c->name, ld.err_stage, c->expect_stage);
            ok = false;
        }
    }

    deck_arena_reset(&arena);
    return ok;
}

deck_err_t deck_loader_run_selftest(void)
{
    uint32_t pass = 0, fail = 0;
    for (size_t i = 0; i < N_CASES; i++) {
        if (run_case(&CASES[i])) pass++;
        else                     fail++;
    }
    if (fail) {
        ESP_LOGE(TAG, "loader selftest: %u/%u pass, %u fail",
                 (unsigned)pass, (unsigned)N_CASES, (unsigned)fail);
        return DECK_LOAD_INTERNAL;
    }
    ESP_LOGI(TAG, "loader selftest: PASS (%u cases)", (unsigned)N_CASES);
    return DECK_RT_OK;
}
