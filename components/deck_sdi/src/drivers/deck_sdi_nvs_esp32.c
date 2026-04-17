#include "drivers/deck_sdi_nvs.h"
#include "deck_sdi_registry.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "sdi.nvs";

static deck_sdi_err_t map_esp(esp_err_t e)
{
    switch (e) {
        case ESP_OK:                          return DECK_SDI_OK;
        case ESP_ERR_NVS_NOT_FOUND:           return DECK_SDI_ERR_NOT_FOUND;
        case ESP_ERR_NVS_INVALID_NAME:
        case ESP_ERR_NVS_INVALID_HANDLE:
        case ESP_ERR_NVS_INVALID_LENGTH:
        case ESP_ERR_INVALID_ARG:             return DECK_SDI_ERR_INVALID_ARG;
        case ESP_ERR_NVS_NOT_INITIALIZED:     return DECK_SDI_ERR_NOT_SUPPORTED;
        case ESP_ERR_NO_MEM:                  return DECK_SDI_ERR_NO_MEMORY;
        default:                              return DECK_SDI_ERR_IO;
    }
}

static deck_sdi_err_t nvs_get_str_impl(void *ctx, const char *ns,
                                        const char *key, char *out, size_t out_size)
{
    (void)ctx;
    if (!ns || !key || !out || out_size == 0) return DECK_SDI_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open(ns, NVS_READONLY, &h);
    if (e != ESP_OK) return map_esp(e);
    size_t len = out_size;
    e = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return map_esp(e);
}

static deck_sdi_err_t nvs_set_str_impl(void *ctx, const char *ns,
                                        const char *key, const char *value)
{
    (void)ctx;
    if (!ns || !key || !value) return DECK_SDI_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open(ns, NVS_READWRITE, &h);
    if (e != ESP_OK) return map_esp(e);
    e = nvs_set_str(h, key, value);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return map_esp(e);
}

static deck_sdi_err_t nvs_get_i64_impl(void *ctx, const char *ns,
                                        const char *key, int64_t *out)
{
    (void)ctx;
    if (!ns || !key || !out) return DECK_SDI_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open(ns, NVS_READONLY, &h);
    if (e != ESP_OK) return map_esp(e);
    e = nvs_get_i64(h, key, out);
    nvs_close(h);
    return map_esp(e);
}

static deck_sdi_err_t nvs_set_i64_impl(void *ctx, const char *ns,
                                        const char *key, int64_t value)
{
    (void)ctx;
    if (!ns || !key) return DECK_SDI_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open(ns, NVS_READWRITE, &h);
    if (e != ESP_OK) return map_esp(e);
    e = nvs_set_i64(h, key, value);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return map_esp(e);
}

static deck_sdi_err_t nvs_get_blob_impl(void *ctx, const char *ns,
                                         const char *key, void *out, size_t *io_size)
{
    (void)ctx;
    if (!ns || !key || !out || !io_size) return DECK_SDI_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open(ns, NVS_READONLY, &h);
    if (e != ESP_OK) return map_esp(e);
    e = nvs_get_blob(h, key, out, io_size);
    nvs_close(h);
    return map_esp(e);
}

static deck_sdi_err_t nvs_set_blob_impl(void *ctx, const char *ns,
                                         const char *key, const void *buf, size_t size)
{
    (void)ctx;
    if (!ns || !key || (!buf && size > 0)) return DECK_SDI_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open(ns, NVS_READWRITE, &h);
    if (e != ESP_OK) return map_esp(e);
    e = nvs_set_blob(h, key, buf, size);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return map_esp(e);
}

static deck_sdi_err_t nvs_del_impl(void *ctx, const char *ns, const char *key)
{
    (void)ctx;
    if (!ns || !key) return DECK_SDI_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open(ns, NVS_READWRITE, &h);
    if (e != ESP_OK) return map_esp(e);
    e = nvs_erase_key(h, key);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return map_esp(e);
}

static deck_sdi_err_t nvs_commit_impl(void *ctx, const char *ns)
{
    (void)ctx;
    if (!ns) return DECK_SDI_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open(ns, NVS_READWRITE, &h);
    if (e != ESP_OK) return map_esp(e);
    e = nvs_commit(h);
    nvs_close(h);
    return map_esp(e);
}

static const deck_sdi_nvs_vtable_t s_vtable = {
    .get_str  = nvs_get_str_impl,
    .set_str  = nvs_set_str_impl,
    .get_i64  = nvs_get_i64_impl,
    .set_i64  = nvs_set_i64_impl,
    .get_blob = nvs_get_blob_impl,
    .set_blob = nvs_set_blob_impl,
    .del      = nvs_del_impl,
    .commit   = nvs_commit_impl,
};

deck_sdi_err_t deck_sdi_nvs_register_esp32(void)
{
    const deck_sdi_driver_t driver = {
        .name    = "storage.nvs",
        .id      = DECK_SDI_DRIVER_NVS,
        .version = "1.0.0",
        .vtable  = &s_vtable,
        .ctx     = NULL,
    };
    return deck_sdi_register(&driver);
}

/* ---------- high-level wrappers ---------- */

static const deck_sdi_nvs_vtable_t *nvs_vt(void **ctx_out)
{
    const deck_sdi_driver_t *d = deck_sdi_lookup(DECK_SDI_DRIVER_NVS);
    if (!d) return NULL;
    if (ctx_out) *ctx_out = d->ctx;
    return (const deck_sdi_nvs_vtable_t *)d->vtable;
}

