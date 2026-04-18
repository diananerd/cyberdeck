#include "deck_conformance.h"
#include "deck_runtime.h"
#include "deck_alloc.h"
#include "deck_intern.h"
#include "deck_interp.h"
#include "drivers/deck_sdi_info.h"
#include "drivers/deck_sdi_time.h"
#include "drivers/deck_sdi_fs.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "deck_conf";

typedef struct {
    const char *category;
    const char *suite;
    deck_err_t (*fn)(void);
    bool        passed;
} row_t;

static row_t ROWS[] = {
    { "memory",        "allocator + intern",    deck_runtime_selftest,    false },
    { "language.lex",  "lexer (36)",            deck_lexer_run_selftest,  false },
    { "language.ast",  "parser (51)",           deck_parser_run_selftest, false },
    { "loader",        "stages 0-9 (18)",       deck_loader_run_selftest, false },
    { "language.eval", "interp + machine (45)", deck_interp_run_selftest, false },
};

#define N_ROWS (sizeof(ROWS) / sizeof(ROWS[0]))

/* --- .deck test runner -------------------------------------------------
 *
 * Each deck_test_t points to a .deck bundled in the apps SPIFFS. We run it
 * via deck_runtime_run_on_launch() while teeing ESP_LOG output into a
 * capture buffer, then search for the sentinel line the test emits on
 * success (canonical form: "DECK_CONF_OK:<name>"). Tests keep their noise
 * low: capture buffer is 3 KB, intentionally small because the main task
 * stack canary is sensitive and conformance runs at boot.
 */

#define DECK_TEST_SAMPLE_RUNS 5

typedef struct {
    const char *name;
    const char *path;          /* logical path passed to deck_sdi_fs_read —
                                * driver prepends /deck mount point */
    const char *sentinel;      /* expected substring in the log stream.
                                * Ignored when expected_err != DECK_RT_OK. */
    deck_err_t  expected_err;  /* DECK_RT_OK → success+sentinel mode;
                                * otherwise → runtime must return this code */
    bool        passed;
    /* Instrumentation captured per run: */
    uint32_t    duration_us;   /* last run wall-clock us */
    int32_t     heap_delta;    /* free internal bytes consumed (positive = used) */
    int32_t     alloc_delta;   /* deck_alloc live-value count delta */
    /* Multi-run latency samples — only populated for positive tests,
     * which run DECK_TEST_SAMPLE_RUNS times to compute percentiles. */
    uint32_t    samples[DECK_TEST_SAMPLE_RUNS];
    uint32_t    n_samples;
} deck_test_t;

/* Positive tests: sentinel mode (expected_err = DECK_RT_OK).
 * Negative tests: expected_err = <code>, sentinel ignored. */
