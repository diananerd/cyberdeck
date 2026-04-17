#pragma once

/* system.info — static + runtime device attributes.
 *
 * Mandatory at DL1. Exposes what the runtime needs to enforce version
 * checks (deck_level, deck_os, runtime, edition), plus observable
 * device state (device_id, free_heap, current app).
 *
 * See deck-lang/03-deck-os.md §system.info and
 *     deck-lang/16-deck-levels.md §9.2.
 */

#include "deck_sdi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Stable unique identifier — on ESP32, derived from the factory MAC. */
    const char *(*device_id)(void *ctx);

    /* Free internal heap in bytes (internal SRAM only; excludes PSRAM). */
    size_t (*free_heap)(void *ctx);

    /* Conformance level this runtime claims: DL1=1, DL2=2, DL3=3. */
    int (*deck_level)(void *ctx);

    /* Surface API level (edition-scoped). Always 1 for edition 2026. */
    int (*deck_os)(void *ctx);

    /* Runtime semver string, e.g. "0.2.0". */
    const char *(*runtime_version)(void *ctx);

    /* Language edition year. */
    int (*edition)(void *ctx);

    /* Id of the currently running Deck app, or NULL if none. */
    const char *(*current_app_id)(void *ctx);
} deck_sdi_info_vtable_t;

deck_sdi_err_t deck_sdi_info_register(void);

const char *deck_sdi_info_device_id(void);
size_t      deck_sdi_info_free_heap(void);
int         deck_sdi_info_deck_level(void);
int         deck_sdi_info_deck_os(void);
const char *deck_sdi_info_runtime_version(void);
int         deck_sdi_info_edition(void);
const char *deck_sdi_info_current_app_id(void);

deck_sdi_err_t deck_sdi_info_selftest(void);

#ifdef __cplusplus
}
#endif
