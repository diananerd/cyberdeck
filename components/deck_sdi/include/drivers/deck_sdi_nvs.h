#pragma once

/* storage.nvs — non-volatile key/value store.
 *
 * Mandatory at DL1. Provides namespaced string/i64/blob storage with
 * explicit commit semantics.
 *
 * See deck-lang/05-deck-os-api.md §2 for the capability contract.
 */

#include "deck_sdi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    deck_sdi_err_t (*get_str)(void *ctx, const char *ns, const char *key,
                              char *out, size_t out_size);
    deck_sdi_err_t (*set_str)(void *ctx, const char *ns, const char *key,
                              const char *value);
    deck_sdi_err_t (*get_i64)(void *ctx, const char *ns, const char *key,
                              int64_t *out);
    deck_sdi_err_t (*set_i64)(void *ctx, const char *ns, const char *key,
                              int64_t value);
    deck_sdi_err_t (*get_blob)(void *ctx, const char *ns, const char *key,
                               void *out, size_t *io_size);
    deck_sdi_err_t (*set_blob)(void *ctx, const char *ns, const char *key,
                               const void *buf, size_t size);
    deck_sdi_err_t (*del)(void *ctx, const char *ns, const char *key);
    deck_sdi_err_t (*commit)(void *ctx, const char *ns);
} deck_sdi_nvs_vtable_t;

/* Platform registration — ESP32 impl on top of esp-idf/nvs_flash. */
deck_sdi_err_t deck_sdi_nvs_register_esp32(void);

/* High-level wrappers — look the driver up in the registry once,
 * then dispatch. Intended for host/main code; the runtime will look
 * up the vtable directly for efficiency.
 */
deck_sdi_err_t deck_sdi_nvs_get_str(const char *ns, const char *key,
                                    char *out, size_t out_size);
deck_sdi_err_t deck_sdi_nvs_set_str(const char *ns, const char *key,
                                    const char *value);
deck_sdi_err_t deck_sdi_nvs_get_i64(const char *ns, const char *key,
                                    int64_t *out);
deck_sdi_err_t deck_sdi_nvs_set_i64(const char *ns, const char *key,
                                    int64_t value);
deck_sdi_err_t deck_sdi_nvs_del(const char *ns, const char *key);
deck_sdi_err_t deck_sdi_nvs_commit(const char *ns);

/* Round-trip selftest over namespace "deck.test". Leaves no residue. */
deck_sdi_err_t deck_sdi_nvs_selftest(void);

#ifdef __cplusplus
}
#endif