deck_sdi_err_t deck_sdi_nvs_get_str(const char *ns, const char *key,
                                    char *out, size_t out_size)
{
    void *ctx; const deck_sdi_nvs_vtable_t *vt = nvs_vt(&ctx);
    if (!vt) return DECK_SDI_ERR_NOT_FOUND;
    return vt->get_str(ctx, ns, key, out, out_size);
}

deck_sdi_err_t deck_sdi_nvs_set_str(const char *ns, const char *key,
                                    const char *value)
{
    void *ctx; const deck_sdi_nvs_vtable_t *vt = nvs_vt(&ctx);
    if (!vt) return DECK_SDI_ERR_NOT_FOUND;
    return vt->set_str(ctx, ns, key, value);
}

deck_sdi_err_t deck_sdi_nvs_get_i64(const char *ns, const char *key, int64_t *out)
{
    void *ctx; const deck_sdi_nvs_vtable_t *vt = nvs_vt(&ctx);
    if (!vt) return DECK_SDI_ERR_NOT_FOUND;
    return vt->get_i64(ctx, ns, key, out);
}

deck_sdi_err_t deck_sdi_nvs_set_i64(const char *ns, const char *key, int64_t value)
{
    void *ctx; const deck_sdi_nvs_vtable_t *vt = nvs_vt(&ctx);
    if (!vt) return DECK_SDI_ERR_NOT_FOUND;
    return vt->set_i64(ctx, ns, key, value);
}

deck_sdi_err_t deck_sdi_nvs_del(const char *ns, const char *key)
{
    void *ctx; const deck_sdi_nvs_vtable_t *vt = nvs_vt(&ctx);
    if (!vt) return DECK_SDI_ERR_NOT_FOUND;
    return vt->del(ctx, ns, key);
}

deck_sdi_err_t deck_sdi_nvs_commit(const char *ns)
{
    void *ctx; const deck_sdi_nvs_vtable_t *vt = nvs_vt(&ctx);
    if (!vt) return DECK_SDI_ERR_NOT_FOUND;
    return vt->commit(ctx, ns);
}

/* ---------- selftest ---------- */

#define SELFTEST_NS   "deck.test"
#define SELFTEST_KSTR "s"
#define SELFTEST_KINT "n"

deck_sdi_err_t deck_sdi_nvs_selftest(void)
{
    deck_sdi_err_t r;

    /* Clean start — ignore NOT_FOUND. */
    deck_sdi_nvs_del(SELFTEST_NS, SELFTEST_KSTR);
    deck_sdi_nvs_del(SELFTEST_NS, SELFTEST_KINT);

    /* String round-trip. */
    const char *want_s = "deck-dl1";
    r = deck_sdi_nvs_set_str(SELFTEST_NS, SELFTEST_KSTR, want_s);
    if (r != DECK_SDI_OK) { ESP_LOGE(TAG, "set_str: %s", deck_sdi_strerror(r)); return r; }
    char got_s[32] = {0};
    r = deck_sdi_nvs_get_str(SELFTEST_NS, SELFTEST_KSTR, got_s, sizeof(got_s));
    if (r != DECK_SDI_OK) { ESP_LOGE(TAG, "get_str: %s", deck_sdi_strerror(r)); return r; }
    if (strcmp(got_s, want_s) != 0) {
        ESP_LOGE(TAG, "str mismatch: got %s want %s", got_s, want_s);
        return DECK_SDI_ERR_FAIL;
    }

    /* i64 round-trip. */
    int64_t want_n = 0x1234567890abcdefLL;
    r = deck_sdi_nvs_set_i64(SELFTEST_NS, SELFTEST_KINT, want_n);
    if (r != DECK_SDI_OK) { ESP_LOGE(TAG, "set_i64: %s", deck_sdi_strerror(r)); return r; }
    int64_t got_n = 0;
    r = deck_sdi_nvs_get_i64(SELFTEST_NS, SELFTEST_KINT, &got_n);
    if (r != DECK_SDI_OK) { ESP_LOGE(TAG, "get_i64: %s", deck_sdi_strerror(r)); return r; }
    if (got_n != want_n) {
        ESP_LOGE(TAG, "i64 mismatch: got %lld want %lld",
                 (long long)got_n, (long long)want_n);
        return DECK_SDI_ERR_FAIL;
    }

    /* Delete verifies NOT_FOUND on subsequent get. */
    r = deck_sdi_nvs_del(SELFTEST_NS, SELFTEST_KSTR);
    if (r != DECK_SDI_OK) { ESP_LOGE(TAG, "del: %s", deck_sdi_strerror(r)); return r; }
    r = deck_sdi_nvs_get_str(SELFTEST_NS, SELFTEST_KSTR, got_s, sizeof(got_s));
    if (r != DECK_SDI_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "expected not_found after del, got %s",
                 deck_sdi_strerror(r));
        return DECK_SDI_ERR_FAIL;
    }

    /* Leave no residue. */
    deck_sdi_nvs_del(SELFTEST_NS, SELFTEST_KINT);

    ESP_LOGI(TAG, "selftest: PASS (str + i64 round-trip, del/not_found)");
    return DECK_SDI_OK;
}