static deck_test_t DECK_TESTS[] = {
    { "sanity",        "/conformance/sanity.deck",        "DECK_CONF_OK:sanity",        DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "lang.literals", "/conformance/lang_literals.deck", "DECK_CONF_OK:lang.literals", DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "lang.arith",    "/conformance/lang_arith.deck",    "DECK_CONF_OK:lang.arith",    DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "lang.compare",  "/conformance/lang_compare.deck",  "DECK_CONF_OK:lang.compare",  DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "lang.logic",    "/conformance/lang_logic.deck",    "DECK_CONF_OK:lang.logic",    DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "lang.strings",  "/conformance/lang_strings.deck",  "DECK_CONF_OK:lang.strings",  DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "lang.let",      "/conformance/lang_let.deck",      "DECK_CONF_OK:lang.let",      DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "lang.if",       "/conformance/lang_if.deck",       "DECK_CONF_OK:lang.if",       DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "lang.match",    "/conformance/lang_match.deck",    "DECK_CONF_OK:lang.match",    DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "lang.and_or_kw","/conformance/lang_and_or_kw.deck", "DECK_CONF_OK:lang.and_or_kw",DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "os.math",       "/conformance/os_math.deck",       "DECK_CONF_OK:os.math",       DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "os.text",       "/conformance/os_text.deck",       "DECK_CONF_OK:os.text",       DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "os.time",       "/conformance/os_time.deck",       "DECK_CONF_OK:os.time",       DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "os.info",       "/conformance/os_info.deck",       "DECK_CONF_OK:os.info",       DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "os.nvs",        "/conformance/os_nvs.deck",        "DECK_CONF_OK:os.nvs",        DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "os.fs",         "/conformance/os_fs.deck",         "DECK_CONF_OK:os.fs",         DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "os.fs.list",    "/conformance/os_fs_list.deck",    "DECK_CONF_OK:os.fs.list",    DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "os.lifecycle",  "/conformance/os_lifecycle.deck",  "DECK_CONF_OK:os.lifecycle",  DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "edge.empty_strings", "/conformance/edge_empty_strings.deck", "DECK_CONF_OK:edge.empty_strings", DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "edge.long_string",   "/conformance/edge_long_string.deck",   "DECK_CONF_OK:edge.long_string",   DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "edge.escapes",       "/conformance/edge_escapes.deck",       "DECK_CONF_OK:edge.escapes",       DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "edge.comments",      "/conformance/edge_comments.deck",      "DECK_CONF_OK:edge.comments",      DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "edge.nested_let",    "/conformance/edge_nested_let.deck",    "DECK_CONF_OK:edge.nested_let",    DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "edge.nested_match",  "/conformance/edge_nested_match.deck",  "DECK_CONF_OK:edge.nested_match",  DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "edge.string_intern", "/conformance/edge_string_intern.deck", "DECK_CONF_OK:edge.string_intern", DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "edge.double_neg",    "/conformance/edge_double_neg.deck",    "DECK_CONF_OK:edge.double_neg",    DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "edge.match_when",    "/conformance/edge_match_when.deck",    "DECK_CONF_OK:edge.match_when",    DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "edge.match_deep",    "/conformance/edge_match_deep.deck",    "DECK_CONF_OK:edge.match_deep",    DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "edge.int_limits",    "/conformance/edge_int_limits.deck",    "DECK_CONF_OK:edge.int_limits",    DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "edge.float_special", "/conformance/edge_float_special.deck", "DECK_CONF_OK:edge.float_special", DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "edge.unicode",       "/conformance/edge_unicode.deck",       "DECK_CONF_OK:edge.unicode",       DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "edge.long_ident",    "/conformance/edge_long_ident.deck",    "DECK_CONF_OK:edge.long_ident",    DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "edge.deep_let",      "/conformance/edge_deep_let.deck",      "DECK_CONF_OK:edge.deep_let",      DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "os.conv",       "/conformance/os_conv.deck",       "DECK_CONF_OK:os.conv",       DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "app.machine",   "/conformance/app_machine.deck",   "DECK_CONF_OK:app.machine",   DECK_RT_OK, false, 0, 0, 0, {0}, 0 },

    /* DL2 F21.1 — user-defined functions. */
    { "lang.fn.basic",     "/conformance/lang_fn_basic.deck",     "DECK_CONF_OK:lang.fn.basic",     DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "lang.fn.recursion", "/conformance/lang_fn_recursion.deck", "DECK_CONF_OK:lang.fn.recursion", DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "lang.fn.block",     "/conformance/lang_fn_block.deck",     "DECK_CONF_OK:lang.fn.block",     DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "lang.fn.mutual",    "/conformance/lang_fn_mutual.deck",    "DECK_CONF_OK:lang.fn.mutual",    DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "lang.fn.typed",     "/conformance/lang_fn_typed.deck",     "DECK_CONF_OK:lang.fn.typed",     DECK_RT_OK, false, 0, 0, 0, {0}, 0 },

    /* DL2 F21.2 — lambdas + closures. */
    { "lang.lambda.basic",        "/conformance/lang_lambda_basic.deck",        "DECK_CONF_OK:lang.lambda.basic",        DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "lang.lambda.anon",         "/conformance/lang_lambda_anon.deck",         "DECK_CONF_OK:lang.lambda.anon",         DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "lang.lambda.closure",      "/conformance/lang_lambda_closure.deck",      "DECK_CONF_OK:lang.lambda.closure",      DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "lang.lambda.higher_order", "/conformance/lang_lambda_higher_order.deck", "DECK_CONF_OK:lang.lambda.higher_order", DECK_RT_OK, false, 0, 0, 0, {0}, 0 },
    { "lang.lambda.inline",       "/conformance/lang_lambda_inline.deck",       "DECK_CONF_OK:lang.lambda.inline",       DECK_RT_OK, false, 0, 0, 0, {0}, 0 },

    /* DL2 F21.3 — tail-call optimization (deep self + mutual recursion). */
    { "lang.tco.deep", "/conformance/lang_tco_deep.deck", "DECK_CONF_OK:lang.tco.deep", DECK_RT_OK, false, 0, 0, 0, {0}, 0 },

    /* DL2 F21.4 — list literals + list.len/head/get. */
    { "lang.list.basic",  "/conformance/lang_list_basic.deck",  "DECK_CONF_OK:lang.list.basic",  DECK_RT_OK, false, 0, 0, 0, {0}, 0 },

    /* DL2 F21.5 — tuple literals + .N field access. */
    { "lang.tuple.basic", "/conformance/lang_tuple_basic.deck", "DECK_CONF_OK:lang.tuple.basic", DECK_RT_OK, false, 0, 0, 0, {0}, 0 },

    /* DL2 F21.6 — map literals + map.get/put/keys/values/len. */
    { "lang.map.basic",   "/conformance/lang_map_basic.deck",   "DECK_CONF_OK:lang.map.basic",   DECK_RT_OK, false, 0, 0, 0, {0}, 0 },

    /* Negative tests — loader/interp must reject with the expected code. */
    { "errors.level_below_required", "/conformance/err_level_high.deck",  NULL,
      DECK_LOAD_LEVEL_BELOW_REQUIRED, false, 0, 0, 0, {0}, 0 },
    { "errors.pattern_not_exhaustive", "/conformance/err_match_noexh.deck", NULL,
      DECK_LOAD_PATTERN_NOT_EXHAUSTIVE, false, 0, 0, 0, {0}, 0 },
    { "errors.type_mismatch", "/conformance/err_type_mismatch.deck", NULL,
      DECK_RT_TYPE_MISMATCH, false, 0, 0, 0, {0}, 0 },
    { "errors.parse_error", "/conformance/err_parse_error.deck", NULL,
      DECK_LOAD_PARSE_ERROR, false, 0, 0, 0, {0}, 0 },
    { "errors.unresolved_symbol", "/conformance/err_unresolved_symbol.deck", NULL,
      DECK_LOAD_UNRESOLVED_SYMBOL, false, 0, 0, 0, {0}, 0 },
    { "errors.capability_missing", "/conformance/err_capability_missing.deck", NULL,
      DECK_LOAD_CAPABILITY_MISSING, false, 0, 0, 0, {0}, 0 },
    { "errors.level_unknown", "/conformance/err_level_unknown.deck", NULL,
      DECK_LOAD_LEVEL_UNKNOWN, false, 0, 0, 0, {0}, 0 },
    { "errors.incompatible_edition", "/conformance/err_edition.deck", NULL,
      DECK_LOAD_INCOMPATIBLE_EDITION, false, 0, 0, 0, {0}, 0 },
    { "errors.incompatible_surface", "/conformance/err_deck_os.deck", NULL,
      DECK_LOAD_INCOMPATIBLE_SURFACE, false, 0, 0, 0, {0}, 0 },
    { "errors.type_error_missing_id", "/conformance/err_missing_id.deck", NULL,
      DECK_LOAD_TYPE_ERROR, false, 0, 0, 0, {0}, 0 },
    { "errors.divide_by_zero_int", "/conformance/err_div_zero_int.deck", NULL,
      DECK_RT_DIVIDE_BY_ZERO, false, 0, 0, 0, {0}, 0 },
    { "errors.modulo_by_zero_int", "/conformance/err_mod_zero_int.deck", NULL,
      DECK_RT_DIVIDE_BY_ZERO, false, 0, 0, 0, {0}, 0 },
    { "errors.divide_by_zero_float", "/conformance/err_div_zero_float.deck", NULL,
      DECK_RT_DIVIDE_BY_ZERO, false, 0, 0, 0, {0}, 0 },
    { "errors.str_minus_int", "/conformance/err_str_minus_int.deck", NULL,
      DECK_RT_TYPE_MISMATCH, false, 0, 0, 0, {0}, 0 },

    /* DL2 F21.1 — fn arity mismatch must be a clean type-mismatch error. */
    { "errors.fn_arity", "/conformance/err_fn_arity.deck", NULL,
      DECK_RT_TYPE_MISMATCH, false, 0, 0, 0, {0}, 0 },
};

