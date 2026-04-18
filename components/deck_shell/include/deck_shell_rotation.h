#pragma once

/* deck_shell_rotation — display rotation persisted in NVS. */

#include "deck_error.h"
#include "deck_bridge_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Read NVS namespace="cyberdeck" key="display_rot" and apply. No-op
 * if the key isn't set. */
deck_err_t deck_shell_rotation_restore(void);

/* Set + persist. */
deck_err_t deck_shell_rotation_set(deck_bridge_ui_rotation_t rot);

#ifdef __cplusplus
}
#endif
