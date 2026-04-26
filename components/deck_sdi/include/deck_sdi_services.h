#pragma once

/* deck_sdi_services — dynamic, name-based service registry for cross-app
 * @service "<id>" dispatch.
 *
 * The numeric driver registry (deck_sdi_registry) holds the fixed set of
 * platform-side capabilities (storage.nvs, network.http, …). Apps that
 * declare `@service "id"` are dynamic providers — count, names, and
 * vtables are not known at boot. This registry gives them an O(1) lookup
 * by service id string, decoupled from the loaded-apps slot table in
 * deck_shell.
 *
 * The opaque `provider` cookie is whatever the caller registers; the
 * runtime registers the deck_runtime_app_t handle, but the registry
 * itself is type-agnostic — a future native @service implementation
 * (e.g. a C-side notify provider) can register through the same surface.
 */

#include "deck_sdi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Idempotent — safe to call once per boot. */
void deck_sdi_services_init(void);

/* Register a service id → opaque provider mapping. Duplicate ids return
 * DECK_SDI_ERR_ALREADY_EXISTS. The id string is copied; provider is held
 * as-is (the caller owns it for the lifetime of the registration). */
deck_sdi_err_t deck_sdi_services_register(const char *service_id, void *provider);

/* Remove a registration (no-op if absent). */
void deck_sdi_services_unregister(const char *service_id);

/* O(1) lookup: returns the provider cookie or NULL. */
void *deck_sdi_services_lookup(const char *service_id);

/* Iterate every registration (in registration order). */
typedef bool (*deck_sdi_services_iter_cb)(const char *service_id,
                                          void *provider, void *user);
void deck_sdi_services_iter(deck_sdi_services_iter_cb cb, void *user);

size_t deck_sdi_services_count(void);

#ifdef __cplusplus
}
#endif
