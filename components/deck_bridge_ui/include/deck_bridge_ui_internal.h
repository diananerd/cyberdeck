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

/* ---- PATCH path (BRIDGE §9) ----
 *
 * The render engine records every primary widget it creates in a
 * pre-order widget map, which the PATCH path later walks in parallel
 * with the old + new trees. If both trees are same_shape (see
 * deck_dvc_tree_same_shape) the patch path updates leaf attribute
 * values (label text, slider/progress/toggle state) in place instead
 * of rebuilding the widget tree.
 *
 * Orchestration:
 *   deck_bridge_ui_patch_begin(cap)  — start recording into an array
 *   deck_bridge_ui_patch_record(n,w) — called by renderers per widget
 *   deck_bridge_ui_patch_snapshot()  — finalize; return ownership to
 *                                       caller (widgets[], count)
 *   deck_bridge_ui_patch_apply(old, new, widgets, n) — apply leaf-attr
 *     diffs to the widgets. Returns 0 on success, nonzero if any diff
 *     is unsupported — caller then performs a full REBUILD.
 */

typedef struct {
    const deck_dvc_node_t *node;
    lv_obj_t              *obj;
} deck_bridge_ui_patch_entry_t;

void    deck_bridge_ui_patch_begin(size_t cap_hint);
void    deck_bridge_ui_patch_record(const deck_dvc_node_t *n, lv_obj_t *obj);
size_t  deck_bridge_ui_patch_snapshot(deck_bridge_ui_patch_entry_t **out);
int     deck_bridge_ui_patch_apply(const deck_dvc_node_t *old_root,
                                    const deck_dvc_node_t *new_root,
                                    const deck_bridge_ui_patch_entry_t *entries,
                                    size_t n_entries);

/* Wipe the active screen — clears all children of lv_scr_act(). */
void deck_bridge_ui_clear_screen(void);

/* Try to render `n` as an overlay on lv_layer_top. Returns true if the
 * node was an overlay type (TOAST/LOADING/CONFIRM) and was handled.
 * Implemented in deck_bridge_ui_overlays.c. */
bool deck_bridge_ui_render_overlay(const deck_dvc_node_t *n);

#ifdef __cplusplus
}
#endif
