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

/* Core resolution services — NULL slot → NOT_SUPPORTED. */

#define BUI_CALL0(slot)                                                   \
    void *c; const deck_sdi_bridge_ui_vtable_t *v = bui_vt(&c);            \
    if (!v) return DECK_SDI_ERR_NOT_FOUND;                                 \
    if (!v->slot) return DECK_SDI_ERR_NOT_FOUND;

deck_sdi_err_t deck_sdi_bridge_ui_toast(const char *text, uint32_t duration_ms)
{ BUI_CALL0(toast); return v->toast(c, text, duration_ms); }

deck_sdi_err_t deck_sdi_bridge_ui_confirm(const char *title, const char *message,
                                           const char *ok_label,
                                           const char *cancel_label,
                                           deck_sdi_bridge_ui_cb_t on_ok,
                                           deck_sdi_bridge_ui_cb_t on_cancel,
                                           void *user_data)
{ BUI_CALL0(confirm);
  return v->confirm(c, title, message, ok_label, cancel_label,
                    on_ok, on_cancel, user_data); }

deck_sdi_err_t deck_sdi_bridge_ui_loading_show(const char *label)
{ BUI_CALL0(loading_show); return v->loading_show(c, label); }

deck_sdi_err_t deck_sdi_bridge_ui_loading_hide(void)
{ BUI_CALL0(loading_hide); return v->loading_hide(c); }

deck_sdi_err_t deck_sdi_bridge_ui_progress_show(const char *label)
{ BUI_CALL0(progress_show); return v->progress_show(c, label); }

deck_sdi_err_t deck_sdi_bridge_ui_progress_set(float pct)
{ BUI_CALL0(progress_set); return v->progress_set(c, pct); }

deck_sdi_err_t deck_sdi_bridge_ui_progress_hide(void)
{ BUI_CALL0(progress_hide); return v->progress_hide(c); }

deck_sdi_err_t deck_sdi_bridge_ui_choice_show(const char *title,
                                               const char *const *options,
                                               uint16_t n_options,
                                               deck_sdi_bridge_ui_choice_cb_t on_pick,
                                               void *user_data)
{ BUI_CALL0(choice_show);
  return v->choice_show(c, title, options, n_options, on_pick, user_data); }

deck_sdi_err_t deck_sdi_bridge_ui_multiselect_show(const char *title,
                                                    const char *const *options,
                                                    uint16_t n_options,
                                                    const bool *initially_selected,
                                                    deck_sdi_bridge_ui_cb_t on_done,
                                                    void *user_data)
{ BUI_CALL0(multiselect_show);
  return v->multiselect_show(c, title, options, n_options,
                              initially_selected, on_done, user_data); }

deck_sdi_err_t deck_sdi_bridge_ui_date_show(const char *title,
                                             int64_t initial_epoch_ms,
                                             deck_sdi_bridge_ui_cb_t on_pick,
                                             void *user_data)
{ BUI_CALL0(date_show);
  return v->date_show(c, title, initial_epoch_ms, on_pick, user_data); }

deck_sdi_err_t deck_sdi_bridge_ui_share_show(const char *text, const char *url)
{ BUI_CALL0(share_show); return v->share_show(c, text, url); }

deck_sdi_err_t deck_sdi_bridge_ui_permission_show(const char *permission_name,
                                                   const char *rationale,
                                                   deck_sdi_bridge_ui_cb_t on_grant,
                                                   deck_sdi_bridge_ui_cb_t on_deny,
                                                   void *user_data)
{ BUI_CALL0(permission_show);
  return v->permission_show(c, permission_name, rationale,
                             on_grant, on_deny, user_data); }

deck_sdi_err_t deck_sdi_bridge_ui_set_locked(bool locked)
{ BUI_CALL0(set_locked); return v->set_locked(c, locked); }

deck_sdi_err_t deck_sdi_bridge_ui_set_theme(const char *theme_atom)
{ BUI_CALL0(set_theme); return v->set_theme(c, theme_atom); }

/* Visual band. */

deck_sdi_err_t deck_sdi_bridge_ui_keyboard_show(const char *kind_atom)
{ BUI_CALL0(keyboard_show); return v->keyboard_show(c, kind_atom); }

deck_sdi_err_t deck_sdi_bridge_ui_keyboard_hide(void)
{ BUI_CALL0(keyboard_hide); return v->keyboard_hide(c); }

deck_sdi_err_t deck_sdi_bridge_ui_set_statusbar(bool visible)
{ BUI_CALL0(set_statusbar); return v->set_statusbar(c, visible); }

deck_sdi_err_t deck_sdi_bridge_ui_set_navbar(bool visible)
{ BUI_CALL0(set_navbar); return v->set_navbar(c, visible); }

deck_sdi_err_t deck_sdi_bridge_ui_set_badge(const char *app_id, int count)
{ BUI_CALL0(set_badge); return v->set_badge(c, app_id, count); }

/* Physical-display band. */

deck_sdi_err_t deck_sdi_bridge_ui_set_rotation(int rot)
{ BUI_CALL0(set_rotation); return v->set_rotation(c, rot); }

deck_sdi_err_t deck_sdi_bridge_ui_set_brightness(float level)
{ BUI_CALL0(set_brightness); return v->set_brightness(c, level); }

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
