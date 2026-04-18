#pragma once

/* deck_shell_lockscreen — PIN entry overlay.
 *
 * Renders a full-screen lockscreen on lv_layer_top (so it covers any
 * activity below) with a 4-digit PIN field + numpad. On verify, dims
 * itself and invokes `on_unlocked`.
 *
 * Pre-condition: deck_sdi_security driver must be registered. If no
 * PIN is set, `_show` returns immediately and invokes `on_unlocked`
 * synchronously.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*deck_shell_unlock_cb_t)(void);

/* Show the lockscreen. cb fires on successful PIN verify (or
 * immediately if no PIN is set). Idempotent — second call is no-op. */
void deck_shell_lockscreen_show(deck_shell_unlock_cb_t cb);

/* Force the lockscreen back up — called when the user requests
 * "lock now" from settings. */
void deck_shell_lockscreen_lock(deck_shell_unlock_cb_t cb);

bool deck_shell_lockscreen_is_visible(void);

#ifdef __cplusplus
}
#endif