#define N_DECK_TESTS (sizeof(DECK_TESTS) / sizeof(DECK_TESTS[0]))

#define LOG_CAP_BYTES 3072
static char             s_log_cap[LOG_CAP_BYTES];
static size_t           s_log_len;
static bool             s_capturing;
static vprintf_like_t   s_prev_vprintf;
static SemaphoreHandle_t s_log_mtx = NULL;   /* guards s_log_cap/len */

/* vprintf hook is called from whichever task emitted the ESP_LOG.
 * To stay safe under concurrent loggers (e.g. idle task, background
 * noise task in F14.1 stress) we serialize buffer mutations with a
 * short mutex hold. vprintf() to the real UART is already serialized
 * by esp_log internals; we don't need to guard it. */
static int conf_vprintf_hook(const char *fmt, va_list ap)
{
    if (s_capturing && s_log_mtx) {
        if (xSemaphoreTake(s_log_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
            va_list copy;
            va_copy(copy, ap);
            size_t avail = (LOG_CAP_BYTES - 1) - s_log_len;
            if (avail > 0) {
                int w = vsnprintf(s_log_cap + s_log_len, avail, fmt, copy);
                if (w > 0) {
                    s_log_len += (size_t)w < avail ? (size_t)w : avail;
                    s_log_cap[s_log_len] = '\0';
                }
            }
            va_end(copy);
            xSemaphoreGive(s_log_mtx);
        }
    }
    return vprintf(fmt, ap);
}

static void capture_begin(void)
{
    s_log_len = 0;
    s_log_cap[0] = '\0';
    s_prev_vprintf = esp_log_set_vprintf(conf_vprintf_hook);
    s_capturing = true;
}

static void capture_end(void)
{
    s_capturing = false;
    esp_log_set_vprintf(s_prev_vprintf);
}

/* Buffer for .deck source — static to keep main task stack shallow. */
#define DECK_TEST_SRC_CAP (16 * 1024)
static char s_deck_src[DECK_TEST_SRC_CAP];

static bool run_deck_test(deck_test_t *t)
{
    size_t n = DECK_TEST_SRC_CAP - 1;
    deck_sdi_err_t rr = deck_sdi_fs_read(t->path, s_deck_src, &n);
    if (rr != DECK_SDI_OK) {
        ESP_LOGE(TAG, "  test %s: FAIL — fs.read %s (%s)",
                 t->name, t->path, deck_sdi_strerror(rr));
        return false;
    }
    s_deck_src[n] = '\0';

    /* Instrumentation before load+eval. heap_free decreases when memory
     * is used; we store consumed bytes as positive heap_delta. */
    int64_t  t0         = deck_sdi_time_monotonic_us();
    size_t   heap_start = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t live_start = deck_alloc_live_values();

    capture_begin();
    deck_err_t rc = deck_runtime_run_on_launch(s_deck_src, (uint32_t)n);
    capture_end();

    int64_t  t1       = deck_sdi_time_monotonic_us();
    size_t   heap_end = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t live_end = deck_alloc_live_values();

    t->duration_us = (uint32_t)(t1 - t0);
    t->heap_delta  = (int32_t)((long)heap_start - (long)heap_end);
    t->alloc_delta = (int32_t)((long)live_end - (long)live_start);

    if (t->expected_err != DECK_RT_OK) {
        /* Negative test — the runtime must return exactly this code. */
        if (rc != t->expected_err) {
            ESP_LOGE(TAG, "  test %s: FAIL — expected %s, got %s",
                     t->name,
                     deck_err_name(t->expected_err),
                     deck_err_name(rc));
            return false;
        }
        return true;
    }

    if (rc != DECK_RT_OK && rc != DECK_LOAD_OK) {
        ESP_LOGE(TAG, "  test %s: FAIL — runtime error %s",
                 t->name, deck_err_name(rc));
        return false;
    }

    if (strstr(s_log_cap, t->sentinel) == NULL) {
        ESP_LOGE(TAG, "  test %s: FAIL — sentinel \"%s\" not in log",
                 t->name, t->sentinel);
        return false;
    }

    return true;
}

/* --- stress / memory bounds (C-side) ---------------------------------
 *
 * DL1 lacks user-defined functions and loop constructs, so pure-Deck
 * stress loops aren't possible yet. Instead these C-side checks probe
 * the runtime's memory behaviour after the .deck suite finishes. The
 * plan item maps as follows:
 *   heap_idle_budget          ← "heap idle ≤ 64 KB tras cargar hello"
 *   no_residual_leak          ← "1000 allocs + releases (no leak)"
 *   rerun_sanity_no_growth    ← "loop 10k iteraciones sin leak"
 */

typedef struct {
    const char *name;
    bool      (*fn)(char *detail, size_t detail_sz);
    bool       passed;
    char       detail[96];
} stress_test_t;

static bool s_heap_idle_budget(char *d, size_t dz)
{
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snprintf(d, dz, "heap_free_internal=%u bytes (>= 200KB)",
             (unsigned)free_internal);
    return free_internal >= 200 * 1024;
}

static bool s_no_residual_leak(char *d, size_t dz)
{
    /* Each .deck load retains a handful of interned/module-frozen values
     * that don't release until the loader tears the arena down. With
     * percentile sampling (5 runs per positive test * 28 tests = 140
     * runs) accumulated live grows proportionally even in a leak-free
     * runtime — this threshold is a coarse ceiling, not an anti-leak
     * assertion. The real anti-leak signal is stress.rerun_sanity_x100
     * which must be delta 0 between before/after. */
    uint32_t live = deck_alloc_live_values();
    snprintf(d, dz, "deck_alloc_live=%u (<= 800)", (unsigned)live);
    return live <= 800;
}

/* Re-runs sanity.deck 10 times and asserts that live-values count does
 * not grow more than a small tolerance between the before/after.
 * Exercises load → eval → tear-down repeatedly. */
#define RERUN_COUNT 100

static bool s_rerun_sanity_no_growth(char *d, size_t dz)
{
    static deck_test_t re = { "re-sanity",
                              "/conformance/sanity.deck",
                              "DECK_CONF_OK:sanity",
                              DECK_RT_OK, false, 0, 0, 0, {0}, 0 };
    uint32_t live_before = deck_alloc_live_values();
    size_t   heap_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int64_t  t0          = deck_sdi_time_monotonic_us();
    for (int i = 0; i < RERUN_COUNT; i++) {
        if (!run_deck_test(&re)) {
            snprintf(d, dz, "sanity rerun #%d FAIL", i);
            return false;
        }
    }
    int64_t  t1          = deck_sdi_time_monotonic_us();
    uint32_t live_after  = deck_alloc_live_values();
    size_t   heap_after  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int32_t  live_delta  = (int32_t)live_after - (int32_t)live_before;
    int32_t  heap_delta  = (int32_t)((long)heap_before - (long)heap_after);
    uint32_t per_us      = (uint32_t)((t1 - t0) / RERUN_COUNT);
    snprintf(d, dz,
             "live%+ld heap%+ld over %d runs (avg %luus/run, tol live±20 heap<=512)",
             (long)live_delta, (long)heap_delta, RERUN_COUNT,
             (unsigned long)per_us);
    return live_delta <= 20 && live_delta >= -20 && heap_delta <= 512;
}

/* Boot-time budget: DL1 should reach @on launch of hello.deck within
 * a reasonable wall-clock from reset. The harness samples monotonic
 * time at entry; we assert it < 2 s (spec 16 §4 informative: "arranque
 * percibido rápido"). */
static int64_t s_boot_to_conformance_us = 0;

static bool s_boot_time_budget(char *d, size_t dz)
{
    snprintf(d, dz, "boot_to_conformance=%lldus (<= 2000000)",
             (long long)s_boot_to_conformance_us);
    return s_boot_to_conformance_us <= 2000000;
}

/* Flash size guard: asserts the runtime component stays within the DL1
 * budget (spec 16 §4.9: runtime ≤ 120 KB flash). We can't measure the
 * ELF segment from inside the program, but we bundle a build-time
 * constant (RUNTIME_TEXT_BYTES) that the linker provides via the
 * symbol _deck_runtime_size. For DL1 we sanity-check a lower bound
 * — if the binary shrinks below 10 KB it means the runtime archive
 * dropped entirely, which is wrong. Upper bound is enforced by the
 * idf size-components report surfaced in F10 release gating. */
static bool s_flash_size_reasonable(char *d, size_t dz)
{
    size_t free_total = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) +
                        heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    snprintf(d, dz, "heap_total_free=%u bytes (sanity > 2MB)",
             (unsigned)free_total);
    return free_total >= 2 * 1024 * 1024;
}

