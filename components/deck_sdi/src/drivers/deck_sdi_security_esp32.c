#include "drivers/deck_sdi_security.h"
#include "drivers/deck_sdi_nvs.h"
#include "deck_sdi_registry.h"

#include "psa/crypto.h"
#include "esp_random.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "sdi.security";

#define SEC_NS         "deck.sec"
#define KEY_SALT       "salt"          /* 16 bytes */
#define KEY_HASH       "pin_hash"      /* 32 bytes */
#define SALT_LEN       16
#define HASH_LEN       32
#define PERM_NS        "deck.perm"

static bool s_locked = false;

/* Constant-time memcmp — independent of input length leaking timing
 * differences via early termination. */
static int ct_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *p = a, *q = b;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= p[i] ^ q[i];
    return diff; /* 0 → equal */
}

static bool valid_pin(const char *pin)
{
    if (!pin) return false;
    size_t len = strlen(pin);
    if (len < DECK_SDI_SEC_PIN_MIN_LEN || len > DECK_SDI_SEC_PIN_MAX_LEN)
        return false;
    return true;
}

static bool s_psa_inited = false;

static deck_sdi_err_t ensure_psa(void)
{
    if (s_psa_inited) return DECK_SDI_OK;
    psa_status_t s = psa_crypto_init();
    if (s != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init: %d", (int)s);
        return DECK_SDI_ERR_IO;
    }
    s_psa_inited = true;
    return DECK_SDI_OK;
}

static deck_sdi_err_t hash_pin(const char *pin, const uint8_t *salt,
                                uint8_t out_hash[HASH_LEN])
{
    if (ensure_psa() != DECK_SDI_OK) return DECK_SDI_ERR_IO;

    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    psa_status_t s = psa_hash_setup(&op, PSA_ALG_SHA_256);
    if (s != PSA_SUCCESS) goto fail;
    s = psa_hash_update(&op, salt, SALT_LEN);
    if (s != PSA_SUCCESS) goto fail;
    s = psa_hash_update(&op, (const uint8_t *)pin, strlen(pin));
    if (s != PSA_SUCCESS) goto fail;
    size_t got = 0;
    s = psa_hash_finish(&op, out_hash, HASH_LEN, &got);
    if (s != PSA_SUCCESS || got != HASH_LEN) goto fail;
    return DECK_SDI_OK;
fail:
    psa_hash_abort(&op);
    return DECK_SDI_ERR_IO;
}

/* The public NVS wrappers don't expose get_blob/set_blob — go through
 * the vtable directly. Small static adapters. */
static deck_sdi_err_t nvs_get_blob_v(const char *ns, const char *key,
                                      void *out, size_t *io_len)
{
    const deck_sdi_driver_t *d = deck_sdi_lookup(DECK_SDI_DRIVER_NVS);
    if (!d) return DECK_SDI_ERR_NOT_FOUND;
    const deck_sdi_nvs_vtable_t *v = (const deck_sdi_nvs_vtable_t *)d->vtable;
    return v->get_blob(d->ctx, ns, key, out, io_len);
}
static deck_sdi_err_t nvs_set_blob_v(const char *ns, const char *key,
                                      const void *buf, size_t len)
{
    const deck_sdi_driver_t *d = deck_sdi_lookup(DECK_SDI_DRIVER_NVS);
    if (!d) return DECK_SDI_ERR_NOT_FOUND;
    const deck_sdi_nvs_vtable_t *v = (const deck_sdi_nvs_vtable_t *)d->vtable;
    return v->set_blob(d->ctx, ns, key, buf, len);
}

static deck_sdi_err_t load_record(uint8_t salt[SALT_LEN], uint8_t hash[HASH_LEN])
{
    size_t s_len = SALT_LEN;
    deck_sdi_err_t r = nvs_get_blob_v(SEC_NS, KEY_SALT, salt, &s_len);
    if (r == DECK_SDI_ERR_NOT_FOUND) return DECK_SDI_ERR_NOT_FOUND;
    if (r != DECK_SDI_OK || s_len != SALT_LEN) return r ? r : DECK_SDI_ERR_IO;

    size_t h_len = HASH_LEN;
    r = nvs_get_blob_v(SEC_NS, KEY_HASH, hash, &h_len);
    if (r == DECK_SDI_ERR_NOT_FOUND) return DECK_SDI_ERR_NOT_FOUND;
    if (r != DECK_SDI_OK || h_len != HASH_LEN) return r ? r : DECK_SDI_ERR_IO;
    return DECK_SDI_OK;
}

