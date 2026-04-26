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

/* Callback-based confirm. Convenience for C consumers that want to run
 * an action directly instead of routing through the DVC intent_id
 * mechanism. Either/both callbacks may be NULL. The overlay is always
 * dismissed first, then the callback fires — so the callback may push a
 * new activity / pop the current one safely without touching the
 * dialog's widget tree. */
typedef void (*deck_bridge_ui_overlay_cb_t)(void *user_data);
void deck_bridge_ui_overlay_confirm_cb(const char *title, const char *message,
                                        const char *ok_label,
                                        const char *cancel_label,
                                        deck_bridge_ui_overlay_cb_t on_ok,
                                        deck_bridge_ui_overlay_cb_t on_cancel,
                                        void *user_data);

/* Progress overlay — modal with a bar and optional label. pct is 0..1;
 * a negative value means indeterminate (marquee). */
void deck_bridge_ui_overlay_progress_show(const char *label);
void deck_bridge_ui_overlay_progress_set(float pct);
void deck_bridge_ui_overlay_progress_hide(void);

/* Choice / multiselect overlays — modal list. The `_cb` callback fires
 * exactly once with the selected index (choice) or when DONE is pressed
 * (multiselect). Caller owns `options[]` / `initially_selected[]` only for
 * the duration of the call; the overlay copies them internally. */
typedef void (*deck_bridge_ui_overlay_choice_cb_t)(void *user_data, int index);
typedef void (*deck_bridge_ui_overlay_multiselect_cb_t)(void *user_data,
                                                         const bool *selected,
                                                         uint16_t n);
void deck_bridge_ui_overlay_choice_show(const char *title,
                                         const char *const *options,
                                         uint16_t n_options,
                                         deck_bridge_ui_overlay_choice_cb_t on_pick,
                                         void *user_data);
void deck_bridge_ui_overlay_multiselect_show(const char *title,
                                              const char *const *options,
                                              uint16_t n_options,
                                              const bool *initially_selected,
                                              deck_bridge_ui_overlay_multiselect_cb_t on_done,
                                              void *user_data);

/* Keyboard overlay — LVGL keyboard docked at the bottom of the screen,
 * tied to a single-line textarea shown just above it. kind_atom selects
 * the keymap layout: "text" / "text_upper" / "number" / "password". */
void deck_bridge_ui_overlay_keyboard_show(const char *kind_atom);
void deck_bridge_ui_overlay_keyboard_hide(void);

/* J5 — native date picker via three lv_roller wheels (year/month/day).
 * `on_pick` fires when the user taps OK; `on_cancel` (may be NULL)
 * fires on dismiss. After OK the latest values are readable via
 * `_picked_year/_month/_day`. */
void deck_bridge_ui_overlay_date_show(const char *title,
                                       int initial_year,
                                       int initial_month,
                                       int initial_day,
                                       deck_bridge_ui_overlay_cb_t on_pick,
                                       deck_bridge_ui_overlay_cb_t on_cancel,
                                       void *user_data);
int  deck_bridge_ui_overlay_date_picked_year (void);
int  deck_bridge_ui_overlay_date_picked_month(void);
int  deck_bridge_ui_overlay_date_picked_day  (void);

/* J6 — native share sheet: shows text + URL on a card with COPY +
 * DISMISS buttons. COPY fires on_copy (caller can stash the payload),
 * DISMISS fires on_dismiss. Either may be NULL. */
void deck_bridge_ui_overlay_share_show(const char *text,
                                        const char *url,
                                        deck_bridge_ui_overlay_cb_t on_copy,
                                        deck_bridge_ui_overlay_cb_t on_dismiss,
                                        void *user_data);

/* J6 — native permission sheet: dedicated card with bold permission
 * name, rationale paragraph, ALLOW + DENY buttons. */
void deck_bridge_ui_overlay_permission_show(const char *permission,
                                             const char *rationale,
                                             deck_bridge_ui_overlay_cb_t on_grant,
                                             deck_bridge_ui_overlay_cb_t on_deny,
                                             void *user_data);

/* ---------- Statusbar (top dock — time + WiFi + battery) ---------- */

deck_sdi_err_t deck_bridge_ui_statusbar_init(void);
void           deck_bridge_ui_statusbar_refresh(void);
/* Resize + re-align the statusbar / navbar after a display rotation.
 * Called internally by deck_bridge_ui_set_rotation; exposed for test
 * harnesses and platforms that rotate the display through other paths. */
void           deck_bridge_ui_statusbar_relayout(void);
void           deck_bridge_ui_navbar_relayout(void);

/* Show / hide the statusbar and navbar. Toggles the LVGL HIDDEN flag on
 * the dock objects so they keep their widget tree and refresh timers
 * alive. Safe to call before the dock is initialized — calls become
 * no-ops in that case. */
void           deck_bridge_ui_statusbar_set_visible(bool visible);
void           deck_bridge_ui_navbar_set_visible(bool visible);

/* J7 — apply a theme atom (`"green"` / `"amber"` / `"neon"`) by
 * re-coloring the statusbar + navbar widgets. The activity content
 * underneath only repaints when the runtime pushes its next snapshot;
 * the docks update immediately. Unknown atoms fall back to green. */
void           deck_bridge_ui_statusbar_apply_theme(const char *atom);
void           deck_bridge_ui_navbar_apply_theme(const char *atom);

