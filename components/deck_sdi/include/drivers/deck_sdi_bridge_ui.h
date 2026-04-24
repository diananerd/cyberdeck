#pragma once

/* bridge.ui — DVC (Deck View Container) decoder driver.
 *
 * DL2 driver. The runtime emits binary snapshots in the DVC wire format
 * (see deck-lang/10-deck-bridge-ui.md and 11-deck-implementation.md
 * §IPC). This driver receives those bytes and renders them through the
 * platform UI layer (LVGL on the reference board).
 *
 * F25.7 ships only the skeleton: registration + a `push_snapshot` entry
 * that validates inputs and logs the byte count. Component decode +
 * widget construction lands in F26.
 */

#include "deck_sdi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Callback carried by a confirm-dialog or choice request. Exactly one
 * of on_ok / on_cancel fires per confirm. Choice callbacks carry the
 * selected index. */
typedef void (*deck_sdi_bridge_ui_cb_t)(void *user_data);
typedef void (*deck_sdi_bridge_ui_choice_cb_t)(void *user_data, int index);

/* BRIDGE §4 stratified vtable. Four bands:
 *   Core               — content pipeline + resolution services +
 *                         security + theme. Every bridge implements.
 *   Visual             — on-screen text entry, statusbar, navbar, badge.
 *                         Implemented by any bridge with a raster/char
 *                         output surface.
 *   Physical-display   — panel-only: rotation + brightness.
 *
 * Stubbing convention: a NULL method pointer is treated as
 * NOT_SUPPORTED by the SDI wrappers below; substrate bridges that do
 * not apply simply leave slots NULL.
 */
typedef struct {
    /* ===== Core (every bridge implements) =============================== */

    /* Init the UI layer (display/touch already up). Idempotent. */
    deck_sdi_err_t (*init)(void *ctx);

    /* Content pipeline — BRIDGE §6. */
    deck_sdi_err_t (*push_snapshot)(void *ctx, const void *bytes, size_t len);
    deck_sdi_err_t (*clear)(void *ctx);

    /* Resolution services — BRIDGE §Part IV. */
    deck_sdi_err_t (*toast)(void *ctx, const char *text, uint32_t duration_ms);
    deck_sdi_err_t (*confirm)(void *ctx,
                               const char *title, const char *message,
                               const char *ok_label, const char *cancel_label,
                               deck_sdi_bridge_ui_cb_t on_ok,
                               deck_sdi_bridge_ui_cb_t on_cancel,
                               void *user_data);
    deck_sdi_err_t (*loading_show)(void *ctx, const char *label);
    deck_sdi_err_t (*loading_hide)(void *ctx);
    deck_sdi_err_t (*progress_show)(void *ctx, const char *label);
    deck_sdi_err_t (*progress_set)(void *ctx, float pct);   /* -1 = indeterminate */
    deck_sdi_err_t (*progress_hide)(void *ctx);
    deck_sdi_err_t (*choice_show)(void *ctx, const char *title,
                                   const char *const *options, uint16_t n_options,
                                   deck_sdi_bridge_ui_choice_cb_t on_pick,
                                   void *user_data);
    deck_sdi_err_t (*multiselect_show)(void *ctx, const char *title,
                                        const char *const *options, uint16_t n_options,
                                        const bool *initially_selected,
                                        deck_sdi_bridge_ui_cb_t on_done,
                                        void *user_data);
    deck_sdi_err_t (*date_show)(void *ctx, const char *title,
                                 int64_t initial_epoch_ms,
                                 deck_sdi_bridge_ui_cb_t on_pick,
                                 void *user_data);
    deck_sdi_err_t (*share_show)(void *ctx, const char *text, const char *url);
    deck_sdi_err_t (*permission_show)(void *ctx, const char *permission_name,
                                       const char *rationale,
                                       deck_sdi_bridge_ui_cb_t on_grant,
                                       deck_sdi_bridge_ui_cb_t on_deny,
                                       void *user_data);

    /* Security + theme — BRIDGE §Part IV. */
    deck_sdi_err_t (*set_locked)(void *ctx, bool locked);
    deck_sdi_err_t (*set_theme)(void *ctx, const char *theme_atom);

    /* ===== Visual (bridges with a raster or char output) ================ */

    deck_sdi_err_t (*keyboard_show)(void *ctx, const char *kind_atom);
    deck_sdi_err_t (*keyboard_hide)(void *ctx);
    deck_sdi_err_t (*set_statusbar)(void *ctx, bool visible);
    deck_sdi_err_t (*set_navbar)(void *ctx, bool visible);
    deck_sdi_err_t (*set_badge)(void *ctx, const char *app_id, int count);

    /* ===== Physical-display (pixel-controllable panel) ================== */

    deck_sdi_err_t (*set_rotation)(void *ctx, int rot);     /* 0/90/180/270 */
    deck_sdi_err_t (*set_brightness)(void *ctx, float level);
} deck_sdi_bridge_ui_vtable_t;

