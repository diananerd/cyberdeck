/* display.panel + display.touch — ESP32-S3 (Waveshare 4.3" RGB LCD + GT911).
 *
 * Both drivers share the same hal_lcd_init call, which initializes both
 * the LCD panel and the GT911 touch controller. We register two SDI
 * driver entries (display.panel, display.touch) but back them with one
 * shared state struct so init() is idempotent across both.
 */

#include "drivers/deck_sdi_display.h"
#include "drivers/deck_sdi_touch.h"
#include "deck_sdi_registry.h"

#include "hal_lcd.h"
#include "hal_backlight.h"
#include "hal_ch422g.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "sdi.display";

#define PANEL_NATIVE_W   800
#define PANEL_NATIVE_H   480

static bool                       s_inited     = false;
static esp_lcd_panel_handle_t     s_panel      = NULL;
static esp_lcd_touch_handle_t     s_touch      = NULL;

static deck_sdi_err_t shared_init(void)
{
    if (s_inited) return DECK_SDI_OK;
    /* Bring up the shared I2C bus (CH422G expander, GT911 touch, RTC). */
    if (hal_ch422g_init() != ESP_OK) {
        ESP_LOGE(TAG, "hal_ch422g_init failed");
        return DECK_SDI_ERR_IO;
    }
    if (hal_lcd_init(&s_panel, &s_touch) != ESP_OK) {
        ESP_LOGE(TAG, "hal_lcd_init failed");
        return DECK_SDI_ERR_IO;
    }
    /* Backlight comes up after touch reset (CH422G OUT register
     * is overwritten by the touch reset sequence). */
    if (hal_backlight_on() != ESP_OK) {
        ESP_LOGW(TAG, "backlight enable failed (continuing)");
    }
    s_inited = true;
    ESP_LOGI(TAG, "panel %dx%d + touch initialized",
             PANEL_NATIVE_W, PANEL_NATIVE_H);
    return DECK_SDI_OK;
}

/* ============================================================ */
/*                      display.panel                            */
/* ============================================================ */

static deck_sdi_err_t disp_init_impl(void *ctx)
{ (void)ctx; return shared_init(); }

static deck_sdi_err_t disp_set_backlight_impl(void *ctx, bool on)
{
    (void)ctx;
    if (!s_inited) return DECK_SDI_ERR_FAIL;
    return hal_backlight_set(on) == ESP_OK
            ? DECK_SDI_OK : DECK_SDI_ERR_IO;
}

static uint16_t disp_width_impl(void *ctx)
{ (void)ctx; return PANEL_NATIVE_W; }

static uint16_t disp_height_impl(void *ctx)
{ (void)ctx; return PANEL_NATIVE_H; }

static const deck_sdi_display_vtable_t s_display_vtable = {
    .init           = disp_init_impl,
    .set_backlight  = disp_set_backlight_impl,
    .width          = disp_width_impl,
    .height         = disp_height_impl,
};

deck_sdi_err_t deck_sdi_display_register_esp32(void)
{
    const deck_sdi_driver_t driver = {
        .name    = "display.panel",
        .id      = DECK_SDI_DRIVER_DISPLAY,
        .version = "1.0.0",
        .vtable  = &s_display_vtable,
        .ctx     = NULL,
    };
    return deck_sdi_register(&driver);
}

/* ---------- display.panel wrappers ---------- */

static const deck_sdi_display_vtable_t *disp_vt(void **ctx_out)
{
    const deck_sdi_driver_t *d = deck_sdi_lookup(DECK_SDI_DRIVER_DISPLAY);
    if (!d) return NULL;
    if (ctx_out) *ctx_out = d->ctx;
    return (const deck_sdi_display_vtable_t *)d->vtable;
}

deck_sdi_err_t deck_sdi_display_init(void)
{ void *c; const deck_sdi_display_vtable_t *v = disp_vt(&c);
  return v ? v->init(c) : DECK_SDI_ERR_NOT_FOUND; }

deck_sdi_err_t deck_sdi_display_set_backlight(bool on)
{ void *c; const deck_sdi_display_vtable_t *v = disp_vt(&c);
  return v ? v->set_backlight(c, on) : DECK_SDI_ERR_NOT_FOUND; }

uint16_t deck_sdi_display_width(void)
{ void *c; const deck_sdi_display_vtable_t *v = disp_vt(&c);
  return v ? v->width(c) : 0; }