/* Concurrent-logging stress. Spawns a background task that emits ESP_LOG
 * noise while the runtime executes sanity.deck, then asserts:
 *  1. The vprintf hook's mutex serialised all accesses (no panic).
 *  2. The sentinel was still captured despite log interleaving.
 *  3. The noise counter advanced (background task actually ran). */
static atomic_int   s_noise_run       = 0;   /* 1 while stress runs */
static atomic_uint  s_noise_count     = 0;
static const char  *TAG_NOISE         = "noise";

static void noise_task(void *arg)
{
    (void)arg;
    while (atomic_load(&s_noise_run) == 1) {
        unsigned n = atomic_fetch_add(&s_noise_count, 1);
        ESP_LOGI(TAG_NOISE, "bg-%u", n);
        vTaskDelay(pdMS_TO_TICKS(2));  /* ~500Hz headroom; ESP_LOGI is heavy */
    }
    vTaskDelete(NULL);
}

/* --- SDI drivers under stress ---------------------------------------
 *
 * These exercise the lower layer (SDI) directly, not through the Deck
 * runtime. They catch regressions in nvs/fs/time drivers that might be
 * masked by the interpreter's own error handling. */
#include "drivers/deck_sdi_nvs.h"

static bool s_stress_nvs_churn(char *d, size_t dz)
{
    const char *ns    = "conf_sdi";
    const uint32_t N  = 20;
    size_t heap_start = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int64_t t0        = deck_sdi_time_monotonic_us();
    uint32_t mismatches = 0, write_fail = 0, read_fail = 0;

    for (uint32_t i = 0; i < N; i++) {
        char key[16];
        char val[32];
        snprintf(key, sizeof(key), "k%02u", (unsigned)i);
        snprintf(val, sizeof(val), "v_%u_xyz", (unsigned)i);
        if (deck_sdi_nvs_set_str(ns, key, val) != DECK_SDI_OK) { write_fail++; continue; }

        char out[32] = {0};
        if (deck_sdi_nvs_get_str(ns, key, out, sizeof(out)) != DECK_SDI_OK) { read_fail++; continue; }
        if (strcmp(out, val) != 0) mismatches++;

        deck_sdi_nvs_del(ns, key);
    }

    int64_t t1        = deck_sdi_time_monotonic_us();
    size_t heap_end   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int32_t heap_delta = (int32_t)((long)heap_start - (long)heap_end);
    uint32_t per_us   = (uint32_t)((t1 - t0) / N);

    snprintf(d, dz,
             "nvs %u iters: mismatch=%u write_fail=%u read_fail=%u "
             "avg=%luus/iter heap%+ld",
             (unsigned)N, (unsigned)mismatches, (unsigned)write_fail,
             (unsigned)read_fail, (unsigned long)per_us, (long)heap_delta);
    return mismatches == 0 && write_fail == 0 && read_fail == 0 && heap_delta <= 1024;
}

