#pragma once

/* deck_bridge_ui — LVGL-backed implementation of the bridge.ui SDI
 * driver.
 *
 * Replaces the F25.7 skeleton (`deck_sdi_bridge_ui_register_skeleton`)
 * with a real driver that runs LVGL on Core 1, owns the framebuffer,
 * and decodes DVC snapshots into LVGL widgets.
 *
 * Lifecycle:
 *   1. host calls `deck_sdi_display_init` + `deck_sdi_touch_init` (or
 *      lets `deck_bridge_ui_register_lvgl` do it — it is idempotent).
 *   2. host calls `deck_bridge_ui_register_lvgl` exactly once. That
 *      installs the driver, spins up the LVGL task on Core 1, and
 *      registers display + touch indev with LVGL.
 *   3. anything that touches LVGL from a non-LVGL thread must take
 *      `deck_bridge_ui_lock(timeout_ms)` / `_unlock`.
 *
 * The runtime publishes snapshots through the SDI bridge.ui driver
 * (`deck_sdi_bridge_ui_push_snapshot`); this component implements that
 * push by decoding the DVC bytes and reconciling the widget tree.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "deck_sdi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the LVGL-backed bridge.ui driver. Idempotent at the
 * registry level (returns ALREADY_EXISTS on second call). Pulls up
 * display + touch via deck_sdi_*_init if not yet initialized. */
deck_sdi_err_t deck_bridge_ui_register_lvgl(void);

/* LVGL mutex. All non-LVGL-task code MUST take this lock before
 * touching any lv_obj_*. timeout_ms=0 → block forever. */
bool deck_bridge_ui_lock(uint32_t timeout_ms);
void deck_bridge_ui_unlock(void);

/* Selftest: register driver, draw a known LABEL via DVC, verify the
 * label exists in the active screen. Briefly dims the backlight to
 * avoid burn-in during repeated test boots. */
deck_sdi_err_t deck_bridge_ui_selftest(void);

/* ---------- Overlays (modal layer over the active screen) ---------- */

void deck_bridge_ui_overlay_toast(const char *text, uint32_t duration_ms);
void deck_bridge_ui_overlay_loading_show(const char *text);
void deck_bridge_ui_overlay_loading_hide(void);
void deck_bridge_ui_overlay_confirm(const char *title, const char *message,
                                     const char *ok_label,
                                     const char *cancel_label,
                                     uint32_t ok_intent,
                                     uint32_t cancel_intent);

/* ---------- Statusbar (top dock — time + WiFi + battery) ---------- */

deck_sdi_err_t deck_bridge_ui_statusbar_init(void);
void           deck_bridge_ui_statusbar_refresh(void);

/* ---------- Navbar (bottom dock — BACK + HOME) ---------- */

typedef void (*deck_bridge_ui_nav_cb_t)(void);
deck_sdi_err_t deck_bridge_ui_navbar_init(deck_bridge_ui_nav_cb_t back_cb,
                                          deck_bridge_ui_nav_cb_t home_cb);

/* ---------- Activity stack (push/pop, max 4 levels) ---------- */

#define DECK_BRIDGE_UI_ACTIVITY_MAX  4

typedef struct deck_bridge_ui_activity deck_bridge_ui_activity_t;

typedef void (*deck_bridge_ui_lifecycle_cb_t)(
    deck_bridge_ui_activity_t *act, void *intent_data);

typedef struct {
    deck_bridge_ui_lifecycle_cb_t on_create;
    deck_bridge_ui_lifecycle_cb_t on_resume;
    deck_bridge_ui_lifecycle_cb_t on_pause;
    deck_bridge_ui_lifecycle_cb_t on_destroy;
} deck_bridge_ui_lifecycle_t;

struct deck_bridge_ui_activity {
    uint16_t                 app_id;
    uint16_t                 screen_id;
    void                    *state;            /* opaque per-activity state */
    void                    *lvgl_screen;      /* lv_obj_t * */
    deck_bridge_ui_lifecycle_t cbs;
};

deck_sdi_err_t deck_bridge_ui_activity_push(uint16_t app_id, uint16_t screen_id,
                                             const deck_bridge_ui_lifecycle_t *cbs,
                                             void *intent_data);
deck_sdi_err_t deck_bridge_ui_activity_pop(void);
deck_sdi_err_t deck_bridge_ui_activity_pop_to_home(void);
deck_bridge_ui_activity_t *deck_bridge_ui_activity_current(void);
size_t        deck_bridge_ui_activity_depth(void);
void          deck_bridge_ui_activity_set_state(void *state);
void          deck_bridge_ui_activity_recreate_all(void);

/* ---------- Rotation ---------- */

typedef enum {
    DECK_BRIDGE_UI_ROT_0   = 0,
    DECK_BRIDGE_UI_ROT_90  = 1,
    DECK_BRIDGE_UI_ROT_180 = 2,
    DECK_BRIDGE_UI_ROT_270 = 3,
} deck_bridge_ui_rotation_t;

/* Apply a display rotation. Calls lv_disp_set_rotation under the lock,
 * then invalidates and recreates every activity in the stack so that
 * each can rebuild its layout for the new dimensions. */
deck_sdi_err_t deck_bridge_ui_set_rotation(deck_bridge_ui_rotation_t rot);
deck_bridge_ui_rotation_t deck_bridge_ui_get_rotation(void);

#ifdef __cplusplus
}
#endif
