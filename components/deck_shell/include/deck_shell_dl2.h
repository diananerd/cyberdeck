#pragma once

/* deck_shell_dl2 — DL2 shell entry point.
 *
 * Replaces the DL1 single-app shell (`deck_shell_boot`) with a real
 * shell:
 *
 *   1. boot → restore display rotation from NVS (F27.4)
 *   2. if a PIN is set, show lockscreen until verified (F27.1)
 *   3. push the launcher activity at slot 0 (F27.3)
 *   4. wire navbar BACK/HOME → activity stack pop / pop_to_home (F27.2)
 *   5. ui_intent_navigate(intent) routes intents to activity push (F27.2)
 *
 * F28+ wires app loading from SD/SPIFFS into the launcher.
 */

#include "deck_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Boot the DL2 shell. Pre-conditions: bridge_ui.lvgl is initialized,
 * statusbar + navbar mounted. Idempotent — second call is a no-op. */
deck_err_t deck_shell_dl2_boot(void);

/* True once the lockscreen has been dismissed (or no PIN was set on
 * boot). Apps should not run code that requires the user to be present
 * until this is true. */
bool deck_shell_dl2_unlocked(void);

#ifdef __cplusplus
}
#endif