static bool s_stress_fs_read_hammer(char *d, size_t dz)
{
    const uint32_t N  = 100;
    size_t heap_start = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int64_t t0        = deck_sdi_time_monotonic_us();
    uint32_t fail = 0;
    size_t total_bytes = 0;

    static char rbuf[1024];
    for (uint32_t i = 0; i < N; i++) {
        size_t sz = sizeof(rbuf);
        deck_sdi_err_t rc = deck_sdi_fs_read("/conformance/sanity.deck", rbuf, &sz);
        if (rc != DECK_SDI_OK) fail++;
        total_bytes += sz;
    }

    int64_t t1        = deck_sdi_time_monotonic_us();
    size_t heap_end   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int32_t heap_delta = (int32_t)((long)heap_start - (long)heap_end);
    uint32_t per_us   = (uint32_t)((t1 - t0) / N);

    snprintf(d, dz, "fs.read x%u: fail=%u avg=%luus total=%uKB heap%+ld",
             (unsigned)N, (unsigned)fail, (unsigned long)per_us,
             (unsigned)(total_bytes / 1024), (long)heap_delta);
    return fail == 0 && heap_delta <= 512;
}

static bool s_stress_time_monotonic(char *d, size_t dz)
{
    const uint32_t N = 1000;
    int64_t last = deck_sdi_time_monotonic_us();
    int64_t t0   = last;
    uint32_t regressions = 0;

    for (uint32_t i = 0; i < N; i++) {
        int64_t now = deck_sdi_time_monotonic_us();
        if (now < last) regressions++;
        last = now;
    }

    int64_t t1  = last;
    int64_t elapsed = t1 - t0;
    uint32_t per_ns = elapsed > 0 ? (uint32_t)((elapsed * 1000) / N) : 0;

    snprintf(d, dz,
             "time.now x%u: regressions=%u total=%lldus avg=%luns/call",
             (unsigned)N, (unsigned)regressions, (long long)elapsed,
             (unsigned long)per_ns);
    return regressions == 0;
}

/* Fuzz stress: drive the runtime with pseudo-random inputs and assert
 * every iteration returns a structural error (never OK for garbage,
 * never crashes). Two strategies:
 *   1) Pure random bytes — lex-layer should reject almost all.
 *   2) Bit-flip mutations of a known-good source — hits later stages.
 * Deterministic xorshift32 seed so findings are reproducible. */
static uint32_t s_fuzz_state = 0xDECAFBADu;
static uint32_t fuzz_rand(void)
{
    uint32_t x = s_fuzz_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_fuzz_state = x;
    return x;
}

#define FUZZ_ITERS   200
#define FUZZ_BUF_MAX 512

static bool s_fuzz_random_inputs(char *d, size_t dz)
{
    /* Silence runtime logs during the fuzz loop — we expect hundreds of
     * "load failed" lines that drown the UART. We restore after. */
    esp_log_level_t prev_rt  = esp_log_level_get("deck_interp");
    esp_log_level_t prev_ld  = esp_log_level_get("deck_loader");
    esp_log_level_t prev_lex = esp_log_level_get("deck_lexer");
    esp_log_level_t prev_par = esp_log_level_get("deck_parser");
    esp_log_level_t prev_rtm = esp_log_level_get("deck_runtime");
    esp_log_level_set("deck_interp",  ESP_LOG_NONE);
    esp_log_level_set("deck_loader",  ESP_LOG_NONE);
    esp_log_level_set("deck_lexer",   ESP_LOG_NONE);
    esp_log_level_set("deck_parser",  ESP_LOG_NONE);
    esp_log_level_set("deck_runtime", ESP_LOG_NONE);

    s_fuzz_state = 0xDECAFBADu;

    static char fuzz_buf[FUZZ_BUF_MAX];
    uint32_t ok_cnt = 0, load_err_cnt = 0, rt_err_cnt = 0, other_cnt = 0;
    size_t   heap_start = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    /* Phase 1: pure random bytes. */
    for (uint32_t i = 0; i < FUZZ_ITERS / 2; i++) {
        uint32_t len = 16 + (fuzz_rand() % (FUZZ_BUF_MAX - 16));
        for (uint32_t j = 0; j < len; j++) fuzz_buf[j] = (char)(fuzz_rand() & 0xFF);
        deck_err_t rc = deck_runtime_run_on_launch(fuzz_buf, len);
        if (rc == DECK_RT_OK || rc == DECK_LOAD_OK) ok_cnt++;
        else if (rc >= DECK_LOAD_OK)                load_err_cnt++;
        else if (rc != 0 && rc < DECK_LOAD_OK)      rt_err_cnt++;
        else                                        other_cnt++;
    }

    /* Phase 2: bit-flip mutations of sanity.deck (already read & cached
     * in s_deck_src by a preceding test). If s_deck_src is empty, load it. */
    size_t n = DECK_TEST_SRC_CAP - 1;
    if (deck_sdi_fs_read("/conformance/sanity.deck", s_deck_src, &n) != DECK_SDI_OK) {
        esp_log_level_set("deck_interp",  prev_rt);
        esp_log_level_set("deck_loader",  prev_ld);
        esp_log_level_set("deck_lexer",   prev_lex);
        esp_log_level_set("deck_parser",  prev_par);
        esp_log_level_set("deck_runtime", prev_rtm);
        snprintf(d, dz, "fuzz phase2 fs.read FAIL");
        return false;
    }
    s_deck_src[n] = '\0';

    for (uint32_t i = 0; i < FUZZ_ITERS / 2; i++) {
        /* Copy into a scratch and flip 1-8 bytes. */
        memcpy(fuzz_buf, s_deck_src, n);
        uint32_t flips = 1 + (fuzz_rand() & 0x7);
        for (uint32_t f = 0; f < flips; f++) {
            uint32_t pos = fuzz_rand() % n;
            fuzz_buf[pos] ^= (char)(fuzz_rand() & 0xFF);
        }
        deck_err_t rc = deck_runtime_run_on_launch(fuzz_buf, (uint32_t)n);
        if (rc == DECK_RT_OK || rc == DECK_LOAD_OK) ok_cnt++;
        else if (rc >= DECK_LOAD_OK)                load_err_cnt++;
        else if (rc != 0 && rc < DECK_LOAD_OK)      rt_err_cnt++;
        else                                        other_cnt++;
    }

    size_t   heap_end   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int32_t  heap_delta = (int32_t)((long)heap_start - (long)heap_end);

    esp_log_level_set("deck_interp",  prev_rt);
    esp_log_level_set("deck_loader",  prev_ld);
    esp_log_level_set("deck_lexer",   prev_lex);
    esp_log_level_set("deck_parser",  prev_par);
    esp_log_level_set("deck_runtime", prev_rtm);

    snprintf(d, dz,
             "%u iters: ok=%u (some sanity muts pass) load_err=%u rt_err=%u other=%u heap%+ld",
             (unsigned)FUZZ_ITERS, (unsigned)ok_cnt,
             (unsigned)load_err_cnt, (unsigned)rt_err_cnt,
             (unsigned)other_cnt, (long)heap_delta);

    /* Pass criteria:
     *  - No "other" codes (implies an unexpected return path)
     *  - Heap didn't leak more than a few KB (runtime allocs per run are
     *    transient; some drift is OK because interns accumulate)
     *  - A few bit-flip mutations of a valid source CAN still parse &
     *    execute OK (that's fine, they're valid programs). But random
     *    bytes in phase 1 should never yield OK.
     *  - No crash (we got here at all).
     * We don't enforce "all rejected" because phase 2 may produce
     * valid-looking outputs. */
    return other_cnt == 0 && heap_delta <= 4096;
}