uint16_t deck_sdi_display_height(void)
{ void *c; const deck_sdi_display_vtable_t *v = disp_vt(&c);
  return v ? v->height(c) : 0; }

deck_sdi_err_t deck_sdi_display_selftest(void)
{
    deck_sdi_err_t r = deck_sdi_display_init();
    if (r != DECK_SDI_OK) {
        ESP_LOGE(TAG, "init: %s", deck_sdi_strerror(r));
        return r;
    }
    uint16_t w = deck_sdi_display_width();
    uint16_t h = deck_sdi_display_height();
    if (w == 0 || h == 0) {
        ESP_LOGE(TAG, "implausible dims %ux%u", w, h);
        return DECK_SDI_ERR_FAIL;
    }
    ESP_LOGI(TAG, "selftest: PASS (%ux%u panel)", w, h);
    return DECK_SDI_OK;
}

/* ============================================================ */
/*                      display.touch                            */
/* ============================================================ */

static deck_sdi_err_t touch_init_impl(void *ctx)
{ (void)ctx; return shared_init(); }

static deck_sdi_err_t touch_read_impl(void *ctx, bool *pressed,
                                       uint16_t *x, uint16_t *y)
{
    (void)ctx;
    if (!pressed) return DECK_SDI_ERR_INVALID_ARG;
    if (!s_inited || !s_touch) {
        *pressed = false;
        return DECK_SDI_ERR_FAIL;
    }

    /* Refresh internal cache. */
    esp_lcd_touch_read_data(s_touch);

    esp_lcd_touch_point_data_t pt = {0};
    uint8_t count = 0;
    esp_err_t e = esp_lcd_touch_get_data(s_touch, &pt, &count, 1);
    if (e != ESP_OK) {
        *pressed = false;
        if (x) *x = 0;
        if (y) *y = 0;
        return DECK_SDI_ERR_IO;
    }
    if (count > 0) {
        *pressed = true;
        if (x) *x = pt.x;
        if (y) *y = pt.y;
    } else {
        *pressed = false;
        if (x) *x = 0;
        if (y) *y = 0;
    }
    return DECK_SDI_OK;
}

static const deck_sdi_touch_vtable_t s_touch_vtable = {
    .init = touch_init_impl,
    .read = touch_read_impl,
};

deck_sdi_err_t deck_sdi_touch_register_esp32(void)
{
    const deck_sdi_driver_t driver = {
        .name    = "display.touch",
        .id      = DECK_SDI_DRIVER_TOUCH,
        .version = "1.0.0",
        .vtable  = &s_touch_vtable,
        .ctx     = NULL,
    };
    return deck_sdi_register(&driver);
}

/* ---------- display.touch wrappers ---------- */

static const deck_sdi_touch_vtable_t *touch_vt(void **ctx_out)
{
    const deck_sdi_driver_t *d = deck_sdi_lookup(DECK_SDI_DRIVER_TOUCH);
    if (!d) return NULL;
    if (ctx_out) *ctx_out = d->ctx;
    return (const deck_sdi_touch_vtable_t *)d->vtable;
}

deck_sdi_err_t deck_sdi_touch_init(void)
{ void *c; const deck_sdi_touch_vtable_t *v = touch_vt(&c);
  return v ? v->init(c) : DECK_SDI_ERR_NOT_FOUND; }

deck_sdi_err_t deck_sdi_touch_read(bool *pressed, uint16_t *x, uint16_t *y)
{ void *c; const deck_sdi_touch_vtable_t *v = touch_vt(&c);
  return v ? v->read(c, pressed, x, y) : DECK_SDI_ERR_NOT_FOUND; }

deck_sdi_err_t deck_sdi_touch_selftest(void)
{
    deck_sdi_err_t r = deck_sdi_touch_init();
    if (r != DECK_SDI_OK) {
        ESP_LOGE(TAG, "init: %s", deck_sdi_strerror(r));
        return r;
    }
    bool pressed = false;
    uint16_t x = 0, y = 0;
    r = deck_sdi_touch_read(&pressed, &x, &y);
    if (r != DECK_SDI_OK) {
        ESP_LOGE(TAG, "read: %s", deck_sdi_strerror(r));
        return r;
    }
    ESP_LOGI(TAG, "selftest: PASS (read OK, pressed=%d at %u,%u)",
             (int)pressed, x, y);
    return DECK_SDI_OK;
}
