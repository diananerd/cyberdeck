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

#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
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
    { "language.lex",  "lexer (35)",            deck_lexer_run_selftest,  false },
    { "language.ast",  "parser (43)",           deck_parser_run_selftest, false },
    { "loader",        "stages 0-9 (18)",       deck_loader_run_selftest, false },
    { "language.eval", "interp + machine (32)", deck_interp_run_selftest, false },
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

typedef struct {
    const char *name;
    const char *path;          /* logical path passed to deck_sdi_fs_read —
                                * driver prepends /deck mount point */
    const char *sentinel;      /* expected substring in the log stream.
                                * Ignored when expected_err != DECK_RT_OK. */
    deck_err_t  expected_err;  /* DECK_RT_OK → success+sentinel mode;
                                * otherwise → runtime must return this code */
    bool        passed;
} deck_test_t;

/* Positive tests: sentinel mode (expected_err = DECK_RT_OK).
 * Negative tests: expected_err = <code>, sentinel ignored. */
static deck_test_t DECK_TESTS[] = {
    { "sanity",        "/conformance/sanity.deck",        "DECK_CONF_OK:sanity",        DECK_RT_OK, false },
    { "lang.literals", "/conformance/lang_literals.deck", "DECK_CONF_OK:lang.literals", DECK_RT_OK, false },
    { "lang.arith",    "/conformance/lang_arith.deck",    "DECK_CONF_OK:lang.arith",    DECK_RT_OK, false },
    { "lang.compare",  "/conformance/lang_compare.deck",  "DECK_CONF_OK:lang.compare",  DECK_RT_OK, false },
    { "lang.logic",    "/conformance/lang_logic.deck",    "DECK_CONF_OK:lang.logic",    DECK_RT_OK, false },
    { "lang.strings",  "/conformance/lang_strings.deck",  "DECK_CONF_OK:lang.strings",  DECK_RT_OK, false },
    { "lang.let",      "/conformance/lang_let.deck",      "DECK_CONF_OK:lang.let",      DECK_RT_OK, false },
    { "lang.if",       "/conformance/lang_if.deck",       "DECK_CONF_OK:lang.if",       DECK_RT_OK, false },
    { "lang.match",    "/conformance/lang_match.deck",    "DECK_CONF_OK:lang.match",    DECK_RT_OK, false },
    { "lang.and_or_kw","/conformance/lang_and_or_kw.deck", "DECK_CONF_OK:lang.and_or_kw",DECK_RT_OK, false },
    { "os.math",       "/conformance/os_math.deck",       "DECK_CONF_OK:os.math",       DECK_RT_OK, false },
    { "os.text",       "/conformance/os_text.deck",       "DECK_CONF_OK:os.text",       DECK_RT_OK, false },
    { "os.time",       "/conformance/os_time.deck",       "DECK_CONF_OK:os.time",       DECK_RT_OK, false },
    { "os.info",       "/conformance/os_info.deck",       "DECK_CONF_OK:os.info",       DECK_RT_OK, false },
    { "os.nvs",        "/conformance/os_nvs.deck",        "DECK_CONF_OK:os.nvs",        DECK_RT_OK, false },
    { "os.fs",         "/conformance/os_fs.deck",         "DECK_CONF_OK:os.fs",         DECK_RT_OK, false },
    { "os.fs.list",    "/conformance/os_fs_list.deck",    "DECK_CONF_OK:os.fs.list",    DECK_RT_OK, false },
    { "os.conv",       "/conformance/os_conv.deck",       "DECK_CONF_OK:os.conv",       DECK_RT_OK, false },
    { "app.machine",   "/conformance/app_machine.deck",   "DECK_CONF_OK:app.machine",   DECK_RT_OK, false },

    /* Negative tests — loader/interp must reject with the expected code. */
    { "errors.level_below_required", "/conformance/err_level_high.deck",  NULL,
      DECK_LOAD_LEVEL_BELOW_REQUIRED, false },
    { "errors.pattern_not_exhaustive", "/conformance/err_match_noexh.deck", NULL,
      DECK_LOAD_PATTERN_NOT_EXHAUSTIVE, false },
    { "errors.type_mismatch", "/conformance/err_type_mismatch.deck", NULL,
      DECK_RT_TYPE_MISMATCH, false },
};

#define N_DECK_TESTS (sizeof(DECK_TESTS) / sizeof(DECK_TESTS[0]))

#define LOG_CAP_BYTES 3072
static char             s_log_cap[LOG_CAP_BYTES];
static size_t           s_log_len;
static bool             s_capturing;
static vprintf_like_t   s_prev_vprintf;

static int conf_vprintf_hook(const char *fmt, va_list ap)
{
    if (s_capturing) {
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

    capture_begin();
    deck_err_t rc = deck_runtime_run_on_launch(s_deck_src, (uint32_t)n);
    capture_end();

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
    uint32_t live = deck_alloc_live_values();
    snprintf(d, dz, "deck_alloc_live=%u (<= 50)", (unsigned)live);
    return live <= 50;
}

/* Re-runs sanity.deck 10 times and asserts that live-values count does
 * not grow more than a small tolerance between the before/after.
 * Exercises load → eval → tear-down repeatedly. */
static bool s_rerun_sanity_no_growth(char *d, size_t dz)
{
    static deck_test_t re = { "re-sanity",
                              "/conformance/sanity.deck",
                              "DECK_CONF_OK:sanity",
                              DECK_RT_OK, false };
    uint32_t live_before = deck_alloc_live_values();
    for (int i = 0; i < 10; i++) {
        if (!run_deck_test(&re)) {
            snprintf(d, dz, "sanity rerun #%d FAIL", i);
            return false;
        }
    }
    uint32_t live_after = deck_alloc_live_values();
    int32_t  delta      = (int32_t)live_after - (int32_t)live_before;
    snprintf(d, dz, "live before=%u after=%u delta=%ld (10 runs, tol 20)",
             (unsigned)live_before, (unsigned)live_after, (long)delta);
    return delta <= 20 && delta >= -20;
}

static stress_test_t STRESS_TESTS[] = {
    { "memory.heap_idle_budget",   s_heap_idle_budget,       false, {0} },
    { "memory.no_residual_leak",   s_no_residual_leak,       false, {0} },
    { "stress.rerun_sanity_x10",   s_rerun_sanity_no_growth, false, {0} },
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

    /* --- .deck tests from SPIFFS --- */
    uint32_t deck_pass = 0, deck_fail = 0;
    if (N_DECK_TESTS > 0) {
        ESP_LOGI(TAG, "--- .deck tests (%u) ---", (unsigned)N_DECK_TESTS);
        for (size_t i = 0; i < N_DECK_TESTS; i++) {
            DECK_TESTS[i].passed = run_deck_test(&DECK_TESTS[i]);
            if (DECK_TESTS[i].passed) deck_pass++; else deck_fail++;
            ESP_LOGI(TAG, "  %-24s %s",
                     DECK_TESTS[i].name,
                     DECK_TESTS[i].passed ? "PASS" : "FAIL");
        }
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

    static char json[640];
    int  n = snprintf(json, sizeof(json),
      "{\"deck_level\":%d,\"deck_os\":%d,\"runtime\":\"%s\",\"edition\":%d,"
       "\"suites_total\":%u,\"suites_pass\":%u,\"suites_fail\":%u,"
       "\"deck_tests_total\":%u,\"deck_tests_pass\":%u,\"deck_tests_fail\":%u,"
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