/* J4 — set / clear a numeric badge pill for an app in the statusbar.
 * count == 0 hides the pill. Multiple apps may have badges; pills
 * stack right-to-left after the time. */
void           deck_bridge_ui_statusbar_set_badge(const char *app_id, int count);

/* J10 — UI tap test harness. Walks the most recently rendered widget
 * map and synthesises an LV_EVENT_CLICKED on the lv_obj for the node
 * matching the requested intent_id. Exercises the full
 * touchscreen → bridge intent_hook → runtime path without needing a
 * physical tap. Returns true if a matching widget was found and the
 * event was sent. */
bool           deck_bridge_ui_simulate_tap(uint32_t intent_id);

/* Test-harness assertions over the post-render widget tree.
 *
 * `_assert_intent_visible` — true iff a widget with that intent_id is
 *   present in the most recent rendered slot AND its lv_obj is not
 *   marked LV_OBJ_FLAG_HIDDEN (overlays count as siblings, not parents).
 * `_assert_label_visible`  — true iff any DVC_LABEL node in the most
 *   recent rendered tree has `value` containing `needle` (substring).
 *   Useful for asserting that a render succeeded with expected text.
 * `_dvc_node_count` — total number of patch-map entries in the slot
 *   matching app_id, or 0 if no slot exists. Useful as a sanity gate
 *   before asserting on individual nodes. */
bool           deck_bridge_ui_assert_intent_visible(uint32_t intent_id);
bool           deck_bridge_ui_assert_label_visible(const char *needle);
size_t         deck_bridge_ui_dvc_node_count(uint32_t app_id);

/* ---------- Navbar (bottom dock — BACK + HOME) ---------- */

typedef void (*deck_bridge_ui_nav_cb_t)(void);
/* Three navbar zones: BACK (left-pointing triangle), HOME (outline circle),
 * TASKS (outline square). The TASKS zone routes to the task manager app.
 * Any callback may be NULL — the zone still renders but tap is a no-op. */
deck_sdi_err_t deck_bridge_ui_navbar_init(deck_bridge_ui_nav_cb_t back_cb,
                                          deck_bridge_ui_nav_cb_t home_cb);
void deck_bridge_ui_navbar_set_tasks_cb(deck_bridge_ui_nav_cb_t tasks_cb);

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
/* Read-only access to a specific activity by stack index. idx=0 is the
 * launcher (bottom), idx=depth-1 is the top. Returns NULL for out-of-
 * range. Pointer is valid until the next push/pop/recreate. */
deck_bridge_ui_activity_t *deck_bridge_ui_activity_at(size_t idx);
void          deck_bridge_ui_activity_set_state(void *state);
void          deck_bridge_ui_activity_recreate_all(void);

/* ---------- Intent hook ----------
 *
 * When a rendered widget with `intent_id != 0` is activated (trigger
 * click, toggle change, slider release, form submit, …), the decoder
 * invokes the registered hook so higher layers can route the intent.
 * Passing NULL clears the hook; missing hook = no-op. Called from the
 * LVGL task context with the UI lock held.
 *
 * Concept #59/#60: `vals` carries the widget's current value payload.
 *   - Scalar widgets: n_vals == 1, vals[0].key == NULL, kind set.
 *   - Form submit:    n_vals == N, every vals[i].key non-NULL (field name).
 *   - Plain trigger:  n_vals == 0, vals == NULL.
 * The runtime side exposes this to Deck apps as `event.value` / `event.values`. */
typedef enum {
    DECK_BRIDGE_UI_VAL_NONE = 0,
    DECK_BRIDGE_UI_VAL_BOOL,
    DECK_BRIDGE_UI_VAL_I64,
    DECK_BRIDGE_UI_VAL_F64,
    DECK_BRIDGE_UI_VAL_STR,
    DECK_BRIDGE_UI_VAL_ATOM,
} deck_bridge_ui_val_kind_t;

typedef struct {
    const char               *key;      /* NULL for scalar payloads */
    deck_bridge_ui_val_kind_t kind;
    bool                      b;
    int64_t                   i;
    double                    f;
    const char               *s;        /* NUL-terminated; lifetime until hook returns */
} deck_bridge_ui_val_t;

typedef void (*deck_bridge_ui_intent_hook_t)(uint32_t intent_id,
                                              const deck_bridge_ui_val_t *vals,
                                              uint32_t n_vals);

void deck_bridge_ui_set_intent_hook(deck_bridge_ui_intent_hook_t hook);

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

/* ---------- Shell-injected handlers ----------
 *
 * The SDI bridge.ui driver routes certain resolution calls out to the
 * shell (lockscreen for set_locked, theme atom storage for set_theme).
 * Rather than having the bridge depend on deck_shell (which would create
 * a cycle since deck_shell depends on deck_bridge_ui for its UI), the
 * shell injects handlers at init time. Passing NULL clears the handler.
 * Missing handler = the vtable call returns OK without side effects. */
typedef void (*deck_bridge_ui_lock_handler_t)(bool locked);
typedef void (*deck_bridge_ui_theme_handler_t)(const char *theme_atom);

void deck_bridge_ui_set_lock_handler(deck_bridge_ui_lock_handler_t cb);
void deck_bridge_ui_set_theme_handler(deck_bridge_ui_theme_handler_t cb);

/* Read the last theme atom seen by the bridge. NULL until one is set. */
const char *deck_bridge_ui_get_theme(void);

#ifdef __cplusplus
}
#endif