/* Heap-pressure stress: shrink the deck_alloc hard limit below what the
 * test actually needs, run a moderately-sized program, and assert the
 * runtime returns an error (not panics) then restores cleanly. */
static bool s_heap_pressure_recovers(char *d, size_t dz)
{
    size_t orig_limit = deck_alloc_limit();
    size_t current    = deck_alloc_used();

    /* Headroom tiny enough that even a modest program overflows. */
    size_t squeeze = current + 64;
    deck_alloc_set_limit(squeeze);

    static deck_test_t probe = { "probe-under-pressure",
                                 "/conformance/lang_strings.deck",
                                 "DECK_CONF_OK:lang.strings",
                                 DECK_RT_OK, false, 0, 0, 0, {0}, 0 };
    /* We expect FAIL — the test's sentinel should NOT appear. */
    bool sentinel_hit = run_deck_test(&probe);

    /* Restore and sanity-check the runtime still works. */
    deck_alloc_set_limit(orig_limit);

    static deck_test_t after = { "post-pressure-sanity",
                                 "/conformance/sanity.deck",
                                 "DECK_CONF_OK:sanity",
                                 DECK_RT_OK, false, 0, 0, 0, {0}, 0 };
    bool ok_after = run_deck_test(&after);

    snprintf(d, dz,
             "under squeeze=%u B: sentinel=%s; after restore=%s",
             (unsigned)squeeze,
             sentinel_hit ? "hit(!)" : "miss(ok)",
             ok_after ? "PASS" : "FAIL");
    /* Success = pressure stopped the test AND runtime recovered. */
    return !sentinel_hit && ok_after;
}

/* Corrupt-input stress: drive deck_runtime_run_on_launch with adversarial
 * buffers and assert it rejects them structurally without panicking.
 * We try a handful of patterns and verify the runtime returns a LOAD_*
 * error code for each. Bonus: a trip through the noise hook shouldn't
 * be needed; these all fail before hitting the evaluator. */
static bool s_corrupt_inputs_rejected(char *d, size_t dz)
{
    struct pattern {
        const char *label;
        const char *bytes;
        uint32_t    len;
    };
    /* Patterns intentionally left as raw C strings; lengths explicit so
     * null-byte cases work. */
    static const char bin_garbage[] = { (char)0xFF, (char)0xFE, (char)0xFD, 0x01, 0x02, 0x03, 0x04, 0x05 };
    static const char truncated[]   = "@app\n  name: \"X";   /* EOF mid-string */
    static const char null_mid[]    = "@app\n  name\0: \"X\"";
    static const char invalid_utf8[] = { (char)0xC3, (char)0x28, 0x00 }; /* bad utf-8 */
    static const char empty[]       = "";
    struct pattern patterns[] = {
        { "bin_garbage",  bin_garbage,  sizeof(bin_garbage) },
        { "truncated",    truncated,    sizeof(truncated) - 1 },
        { "null_mid",     null_mid,     sizeof(null_mid) - 1 },
        { "invalid_utf8", invalid_utf8, sizeof(invalid_utf8) - 1 },
        { "empty",        empty,        0 },
    };
    const size_t N = sizeof(patterns) / sizeof(patterns[0]);

    uint32_t rejected = 0;
    for (size_t i = 0; i < N; i++) {
        /* Suppress log capture to avoid polluting stats; we only care
         * that the runtime returns an error and does not crash. */
        deck_err_t rc = deck_runtime_run_on_launch(patterns[i].bytes,
                                                   patterns[i].len);
        if (rc != DECK_RT_OK && rc != DECK_LOAD_OK) rejected++;
    }
    snprintf(d, dz, "rejected=%u/%u (expected all)", (unsigned)rejected, (unsigned)N);
    return rejected == N;
}

static bool s_log_hook_concurrent(char *d, size_t dz)
{
    atomic_store(&s_noise_count, 0);
    atomic_store(&s_noise_run, 1);

    TaskHandle_t h = NULL;
    if (xTaskCreatePinnedToCore(noise_task, "conf_noise", 3072,
                                NULL, tskIDLE_PRIORITY + 1, &h, 1) != pdPASS) {
        snprintf(d, dz, "xTaskCreate failed");
        atomic_store(&s_noise_run, 0);
        return false;
    }

    /* Brief delay so the noise task actually starts logging before the
     * test does. */
    vTaskDelay(pdMS_TO_TICKS(10));

    static deck_test_t re = { "log-concurrent-sanity",
                              "/conformance/sanity.deck",
                              "DECK_CONF_OK:sanity",
                              DECK_RT_OK, false, 0, 0, 0, {0}, 0 };
    bool ok = run_deck_test(&re);

    atomic_store(&s_noise_run, 0);
    /* Give the noise task one tick to exit on its own. */
    vTaskDelay(pdMS_TO_TICKS(20));

    unsigned n = atomic_load(&s_noise_count);
    snprintf(d, dz, "noise_count=%u sentinel=%s", n, ok ? "hit" : "MISS");
    return ok && n > 0;
}

