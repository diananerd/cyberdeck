#pragma once

/* system.battery — battery telemetry.
 *
 * DL2 driver. Wraps the board HAL ADC behind a platform-neutral vtable.
 * Provides voltage (mV) + estimated charge percent. Low-battery events
 * are surfaced through a polling threshold; the runtime adapter posts
 * them to the event bus as `system.low_battery`.
 *
 * No charging-state detection at DL2 (no charger IC on the reference
 * board) — `is_charging()` returns a constant false.
 */

#include "deck_sdi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    deck_sdi_err_t (*init)(void *ctx);
    deck_sdi_err_t (*read_mv)(void *ctx, uint32_t *out_mv);
    deck_sdi_err_t (*read_pct)(void *ctx, uint8_t  *out_pct);
    bool           (*is_charging)(void *ctx);

    /* Threshold (percent) below which read_pct callers should treat
     * the battery as low. 0 disables. */
    deck_sdi_err_t (*set_low_threshold)(void *ctx, uint8_t pct);
    uint8_t        (*get_low_threshold)(void *ctx);
} deck_sdi_battery_vtable_t;

deck_sdi_err_t deck_sdi_battery_register_esp32(void);

/* High-level wrappers. */
deck_sdi_err_t deck_sdi_battery_init(void);
deck_sdi_err_t deck_sdi_battery_read_mv(uint32_t *out_mv);
deck_sdi_err_t deck_sdi_battery_read_pct(uint8_t  *out_pct);
bool           deck_sdi_battery_is_charging(void);
deck_sdi_err_t deck_sdi_battery_set_low_threshold(uint8_t pct);
uint8_t        deck_sdi_battery_get_low_threshold(void);

/* Selftest: init + one ADC read in plausible range (0..100 pct). */
deck_sdi_err_t deck_sdi_battery_selftest(void);

#ifdef __cplusplus
}
#endif
