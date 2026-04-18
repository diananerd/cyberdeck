#pragma once

/* display.touch — touch input.
 *
 * DL2 driver. Wraps the GT911 capacitive controller (or whatever the
 * platform exposes) behind a polling read API. Native (un-rotated)
 * coordinates — rotation is the UI layer's concern.
 *
 * Init for the reference board is shared with display.panel (single
 * I2C bus, single hal_lcd_init call). Calling display.touch.init()
 * after display.panel.init() is a no-op.
 */

#include "deck_sdi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    deck_sdi_err_t (*init)(void *ctx);

    /* Poll one sample. *pressed=true with (x,y) populated if the
     * surface is currently touched, *pressed=false otherwise. */
    deck_sdi_err_t (*read)(void *ctx, bool *pressed,
                           uint16_t *x, uint16_t *y);
} deck_sdi_touch_vtable_t;

deck_sdi_err_t deck_sdi_touch_register_esp32(void);

deck_sdi_err_t deck_sdi_touch_init(void);
deck_sdi_err_t deck_sdi_touch_read(bool *pressed, uint16_t *x, uint16_t *y);

/* Selftest: init + one read (no requirement on press state). */
deck_sdi_err_t deck_sdi_touch_selftest(void);

#ifdef __cplusplus
}
#endif