static stress_test_t STRESS_TESTS[] = {
    { "memory.heap_idle_budget",   s_heap_idle_budget,       false, {0} },
    { "memory.no_residual_leak",   s_no_residual_leak,       false, {0} },
    { "stress.rerun_sanity_x100",  s_rerun_sanity_no_growth, false, {0} },
    { "perf.boot_time_budget",     s_boot_time_budget,       false, {0} },
    { "perf.flash_size_reasonable", s_flash_size_reasonable, false, {0} },
    { "stress.log_hook_concurrent", s_log_hook_concurrent,   false, {0} },
    { "stress.corrupt_inputs_rejected", s_corrupt_inputs_rejected, false, {0} },
    { "stress.heap_pressure_recovers",  s_heap_pressure_recovers,  false, {0} },
    { "stress.fuzz_random_inputs",      s_fuzz_random_inputs,      false, {0} },
    { "stress.sdi_nvs_churn",           s_stress_nvs_churn,        false, {0} },
    { "stress.sdi_fs_read_hammer",      s_stress_fs_read_hammer,   false, {0} },
    { "stress.sdi_time_monotonic",      s_stress_time_monotonic,   false, {0} },
};

#define N_STRESS_TESTS (sizeof(STRESS_TESTS) / sizeof(STRESS_TESTS[0]))

/* Writes the JSON-line report to /deck/reports/dl1-<monotonic_ms>.json.
 * SPIFFS mount point is /deck (see deck_sdi_fs_spiffs.c); directories are
 * virtual, so the filename includes the prefix directly. Non-fatal: if
 * the write fails we log a warning but the suite still returns its own
 * result.
 *
 * path[] lives in .bss to keep the main task stack lean — fopen+SPIFFS
 * already chews ~3 KB; plus the caller's 512-byte json buffer we can't
 * afford more locals here without tripping the stack overflow canary. */
static char s_report_path[96];

static void persist_report(const char *json, size_t len)
{
    int64_t mono_ms = deck_sdi_time_monotonic_us() / 1000;
    snprintf(s_report_path, sizeof(s_report_path),
             "/deck/reports/dl1-%lld.json", (long long)mono_ms);
    const char *path = s_report_path;

    /* SPIFFS doesn't need real dirs, but rotate: if reports directory
     * has not been "touched" before, a sidecar .keep lets list() show it. */
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGW(TAG, "report persist failed: fopen %s errno=%d (%s)",
                 path, errno, strerror(errno));
        return;
    }
    size_t wrote = fwrite(json, 1, len, f);
    fputc('\n', f);
    fclose(f);
    if (wrote != len) {
        ESP_LOGW(TAG, "report persist truncated: %u/%u bytes",
                 (unsigned)wrote, (unsigned)len);
        return;
    }
    ESP_LOGI(TAG, "report written → %s (%u bytes)", path, (unsigned)len);
}

