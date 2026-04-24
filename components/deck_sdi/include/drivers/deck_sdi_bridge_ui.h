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

/* Stage 7 — callbacks carried by a confirm-dialog request. Either may
 * be NULL; the bridge fires exactly one of them when the user picks. */
typedef void (*deck_sdi_bridge_ui_cb_t)(void *user_data);

typedef struct {
    /* Init the UI layer (display/touch already up). Idempotent. */
    deck_sdi_err_t (*init)(void *ctx);

    /* Apply a DVC snapshot. Format details live in F26.1. */
    deck_sdi_err_t (*push_snapshot)(void *ctx,
                                     const void *bytes, size_t len);

    /* Clear the rendered tree (post-app-terminate cleanup). */
    deck_sdi_err_t (*clear)(void *ctx);

    /* BRIDGE UI service (§Part IV) — confirm dialog. Routes
     * @on back :confirm payloads. Labels / prompt are caller-owned
     * const strings (the bridge copies what it needs). */
    deck_sdi_err_t (*confirm)(void *ctx,
                               const char *title, const char *message,
                               const char *ok_label, const char *cancel_label,
                               deck_sdi_bridge_ui_cb_t on_ok,
                               deck_sdi_bridge_ui_cb_t on_cancel,
                               void *user_data);
} deck_sdi_bridge_ui_vtable_t;

/* Skeleton implementation — accepts snapshots, logs byte count, no
 * actual decode/render yet. Suitable for wiring tests and DVC encoder
 * round-trip validation in F26.1–F26.2. */
deck_sdi_err_t deck_sdi_bridge_ui_register_skeleton(void);

/* High-level wrappers. */
deck_sdi_err_t deck_sdi_bridge_ui_init(void);
deck_sdi_err_t deck_sdi_bridge_ui_push_snapshot(const void *bytes, size_t len);
deck_sdi_err_t deck_sdi_bridge_ui_clear(void);
deck_sdi_err_t deck_sdi_bridge_ui_confirm(const char *title, const char *message,
                                           const char *ok_label,
                                           const char *cancel_label,
                                           deck_sdi_bridge_ui_cb_t on_ok,
                                           deck_sdi_bridge_ui_cb_t on_cancel,
                                           void *user_data);

/* Selftest: init + push 0-length snapshot returns INVALID_ARG;
 * push small dummy buffer returns OK. */
deck_sdi_err_t deck_sdi_bridge_ui_selftest(void);

#ifdef __cplusplus
}
#endif
