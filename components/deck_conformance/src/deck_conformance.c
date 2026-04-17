#include "deck_conformance.h"
#include "deck_runtime.h"
#include "deck_alloc.h"
#include "deck_intern.h"
#include "drivers/deck_sdi_info.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdbool.h>

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

    /* JSON-line summary — greppable from the UART log. */
    ESP_LOGI(TAG,
      "{\"deck_level\":%d,\"deck_os\":%d,\"runtime\":\"%s\",\"edition\":%d,"
       "\"suites_total\":%u,\"suites_pass\":%u,\"suites_fail\":%u,"
       "\"heap_used_during_suite\":%ld,"
       "\"intern_count\":%u,\"intern_bytes\":%u,"
       "\"deck_alloc_peak\":%u,\"deck_alloc_live\":%u}",
      deck_sdi_info_deck_level(),
      deck_sdi_info_deck_os(),
      deck_sdi_info_runtime_version(),
      deck_sdi_info_edition(),
      (unsigned)N_ROWS, (unsigned)passed, (unsigned)failed,
      heap_delta,
      (unsigned)deck_intern_count(),
      (unsigned)deck_intern_bytes(),
      (unsigned)deck_alloc_peak(),
      (unsigned)deck_alloc_live_values());

    if (failed > 0) return DECK_LOAD_INTERNAL;
    ESP_LOGI(TAG, "=== DL1 CONFORMANCE: PASS ===");
    return DECK_RT_OK;
}
