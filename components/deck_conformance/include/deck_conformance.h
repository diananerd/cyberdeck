#pragma once

/* deck_conformance — DL1 conformance aggregator.
 *
 * Runs every selftest in the runtime + SDI + shell stack, tabulates
 * pass/fail by category, and emits a structured report via ESP_LOGI
 * in JSON-style flat form that downstream tooling can grep.
 *
 * Categories follow deck-lang/16-deck-levels.md §11.1:
 *   language.lex, language.ast, language.eval, loader, drivers,
 *   apps, memory.
 */

#include "deck_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Run the full DL1 conformance suite. Returns OK only if all suites
 * pass. Prints a per-category table + a final JSON-line summary. */
deck_err_t deck_conformance_run(void);

#ifdef __cplusplus
}
#endif
