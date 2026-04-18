#pragma once

/* Internal cross-file API — not part of the public bridge_ui ABI. */

#include "lvgl.h"
#include "deck_sdi.h"
#include "deck_dvc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LVGL stack init (one-shot). Brings up the LVGL display driver wired
 * to the SDI panel + a touch indev wired to the SDI touch driver, and
 * spawns the LVGL tick + handler task on Core 1. */
deck_sdi_err_t deck_bridge_ui_lvgl_init(void);

/* True once the LVGL stack is up and the task is running. */
bool deck_bridge_ui_lvgl_is_ready(void);

/* Snapshot apply (under ui_lock, called by SDI push_snapshot).
 * Wipes the active screen contents and rebuilds widgets from `root`. */
deck_sdi_err_t deck_bridge_ui_render(const deck_dvc_node_t *root);

/* Wipe the active screen — clears all children of lv_scr_act(). */
void deck_bridge_ui_clear_screen(void);

/* Try to render `n` as an overlay on lv_layer_top. Returns true if the
 * node was an overlay type (TOAST/LOADING/CONFIRM) and was handled.
 * Implemented in deck_bridge_ui_overlays.c. */
bool deck_bridge_ui_render_overlay(const deck_dvc_node_t *n);

#ifdef __cplusplus
}
#endif