/* Skeleton implementation — accepts snapshots, logs byte count, no
 * actual decode/render yet. Suitable for wiring tests and DVC encoder
 * round-trip validation in F26.1–F26.2. */
deck_sdi_err_t deck_sdi_bridge_ui_register_skeleton(void);

/* High-level wrappers. Each returns DECK_SDI_ERR_NOT_FOUND if the
 * driver isn't registered, NOT_SUPPORTED if registered but the method
 * slot is NULL (stratification — voice bridges stub visual methods,
 * terminal bridges stub physical-display methods, etc.). */
deck_sdi_err_t deck_sdi_bridge_ui_init(void);
deck_sdi_err_t deck_sdi_bridge_ui_push_snapshot(const void *bytes, size_t len);
deck_sdi_err_t deck_sdi_bridge_ui_clear(void);

/* Core resolution services. */
deck_sdi_err_t deck_sdi_bridge_ui_toast(const char *text, uint32_t duration_ms);
deck_sdi_err_t deck_sdi_bridge_ui_confirm(const char *title, const char *message,
                                           const char *ok_label,
                                           const char *cancel_label,
                                           deck_sdi_bridge_ui_cb_t on_ok,
                                           deck_sdi_bridge_ui_cb_t on_cancel,
                                           void *user_data);
deck_sdi_err_t deck_sdi_bridge_ui_loading_show(const char *label);
deck_sdi_err_t deck_sdi_bridge_ui_loading_hide(void);
deck_sdi_err_t deck_sdi_bridge_ui_progress_show(const char *label);
deck_sdi_err_t deck_sdi_bridge_ui_progress_set(float pct);
deck_sdi_err_t deck_sdi_bridge_ui_progress_hide(void);
deck_sdi_err_t deck_sdi_bridge_ui_choice_show(const char *title,
                                               const char *const *options,
                                               uint16_t n_options,
                                               deck_sdi_bridge_ui_choice_cb_t on_pick,
                                               void *user_data);
deck_sdi_err_t deck_sdi_bridge_ui_multiselect_show(const char *title,
                                                    const char *const *options,
                                                    uint16_t n_options,
                                                    const bool *initially_selected,
                                                    deck_sdi_bridge_ui_cb_t on_done,
                                                    void *user_data);
deck_sdi_err_t deck_sdi_bridge_ui_date_show(const char *title,
                                             int64_t initial_epoch_ms,
                                             deck_sdi_bridge_ui_cb_t on_pick,
                                             void *user_data);
deck_sdi_err_t deck_sdi_bridge_ui_share_show(const char *text, const char *url);
deck_sdi_err_t deck_sdi_bridge_ui_permission_show(const char *permission_name,
                                                   const char *rationale,
                                                   deck_sdi_bridge_ui_cb_t on_grant,
                                                   deck_sdi_bridge_ui_cb_t on_deny,
                                                   void *user_data);
deck_sdi_err_t deck_sdi_bridge_ui_set_locked(bool locked);
deck_sdi_err_t deck_sdi_bridge_ui_set_theme(const char *theme_atom);

/* Visual band. */
deck_sdi_err_t deck_sdi_bridge_ui_keyboard_show(const char *kind_atom);
deck_sdi_err_t deck_sdi_bridge_ui_keyboard_hide(void);
deck_sdi_err_t deck_sdi_bridge_ui_set_statusbar(bool visible);
deck_sdi_err_t deck_sdi_bridge_ui_set_navbar(bool visible);
deck_sdi_err_t deck_sdi_bridge_ui_set_badge(const char *app_id, int count);

/* Physical-display band. */
deck_sdi_err_t deck_sdi_bridge_ui_set_rotation(int rot);
deck_sdi_err_t deck_sdi_bridge_ui_set_brightness(float level);

/* Selftest: init + push 0-length snapshot returns INVALID_ARG;
 * push small dummy buffer returns OK. */
deck_sdi_err_t deck_sdi_bridge_ui_selftest(void);

#ifdef __cplusplus
}
#endif
