#include "drivers/deck_sdi_battery.h"
#include "deck_sdi_registry.h"

#include "hal_battery.h"
#include "esp_log.h"

static const char *TAG = "sdi.battery";

static bool    s_inited        = false;
static uint8_t s_low_threshold = 15; /* default: warn at 15% */

static deck_sdi_err_t bat_init_impl(void *ctx)
{
    (void)ctx;
    if (s_inited) return DECK_SDI_OK;
    if (hal_battery_init() != ESP_OK) return DECK_SDI_ERR_IO;
    s_inited = true;
    return DECK_SDI_OK;
}

static deck_sdi_err_t bat_read_mv_impl(void *ctx, uint32_t *out_mv)
{
    (void)ctx;
    if (!out_mv) return DECK_SDI_ERR_INVALID_ARG;
    if (!s_inited) return DECK_SDI_ERR_FAIL;
    return hal_battery_read_mv(out_mv) == ESP_OK ? DECK_SDI_OK : DECK_SDI_ERR_IO;
}

static deck_sdi_err_t bat_read_pct_impl(void *ctx, uint8_t *out_pct)
{
    (void)ctx;
    if (!out_pct) return DECK_SDI_ERR_INVALID_ARG;
    if (!s_inited) return DECK_SDI_ERR_FAIL;
    return hal_battery_read_pct(out_pct) == ESP_OK ? DECK_SDI_OK : DECK_SDI_ERR_IO;
}

static bool bat_is_charging_impl(void *ctx)
{
    (void)ctx;
    /* No charger-status pin on the reference board. */
    return false;
}

static deck_sdi_err_t bat_set_low_impl(void *ctx, uint8_t pct)
{
    (void)ctx;
    if (pct > 100) return DECK_SDI_ERR_INVALID_ARG;
    s_low_threshold = pct;
    return DECK_SDI_OK;
}

static uint8_t bat_get_low_impl(void *ctx)
{
    (void)ctx;
    return s_low_threshold;
}

static const deck_sdi_battery_vtable_t s_vtable = {
    .init               = bat_init_impl,
    .read_mv            = bat_read_mv_impl,
    .read_pct           = bat_read_pct_impl,
    .is_charging        = bat_is_charging_impl,
    .set_low_threshold  = bat_set_low_impl,
    .get_low_threshold  = bat_get_low_impl,
};

deck_sdi_err_t deck_sdi_battery_register_esp32(void)
{
    const deck_sdi_driver_t driver = {
        .name    = "system.battery",
        .id      = DECK_SDI_DRIVER_BATTERY,
        .version = "1.0.0",
        .vtable  = &s_vtable,
        .ctx     = NULL,
    };
    return deck_sdi_register(&driver);
}

/* ---------- wrappers ---------- */

static const deck_sdi_battery_vtable_t *bat_vt(void **ctx_out)
{
    const deck_sdi_driver_t *d = deck_sdi_lookup(DECK_SDI_DRIVER_BATTERY);
    if (!d) return NULL;
    if (ctx_out) *ctx_out = d->ctx;
    return (const deck_sdi_battery_vtable_t *)d->vtable;
}

deck_sdi_err_t deck_sdi_battery_init(void)
{ void *c; const deck_sdi_battery_vtable_t *v = bat_vt(&c);
  return v ? v->init(c) : DECK_SDI_ERR_NOT_FOUND; }

deck_sdi_err_t deck_sdi_battery_read_mv(uint32_t *out_mv)
{ void *c; const deck_sdi_battery_vtable_t *v = bat_vt(&c);
  return v ? v->read_mv(c, out_mv) : DECK_SDI_ERR_NOT_FOUND; }

deck_sdi_err_t deck_sdi_battery_read_pct(uint8_t *out_pct)
{ void *c; const deck_sdi_battery_vtable_t *v = bat_vt(&c);
  return v ? v->read_pct(c, out_pct) : DECK_SDI_ERR_NOT_FOUND; }

bool deck_sdi_battery_is_charging(void)
{ void *c; const deck_sdi_battery_vtable_t *v = bat_vt(&c);
  return v ? v->is_charging(c) : false; }

deck_sdi_err_t deck_sdi_battery_set_low_threshold(uint8_t pct)
{ void *c; const deck_sdi_battery_vtable_t *v = bat_vt(&c);
  return v ? v->set_low_threshold(c, pct) : DECK_SDI_ERR_NOT_FOUND; }

uint8_t deck_sdi_battery_get_low_threshold(void)
{ void *c; const deck_sdi_battery_vtable_t *v = bat_vt(&c);
  return v ? v->get_low_threshold(c) : 0; }

/* ---------- selftest ---------- */

deck_sdi_err_t deck_sdi_battery_selftest(void)
{
    deck_sdi_err_t r = deck_sdi_battery_init();
    if (r != DECK_SDI_OK) {
        ESP_LOGE(TAG, "init failed: %s", deck_sdi_strerror(r));
        return r;
    }

    uint32_t mv = 0;
    r = deck_sdi_battery_read_mv(&mv);
    if (r != DECK_SDI_OK) {
        ESP_LOGE(TAG, "read_mv: %s", deck_sdi_strerror(r));
        return r;
    }
    /* Reference range: 0..6000 mV. Anything else is suspicious. */
    if (mv > 6000) {
        ESP_LOGW(TAG, "implausible battery mv: %u", (unsigned)mv);
    }

    uint8_t pct = 0;
    r = deck_sdi_battery_read_pct(&pct);
    if (r != DECK_SDI_OK || pct > 100) {
        ESP_LOGE(TAG, "read_pct: r=%s pct=%u", deck_sdi_strerror(r), pct);
        return DECK_SDI_ERR_FAIL;
    }

    /* Threshold round-trip. */
    r = deck_sdi_battery_set_low_threshold(20);
    if (r != DECK_SDI_OK) return r;
    if (deck_sdi_battery_get_low_threshold() != 20) {
        ESP_LOGE(TAG, "low threshold not persisted");
        return DECK_SDI_ERR_FAIL;
    }
    /* Restore default. */
    (void)deck_sdi_battery_set_low_threshold(15);

    ESP_LOGI(TAG, "selftest: PASS (%u mV / %u%%)", (unsigned)mv, pct);
    return DECK_SDI_OK;
}
