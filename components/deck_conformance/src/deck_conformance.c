#include "deck_conformance.h"
#include "deck_runtime.h"
#include "deck_alloc.h"
#include "deck_intern.h"
#include "drivers/deck_sdi_info.h"
#include "drivers/deck_sdi_time.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdbool.h>
#include <stdio.h>
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

    size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    long   heap_delta = (long)heap_before - (long)heap_after;

    static char json[512];
    int  n = snprintf(json, sizeof(json),
      "{\"deck_level\":%d,\"deck_os\":%d,\"runtime\":\"%s\",\"edition\":%d,"
       "\"suites_total\":%u,\"suites_pass\":%u,\"suites_fail\":%u,"
       "\"heap_used_during_suite\":%ld,"
       "\"intern_count\":%u,\"intern_bytes\":%u,"
       "\"deck_alloc_peak\":%u,\"deck_alloc_live\":%u,"
       "\"monotonic_ms\":%lld}",
      deck_sdi_info_deck_level(),
      deck_sdi_info_deck_os(),
      deck_sdi_info_runtime_version(),
      deck_sdi_info_edition(),
      (unsigned)N_ROWS, (unsigned)passed, (unsigned)failed,
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

    if (failed > 0) return DECK_LOAD_INTERNAL;
    ESP_LOGI(TAG, "=== DL1 CONFORMANCE: PASS ===");
    return DECK_RT_OK;
}
