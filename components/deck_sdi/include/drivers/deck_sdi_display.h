#pragma once

/* display.panel — display panel + backlight.
 *
 * DL2 driver. Wraps the board LCD initialization and backlight gate
 * behind a platform-neutral vtable. The underlying frame buffer is
 * managed by the platform UI layer (LVGL on the reference board) — the
 * SDI driver does not expose pixel-level access at DL2.
 *
 * Width/height are queried after init; both stay constant across the
 * device lifetime (rotation is a UI concern handled higher up).
 */

#include "deck_sdi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Bring up the LCD panel (and the shared touch I2C bus on the
     * reference board). Idempotent. */
    deck_sdi_err_t (*init)(void *ctx);

    /* Backlight control. */
    deck_sdi_err_t (*set_backlight)(void *ctx, bool on);

    /* Native panel dimensions (no rotation applied). */
    uint16_t (*width)(void *ctx);
    uint16_t (*height)(void *ctx);
} deck_sdi_display_vtable_t;

deck_sdi_err_t deck_sdi_display_register_esp32(void);

deck_sdi_err_t deck_sdi_display_init(void);
deck_sdi_err_t deck_sdi_display_set_backlight(bool on);
uint16_t       deck_sdi_display_width(void);
uint16_t       deck_sdi_display_height(void);

/* Selftest: init + width/height plausible (>0). */
deck_sdi_err_t deck_sdi_display_selftest(void);

#ifdef __cplusplus
}
#endif