static bool sec_has_pin_impl(void *ctx)
{
    (void)ctx;
    uint8_t salt[SALT_LEN], hash[HASH_LEN];
    return load_record(salt, hash) == DECK_SDI_OK;
}

static deck_sdi_err_t sec_set_pin_impl(void *ctx, const char *old_pin,
                                        const char *new_pin)
{
    (void)ctx;
    if (!valid_pin(new_pin)) return DECK_SDI_ERR_INVALID_ARG;

    uint8_t salt[SALT_LEN], stored_hash[HASH_LEN];
    bool exists = (load_record(salt, stored_hash) == DECK_SDI_OK);

    if (exists) {
        if (!old_pin) return DECK_SDI_ERR_FAIL;
        uint8_t check[HASH_LEN];
        if (hash_pin(old_pin, salt, check) != DECK_SDI_OK)
            return DECK_SDI_ERR_IO;
        if (ct_memcmp(check, stored_hash, HASH_LEN) != 0)
            return DECK_SDI_ERR_FAIL;
    }

    /* Generate fresh salt + hash with new PIN. */
    esp_fill_random(salt, SALT_LEN);
    uint8_t new_hash[HASH_LEN];
    if (hash_pin(new_pin, salt, new_hash) != DECK_SDI_OK)
        return DECK_SDI_ERR_IO;

    deck_sdi_err_t r;
    r = nvs_set_blob_v(SEC_NS, KEY_SALT, salt, SALT_LEN);
    if (r != DECK_SDI_OK) return r;
    r = nvs_set_blob_v(SEC_NS, KEY_HASH, new_hash, HASH_LEN);
    if (r != DECK_SDI_OK) return r;
    return DECK_SDI_OK;
}

static deck_sdi_err_t sec_verify_pin_impl(void *ctx, const char *pin)
{
    (void)ctx;
    if (!pin) return DECK_SDI_ERR_INVALID_ARG;
    uint8_t salt[SALT_LEN], stored_hash[HASH_LEN];
    deck_sdi_err_t r = load_record(salt, stored_hash);
    if (r != DECK_SDI_OK) return r; /* NOT_FOUND propagates */
    uint8_t check[HASH_LEN];
    if (hash_pin(pin, salt, check) != DECK_SDI_OK) return DECK_SDI_ERR_IO;
    return ct_memcmp(check, stored_hash, HASH_LEN) == 0
            ? DECK_SDI_OK : DECK_SDI_ERR_FAIL;
}

static deck_sdi_err_t sec_clear_pin_impl(void *ctx, const char *current_pin)
{
    (void)ctx;
    deck_sdi_err_t r = sec_verify_pin_impl(ctx, current_pin);
    if (r != DECK_SDI_OK) return r;
    (void)deck_sdi_nvs_del(SEC_NS, KEY_SALT);
    (void)deck_sdi_nvs_del(SEC_NS, KEY_HASH);
    return DECK_SDI_OK;
}

static void sec_lock_impl(void *ctx)   { (void)ctx; s_locked = true; }
static void sec_unlock_impl(void *ctx) { (void)ctx; s_locked = false; }
static bool sec_is_locked_impl(void *ctx) { (void)ctx; return s_locked; }

static deck_sdi_err_t sec_perm_store_impl(void *ctx, const char *app_id,
                                           const void *bytes, size_t len)
{
    (void)ctx;
    if (!app_id) return DECK_SDI_ERR_INVALID_ARG;
    return nvs_set_blob_v(PERM_NS, app_id, bytes, len);
}

static deck_sdi_err_t sec_perm_load_impl(void *ctx, const char *app_id,
                                          void *out, size_t *io_len)
{
    (void)ctx;
    if (!app_id) return DECK_SDI_ERR_INVALID_ARG;
    return nvs_get_blob_v(PERM_NS, app_id, out, io_len);
}