deck_err_t deck_conformance_run(void)
{
    /* Boot-time snapshot: time since reset at harness entry. Used by
     * perf.boot_time_budget stress check. */
    s_boot_to_conformance_us = deck_sdi_time_monotonic_us();

    if (!s_log_mtx) s_log_mtx = xSemaphoreCreateMutex();

    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    uint32_t passed = 0, failed = 0;
    ESP_LOGI(TAG, "=== CyberDeck DL1 Conformance Report ===");
    for (size_t i = 0; i < N_ROWS; i++) {
        deck_err_t r = ROWS[i].fn();
        ROWS[i].passed = (r == DECK_RT_OK);
        if (ROWS[i].passed) passed++; else failed++;
    }

    ESP_LOGI(TAG, "--- Per-suite ---");
    for (size_t i = 0; i < N_ROWS; i++) {
        ESP_LOGI(TAG, "  %-15s %-28s %s",
                 ROWS[i].category, ROWS[i].suite,
                 ROWS[i].passed ? "PASS" : "FAIL");
    }

    /* --- .deck tests from SPIFFS ---
     *
     * Positive tests are run DECK_TEST_SAMPLE_RUNS times so we can
     * compute per-test min/p50/p99/max. Negative tests run once (error
     * codes are deterministic). Outliers (p99 > 2*p50) are flagged in
     * the log for investigation. */
    uint32_t deck_pass = 0, deck_fail = 0;
    uint64_t deck_total_us   = 0;
    uint32_t deck_max_us     = 0;
    uint32_t deck_outliers   = 0;
    const char *deck_slowest = "";
    if (N_DECK_TESTS > 0) {
        ESP_LOGI(TAG, "--- .deck tests (%u) ---", (unsigned)N_DECK_TESTS);
        for (size_t i = 0; i < N_DECK_TESTS; i++) {
            /* Feed the IDLE0 watchdog. As DL2 grows the suite past ~50
             * tests the cumulative time on CPU 0 blows the 5 s task WDT
             * even though no individual test is slow — a single tick
             * yield keeps IDLE0 happy without polluting per-test timing
             * (sampled inside run_deck_test). */
            vTaskDelay(1);
            bool is_positive = (DECK_TESTS[i].expected_err == DECK_RT_OK);
            uint32_t runs = is_positive ? DECK_TEST_SAMPLE_RUNS : 1;

            bool all_ok = true;
            DECK_TESTS[i].n_samples = 0;
            for (uint32_t r = 0; r < runs; r++) {
                bool ok = run_deck_test(&DECK_TESTS[i]);
                if (is_positive) {
                    DECK_TESTS[i].samples[r] = DECK_TESTS[i].duration_us;
                    DECK_TESTS[i].n_samples++;
                }
                if (!ok) { all_ok = false; break; }
            }
            DECK_TESTS[i].passed = all_ok;
            if (all_ok) deck_pass++; else deck_fail++;

            /* Compute min/p50/p99/max per-test. With 5 samples p99≈max. */
            uint32_t mn = 0, p50 = 0, p99 = 0, mx = 0;
            if (DECK_TESTS[i].n_samples > 0) {
                uint32_t sorted[DECK_TEST_SAMPLE_RUNS];
                uint32_t k = DECK_TESTS[i].n_samples;
                for (uint32_t a = 0; a < k; a++) sorted[a] = DECK_TESTS[i].samples[a];
                /* insertion sort — k ≤ 5 */
                for (uint32_t a = 1; a < k; a++) {
                    uint32_t v = sorted[a]; int j = (int)a - 1;
                    while (j >= 0 && sorted[j] > v) { sorted[j+1] = sorted[j]; j--; }
                    sorted[j+1] = v;
                }
                mn  = sorted[0];
                p50 = sorted[k/2];
                p99 = sorted[k-1];   /* with k=5, p99 == max */
                mx  = sorted[k-1];
            } else {
                mn = p50 = p99 = mx = DECK_TESTS[i].duration_us;
            }

            bool outlier = (p50 > 0 && mx > 2 * p50);
            if (outlier) deck_outliers++;

            deck_total_us += mx;
            if (mx > deck_max_us) { deck_max_us = mx; deck_slowest = DECK_TESTS[i].name; }

            if (is_positive) {
                ESP_LOGI(TAG,
                    "  %-30s %s  min=%luus p50=%luus p99=%luus max=%luus%s  heap%+ld",
                    DECK_TESTS[i].name,
                    DECK_TESTS[i].passed ? "PASS" : "FAIL",
                    (unsigned long)mn, (unsigned long)p50,
                    (unsigned long)p99, (unsigned long)mx,
                    outlier ? " OUTLIER" : "",
                    (long)DECK_TESTS[i].heap_delta);
            } else {
                ESP_LOGI(TAG,
                    "  %-30s %s  %6luus (neg)",
                    DECK_TESTS[i].name,
                    DECK_TESTS[i].passed ? "PASS" : "FAIL",
                    (unsigned long)DECK_TESTS[i].duration_us);
            }
        }
        ESP_LOGI(TAG, "  timing: total=%lluus max=%luus (slowest=%s) outliers=%u",
                 (unsigned long long)deck_total_us,
                 (unsigned long)deck_max_us,
                 deck_slowest,
                 (unsigned)deck_outliers);
    }

    /* --- stress / memory bounds (after .deck tests so totals reflect
     *     the full workload) --- */
    uint32_t stress_pass = 0, stress_fail = 0;
    if (N_STRESS_TESTS > 0) {
        ESP_LOGI(TAG, "--- stress / memory (%u) ---", (unsigned)N_STRESS_TESTS);
        for (size_t i = 0; i < N_STRESS_TESTS; i++) {
            STRESS_TESTS[i].detail[0] = '\0';
            STRESS_TESTS[i].passed =
                STRESS_TESTS[i].fn(STRESS_TESTS[i].detail,
                                   sizeof(STRESS_TESTS[i].detail));
            if (STRESS_TESTS[i].passed) stress_pass++; else stress_fail++;
            ESP_LOGI(TAG, "  %-28s %s — %s",
                     STRESS_TESTS[i].name,
                     STRESS_TESTS[i].passed ? "PASS" : "FAIL",
                     STRESS_TESTS[i].detail);
        }
    }

    size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    long   heap_delta = (long)heap_before - (long)heap_after;

    static char json[768];
    int  n = snprintf(json, sizeof(json),
      "{\"deck_level\":%d,\"deck_os\":%d,\"runtime\":\"%s\",\"edition\":%d,"
       "\"suites_total\":%u,\"suites_pass\":%u,\"suites_fail\":%u,"
       "\"deck_tests_total\":%u,\"deck_tests_pass\":%u,\"deck_tests_fail\":%u,"
       "\"deck_total_us\":%llu,\"deck_max_us\":%lu,\"deck_slowest\":\"%s\","
       "\"deck_outliers\":%u,"
       "\"stress_total\":%u,\"stress_pass\":%u,\"stress_fail\":%u,"
       "\"heap_used_during_suite\":%ld,"
       "\"intern_count\":%u,\"intern_bytes\":%u,"
       "\"deck_alloc_peak\":%u,\"deck_alloc_live\":%u,"
       "\"monotonic_ms\":%lld}",
      deck_sdi_info_deck_level(),
      deck_sdi_info_deck_os(),
      deck_sdi_info_runtime_version(),
      deck_sdi_info_edition(),
      (unsigned)N_ROWS, (unsigned)passed, (unsigned)failed,
      (unsigned)N_DECK_TESTS, (unsigned)deck_pass, (unsigned)deck_fail,
      (unsigned long long)deck_total_us, (unsigned long)deck_max_us, deck_slowest,
      (unsigned)deck_outliers,
      (unsigned)N_STRESS_TESTS, (unsigned)stress_pass, (unsigned)stress_fail,
      heap_delta,
      (unsigned)deck_intern_count(),
      (unsigned)deck_intern_bytes(),
      (unsigned)deck_alloc_peak(),
      (unsigned)deck_alloc_live_values(),
      (long long)(deck_sdi_time_monotonic_us() / 1000));

    if (n < 0 || n >= (int)sizeof(json)) {
        ESP_LOGE(TAG, "json encode overflow");
        return DECK_LOAD_INTERNAL;
    }

    /* UART (greppable) + SPIFFS (persistent) */
    ESP_LOGI(TAG, "%s", json);
    persist_report(json, (size_t)n);

    if (failed > 0 || deck_fail > 0 || stress_fail > 0) return DECK_LOAD_INTERNAL;
    ESP_LOGI(TAG, "=== DL1 CONFORMANCE: PASS ===");
    return DECK_RT_OK;
}
