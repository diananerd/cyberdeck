#include "drivers/deck_sdi_shell.h"
#include "deck_sdi_registry.h"

#include "esp_log.h"

static const char *TAG = "sdi.shell";

/* Stub state: no app ever actually loads at F1.6. The loader + eval
 * pipeline needed to service launch() is implemented F2–F7; the real
 * shell lands in F8 and replaces this driver. */

static deck_sdi_err_t shell_launch(void *ctx, const char *app_id)
{
    (void)ctx;
    if (!app_id) return DECK_SDI_ERR_INVALID_ARG;
    ESP_LOGW(TAG, "launch(%s) requested — stub, returning not_supported", app_id);
    return DECK_SDI_ERR_NOT_SUPPORTED;
}

static deck_sdi_err_t shell_terminate(void *ctx)
{
    (void)ctx;
    return DECK_SDI_OK;
}

static const char *shell_current_app_id(void *ctx)
{
    (void)ctx;
    return NULL;
}

static bool shell_is_running(void *ctx)
{
    (void)ctx;
    return false;
}

static const deck_sdi_shell_vtable_t s_vtable = {
    .launch         = shell_launch,
    .terminate      = shell_terminate,
    .current_app_id = shell_current_app_id,
    .is_running     = shell_is_running,
};

deck_sdi_err_t deck_sdi_shell_register_stub(void)
{
    const deck_sdi_driver_t driver = {
        .name    = "system.shell",
        .id      = DECK_SDI_DRIVER_SHELL,
        .version = "1.0.0-stub",
        .vtable  = &s_vtable,
        .ctx     = NULL,
    };
    return deck_sdi_register(&driver);
}

/* ---------- wrappers ---------- */

static const deck_sdi_shell_vtable_t *shell_vt(void **ctx_out)
{
    const deck_sdi_driver_t *d = deck_sdi_lookup(DECK_SDI_DRIVER_SHELL);
    if (!d) return NULL;
    if (ctx_out) *ctx_out = d->ctx;
    return (const deck_sdi_shell_vtable_t *)d->vtable;
}

deck_sdi_err_t deck_sdi_shell_launch(const char *app_id)
{ void *c; const deck_sdi_shell_vtable_t *v = shell_vt(&c); return v ? v->launch(c, app_id) : DECK_SDI_ERR_NOT_FOUND; }
deck_sdi_err_t deck_sdi_shell_terminate(void)
{ void *c; const deck_sdi_shell_vtable_t *v = shell_vt(&c); return v ? v->terminate(c) : DECK_SDI_ERR_NOT_FOUND; }
const char *deck_sdi_shell_current_app_id(void)
{ void *c; const deck_sdi_shell_vtable_t *v = shell_vt(&c); return v ? v->current_app_id(c) : NULL; }
bool deck_sdi_shell_is_running(void)
{ void *c; const deck_sdi_shell_vtable_t *v = shell_vt(&c); return v ? v->is_running(c) : false; }

/* ---------- selftest ---------- */

deck_sdi_err_t deck_sdi_shell_selftest(void)
{
    if (deck_sdi_shell_is_running()) {
        ESP_LOGE(TAG, "is_running=true before any launch");
        return DECK_SDI_ERR_FAIL;
    }
    if (deck_sdi_shell_current_app_id() != NULL) {
        ESP_LOGE(TAG, "current_app_id non-null before any launch");
        return DECK_SDI_ERR_FAIL;
    }
    deck_sdi_err_t r = deck_sdi_shell_launch("sys.nope");
    if (r != DECK_SDI_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "expected not_supported from stub, got %s",
                 deck_sdi_strerror(r));
        return DECK_SDI_ERR_FAIL;
    }
    r = deck_sdi_shell_terminate();
    if (r != DECK_SDI_OK) {
        ESP_LOGE(TAG, "terminate stub should be OK, got %s",
                 deck_sdi_strerror(r));
        return DECK_SDI_ERR_FAIL;
    }

    ESP_LOGI(TAG, "selftest: PASS (stub — launch→not_supported, terminate→ok)");
    return DECK_SDI_OK;
}