static deck_sdi_err_t sec_perm_clear_impl(void *ctx, const char *app_id)
{
    (void)ctx;
    if (!app_id) return DECK_SDI_ERR_INVALID_ARG;
    return deck_sdi_nvs_del(PERM_NS, app_id);
}

static const deck_sdi_security_vtable_t s_vtable = {
    .set_pin     = sec_set_pin_impl,
    .verify_pin  = sec_verify_pin_impl,
    .clear_pin   = sec_clear_pin_impl,
    .has_pin     = sec_has_pin_impl,
    .lock        = sec_lock_impl,
    .unlock      = sec_unlock_impl,
    .is_locked   = sec_is_locked_impl,
    .perm_store  = sec_perm_store_impl,
    .perm_load   = sec_perm_load_impl,
    .perm_clear  = sec_perm_clear_impl,
};

deck_sdi_err_t deck_sdi_security_register_esp32(void)
{
    const deck_sdi_driver_t driver = {
        .name    = "system.security",
        .id      = DECK_SDI_DRIVER_SECURITY,
        .version = "1.0.0",
        .vtable  = &s_vtable,
        .ctx     = NULL,
    };
    return deck_sdi_register(&driver);
}

/* ---------- wrappers ---------- */

static const deck_sdi_security_vtable_t *sec_vt(void **ctx_out)
{
    const deck_sdi_driver_t *d = deck_sdi_lookup(DECK_SDI_DRIVER_SECURITY);
    if (!d) return NULL;
    if (ctx_out) *ctx_out = d->ctx;
    return (const deck_sdi_security_vtable_t *)d->vtable;
}

deck_sdi_err_t deck_sdi_security_set_pin(const char *old_pin, const char *new_pin)
{ void *c; const deck_sdi_security_vtable_t *v = sec_vt(&c);
  return v ? v->set_pin(c, old_pin, new_pin) : DECK_SDI_ERR_NOT_FOUND; }

deck_sdi_err_t deck_sdi_security_verify_pin(const char *pin)
{ void *c; const deck_sdi_security_vtable_t *v = sec_vt(&c);
  return v ? v->verify_pin(c, pin) : DECK_SDI_ERR_NOT_FOUND; }

deck_sdi_err_t deck_sdi_security_clear_pin(const char *current_pin)
{ void *c; const deck_sdi_security_vtable_t *v = sec_vt(&c);
  return v ? v->clear_pin(c, current_pin) : DECK_SDI_ERR_NOT_FOUND; }

bool deck_sdi_security_has_pin(void)
{ void *c; const deck_sdi_security_vtable_t *v = sec_vt(&c);
  return v ? v->has_pin(c) : false; }

void deck_sdi_security_lock(void)
{ void *c; const deck_sdi_security_vtable_t *v = sec_vt(&c);
  if (v) v->lock(c); }

void deck_sdi_security_unlock(void)
{ void *c; const deck_sdi_security_vtable_t *v = sec_vt(&c);
  if (v) v->unlock(c); }

bool deck_sdi_security_is_locked(void)
{ void *c; const deck_sdi_security_vtable_t *v = sec_vt(&c);
  return v ? v->is_locked(c) : false; }

deck_sdi_err_t deck_sdi_security_perm_store(const char *app_id,
                                             const void *bytes, size_t len)
{ void *c; const deck_sdi_security_vtable_t *v = sec_vt(&c);
  return v ? v->perm_store(c, app_id, bytes, len) : DECK_SDI_ERR_NOT_FOUND; }

deck_sdi_err_t deck_sdi_security_perm_load(const char *app_id,
                                            void *out, size_t *io_len)
{ void *c; const deck_sdi_security_vtable_t *v = sec_vt(&c);
  return v ? v->perm_load(c, app_id, out, io_len) : DECK_SDI_ERR_NOT_FOUND; }

deck_sdi_err_t deck_sdi_security_perm_clear(const char *app_id)
{ void *c; const deck_sdi_security_vtable_t *v = sec_vt(&c);
  return v ? v->perm_clear(c, app_id) : DECK_SDI_ERR_NOT_FOUND; }

