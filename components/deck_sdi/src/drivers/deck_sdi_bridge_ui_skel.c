#include "drivers/deck_sdi_bridge_ui.h"
#include "deck_sdi_registry.h"

#include "esp_log.h"

static const char *TAG = "sdi.bridge_ui";

static bool s_inited = false;

static deck_sdi_err_t bui_init_impl(void *ctx)
{
    (void)ctx;
    s_inited = true;
    ESP_LOGI(TAG, "skeleton init OK (no widgets — F26 wires LVGL)");
    return DECK_SDI_OK;
}

static deck_sdi_err_t bui_push_snapshot_impl(void *ctx,
                                              const void *bytes, size_t len)
{
    (void)ctx;
    if (!s_inited) return DECK_SDI_ERR_FAIL;
    if (!bytes || len == 0) return DECK_SDI_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "snapshot received: %u bytes (skeleton — discarded)",
             (unsigned)len);
    return DECK_SDI_OK;
}

static deck_sdi_err_t bui_clear_impl(void *ctx)
{
    (void)ctx;
    if (!s_inited) return DECK_SDI_ERR_FAIL;
    ESP_LOGI(TAG, "clear (skeleton no-op)");
    return DECK_SDI_OK;
}

static const deck_sdi_bridge_ui_vtable_t s_vtable = {
    .init          = bui_init_impl,
    .push_snapshot = bui_push_snapshot_impl,
    .clear         = bui_clear_impl,
};

deck_sdi_err_t deck_sdi_bridge_ui_register_skeleton(void)
{
    const deck_sdi_driver_t driver = {
        .name    = "bridge.ui",
        .id      = DECK_SDI_DRIVER_BRIDGE_UI,
        .version = "0.1.0",       /* skeleton — bumps to 1.0.0 in F26 */
        .vtable  = &s_vtable,
        .ctx     = NULL,
    };
    return deck_sdi_register(&driver);
}

/* ---------- wrappers ---------- */

static const deck_sdi_bridge_ui_vtable_t *bui_vt(void **ctx_out)
{
    const deck_sdi_driver_t *d = deck_sdi_lookup(DECK_SDI_DRIVER_BRIDGE_UI);
    if (!d) return NULL;
    if (ctx_out) *ctx_out = d->ctx;
    return (const deck_sdi_bridge_ui_vtable_t *)d->vtable;
}

deck_sdi_err_t deck_sdi_bridge_ui_init(void)
{ void *c; const deck_sdi_bridge_ui_vtable_t *v = bui_vt(&c);
  return v ? v->init(c) : DECK_SDI_ERR_NOT_FOUND; }

deck_sdi_err_t deck_sdi_bridge_ui_push_snapshot(const void *bytes, size_t len)
{ void *c; const deck_sdi_bridge_ui_vtable_t *v = bui_vt(&c);
  return v ? v->push_snapshot(c, bytes, len) : DECK_SDI_ERR_NOT_FOUND; }

deck_sdi_err_t deck_sdi_bridge_ui_clear(void)
{ void *c; const deck_sdi_bridge_ui_vtable_t *v = bui_vt(&c);
  return v ? v->clear(c) : DECK_SDI_ERR_NOT_FOUND; }

deck_sdi_err_t deck_sdi_bridge_ui_confirm(const char *title, const char *message,
                                           const char *ok_label,
                                           const char *cancel_label,
                                           deck_sdi_bridge_ui_cb_t on_ok,
                                           deck_sdi_bridge_ui_cb_t on_cancel,
                                           void *user_data)
{
    void *c; const deck_sdi_bridge_ui_vtable_t *v = bui_vt(&c);
    if (!v || !v->confirm) return DECK_SDI_ERR_NOT_FOUND;
    return v->confirm(c, title, message, ok_label, cancel_label,
                      on_ok, on_cancel, user_data);
}

/* ---------- selftest ---------- */

deck_sdi_err_t deck_sdi_bridge_ui_selftest(void)
{
    deck_sdi_err_t r = deck_sdi_bridge_ui_init();
    if (r != DECK_SDI_OK) {
        ESP_LOGE(TAG, "init: %s", deck_sdi_strerror(r));
        return r;
    }

    /* 0-length snapshot → INVALID_ARG. */
    r = deck_sdi_bridge_ui_push_snapshot(NULL, 0);
    if (r != DECK_SDI_ERR_INVALID_ARG) {
        ESP_LOGE(TAG, "expected INVALID_ARG on 0-len, got %s", deck_sdi_strerror(r));
        return DECK_SDI_ERR_FAIL;
    }

    /* Small dummy buffer → OK. */
    const uint8_t dummy[4] = { 0xDE, 0xCA, 0xDC, 0x01 };
    r = deck_sdi_bridge_ui_push_snapshot(dummy, sizeof(dummy));
    if (r != DECK_SDI_OK) {
        ESP_LOGE(TAG, "push_snapshot: %s", deck_sdi_strerror(r));
        return r;
    }

    r = deck_sdi_bridge_ui_clear();
    if (r != DECK_SDI_OK) {
        ESP_LOGE(TAG, "clear: %s", deck_sdi_strerror(r));
        return r;
    }

    ESP_LOGI(TAG, "selftest: PASS (skeleton init + push + clear)");
    return DECK_SDI_OK;
}
