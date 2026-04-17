#pragma once

#include "deck_sdi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the driver registry. Idempotent; safe to call once per boot. */
void deck_sdi_registry_init(void);

/* Register a driver. Slot is assigned by driver->id; each id may be registered
 * at most once (ERR_ALREADY_EXISTS on duplicate).
 */
deck_sdi_err_t deck_sdi_register(const deck_sdi_driver_t *driver);

/* Look up a driver by numeric id. Returns NULL if not registered. */
const deck_sdi_driver_t *deck_sdi_lookup(deck_sdi_driver_id_t id);

/* Look up a driver by canonical name. Returns NULL if not found. */
const deck_sdi_driver_t *deck_sdi_lookup_by_name(const char *name);

/* Number of currently registered drivers. */
size_t deck_sdi_count(void);

/* Iterate over all registered drivers in id order. */
typedef void (*deck_sdi_list_cb_t)(const deck_sdi_driver_t *driver, void *user);
void deck_sdi_list(deck_sdi_list_cb_t cb, void *user);

/* Convenience: log the list of registered drivers via ESP_LOGI. */
void deck_sdi_log_registered(void);

#ifdef __cplusplus
}
#endif
