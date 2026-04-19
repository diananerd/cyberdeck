#pragma once

/* deck_shell_deck_apps — .deck source app discovery + registration.
 *
 * Scans the bundled apps partition for .deck files at boot and loads each
 * via deck_runtime_app_load (the persistent-handle API landed in F28
 * Phase 1). Successful loads get a sequential app_id starting from
 * DECK_APPS_BASE_ID (100) and an intent resolver that fires
 * `@on resume` on the handle whenever the launcher card is tapped.
 *
 * UI drawing from inside .deck code is NOT yet supported — .deck apps
 * currently run purely in the log stream (log.info, state machine
 * transitions). Phase 2 of the .deck-source-apps deferral will add
 * bridge.ui.* builtins so apps can build DVC snapshots. Until then the
 * shell shows a toast with the app name when the resume event fires.
 *
 * Callers: deck_shell_dl2_boot. Not thread-safe; call once at boot.
 */

#include <stdint.h>
#include "deck_error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DECK_APPS_BASE_ID       100
#define DECK_APPS_MAX_LOADED      8

/* How many .deck apps were loaded successfully during the last scan.
 * Exposed so the launcher can iterate registered apps to build cards. */
uint32_t deck_shell_deck_apps_count(void);

/* Metadata for a loaded .deck app, indexed by the order it was registered
 * (0..count-1). Returns NULL fields for out-of-range idx. */
typedef struct {
    uint16_t    app_id;
    const char *id;        /* Deck @app id, e.g. "sys.hello" */
    const char *name;      /* Deck @app name, e.g. "Hello" */
    const char *path;      /* fs path the app was read from */
} deck_shell_deck_app_info_t;

void deck_shell_deck_apps_info(uint32_t idx, deck_shell_deck_app_info_t *out);

/* Scan the filesystem root for *.deck files, load each as a persistent
 * runtime app handle, and register an intent resolver. Returns OK even
 * if zero apps load (empty fs is not an error). Individual load failures
 * are logged but do not abort the scan. */
deck_err_t deck_shell_deck_apps_scan_and_register(void);

#ifdef __cplusplus
}
#endif