/* ---------- selftest ---------- */

deck_sdi_err_t deck_sdi_security_selftest(void)
{
    /* Wipe any previous state before the test. */
    (void)deck_sdi_nvs_del(SEC_NS, KEY_SALT);
    (void)deck_sdi_nvs_del(SEC_NS, KEY_HASH);

    if (deck_sdi_security_has_pin()) {
        ESP_LOGE(TAG, "expected no PIN after wipe");
        return DECK_SDI_ERR_FAIL;
    }

    const char *pin1 = "1234";
    const char *pin2 = "9876";

    /* Set initial PIN — old_pin must be NULL. */
    deck_sdi_err_t r = deck_sdi_security_set_pin(NULL, pin1);
    if (r != DECK_SDI_OK) {
        ESP_LOGE(TAG, "set_pin (initial): %s", deck_sdi_strerror(r));
        return r;
    }
    if (!deck_sdi_security_has_pin()) {
        ESP_LOGE(TAG, "has_pin false after set_pin");
        return DECK_SDI_ERR_FAIL;
    }

    /* Verify good + bad. */
    r = deck_sdi_security_verify_pin(pin1);
    if (r != DECK_SDI_OK) { ESP_LOGE(TAG, "verify good: %s", deck_sdi_strerror(r)); return r; }
    r = deck_sdi_security_verify_pin("0000");
    if (r != DECK_SDI_ERR_FAIL) {
        ESP_LOGE(TAG, "verify bad: expected FAIL, got %s", deck_sdi_strerror(r));
        return DECK_SDI_ERR_FAIL;
    }

    /* Change PIN — must require old PIN. */
    r = deck_sdi_security_set_pin("0000", pin2);
    if (r != DECK_SDI_ERR_FAIL) {
        ESP_LOGE(TAG, "set_pin with wrong old PIN: expected FAIL, got %s",
                 deck_sdi_strerror(r));
        return DECK_SDI_ERR_FAIL;
    }
    r = deck_sdi_security_set_pin(pin1, pin2);
    if (r != DECK_SDI_OK) { ESP_LOGE(TAG, "rotate PIN: %s", deck_sdi_strerror(r)); return r; }
    r = deck_sdi_security_verify_pin(pin2);
    if (r != DECK_SDI_OK) { ESP_LOGE(TAG, "verify after rotate: %s", deck_sdi_strerror(r)); return r; }

    /* Lock/unlock round-trip. */
    deck_sdi_security_lock();
    if (!deck_sdi_security_is_locked()) return DECK_SDI_ERR_FAIL;
    deck_sdi_security_unlock();
    if (deck_sdi_security_is_locked()) return DECK_SDI_ERR_FAIL;

    /* Clear PIN. */
    r = deck_sdi_security_clear_pin(pin2);
    if (r != DECK_SDI_OK) { ESP_LOGE(TAG, "clear_pin: %s", deck_sdi_strerror(r)); return r; }
    if (deck_sdi_security_has_pin()) {
        ESP_LOGE(TAG, "has_pin true after clear");
        return DECK_SDI_ERR_FAIL;
    }

    /* Permission blob round-trip. */
    const uint8_t perm_bytes[3] = {0x01, 0x02, 0x04};
    r = deck_sdi_security_perm_store("test.app", perm_bytes, 3);
    if (r != DECK_SDI_OK) { ESP_LOGE(TAG, "perm_store: %s", deck_sdi_strerror(r)); return r; }
    uint8_t got[8] = {0};
    size_t got_len = sizeof(got);
    r = deck_sdi_security_perm_load("test.app", got, &got_len);
    if (r != DECK_SDI_OK || got_len != 3 || memcmp(got, perm_bytes, 3) != 0) {
        ESP_LOGE(TAG, "perm_load mismatch: r=%s len=%u",
                 deck_sdi_strerror(r), (unsigned)got_len);
        return DECK_SDI_ERR_FAIL;
    }
    (void)deck_sdi_security_perm_clear("test.app");

    ESP_LOGI(TAG, "selftest: PASS (set/verify/rotate/clear PIN + perms blob)");
    return DECK_SDI_OK;
}
