#pragma once

/* system.security — PIN + permission storage.
 *
 * DL2 driver. Stores a salted SHA-256 hash of the user PIN under NVS
 * (namespace "deck.sec"). Verification is constant-time at the byte
 * level. Lock state is in-RAM (not persisted) — boot always starts
 * unlocked-but-required-if-PIN-set; the shell decides what to do.
 *
 * Permission storage is a flat NVS bitmap keyed by app id. The runtime
 * decides the bit layout; this driver only persists/reads bytes.
 */

#include "deck_sdi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DECK_SDI_SEC_PIN_MIN_LEN   4
#define DECK_SDI_SEC_PIN_MAX_LEN   16

/* Returned by has_pin() when no PIN has been set. */
typedef struct {
    /* Set the PIN (digits as ASCII). Old PIN required if one exists.
     * old_pin may be NULL when no PIN is yet set. */
    deck_sdi_err_t (*set_pin)(void *ctx, const char *old_pin, const char *new_pin);

    /* Verify a PIN candidate. Returns DECK_SDI_OK if it matches the
     * stored PIN, DECK_SDI_ERR_FAIL if not, NOT_FOUND if no PIN set. */
    deck_sdi_err_t (*verify_pin)(void *ctx, const char *pin);

    /* Remove the stored PIN (requires current PIN). */
    deck_sdi_err_t (*clear_pin)(void *ctx, const char *current_pin);

    /* True if a PIN is currently stored. */
    bool (*has_pin)(void *ctx);

    /* Lock state — in-RAM only. */
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    bool (*is_locked)(void *ctx);

    /* Permission bitmap storage (binary blob, opaque to the driver). */
    deck_sdi_err_t (*perm_store)(void *ctx, const char *app_id,
                                 const void *bytes, size_t len);
    deck_sdi_err_t (*perm_load)(void *ctx, const char *app_id,
                                void *out, size_t *io_len);
    deck_sdi_err_t (*perm_clear)(void *ctx, const char *app_id);
} deck_sdi_security_vtable_t;

deck_sdi_err_t deck_sdi_security_register_esp32(void);

/* High-level wrappers. */
deck_sdi_err_t deck_sdi_security_set_pin(const char *old_pin, const char *new_pin);
deck_sdi_err_t deck_sdi_security_verify_pin(const char *pin);
deck_sdi_err_t deck_sdi_security_clear_pin(const char *current_pin);
bool           deck_sdi_security_has_pin(void);
void           deck_sdi_security_lock(void);
void           deck_sdi_security_unlock(void);
bool           deck_sdi_security_is_locked(void);
deck_sdi_err_t deck_sdi_security_perm_store(const char *app_id, const void *bytes, size_t len);
deck_sdi_err_t deck_sdi_security_perm_load (const char *app_id, void *out, size_t *io_len);
deck_sdi_err_t deck_sdi_security_perm_clear(const char *app_id);

/* Selftest: set, verify (good + bad), clear, lock/unlock round-trip. */
deck_sdi_err_t deck_sdi_security_selftest(void);

#ifdef __cplusplus
}
#endif
