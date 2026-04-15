/*
 * CyberDeck — OS Storage API (F2)
 *
 * Provides per-app file storage on the SD card.
 * Default path: /sdcard/apps/<app_id>/
 * Custom paths can be registered with os_storage_register() (used by G2 for dynamic apps).
 *
 * All functions return NULL / ESP_ERR_INVALID_STATE if the SD card is not mounted.
 * os_storage_dir() creates the directory on first access.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "esp_err.h"
#include "os_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize storage registry. Call once from app_main before SD mount. */
esp_err_t   os_storage_init(void);

/**
 * Register a custom base directory for an app.
 * Dynamic apps (discovered from SD) call this so os_storage_dir() returns their
 * actual path (e.g. /sdcard/apps/myapp) instead of the numeric fallback.
 */
esp_err_t   os_storage_register(app_id_t id, const char *base_dir);

/**
 * Return the base directory for an app, creating it if needed.
 * Returns NULL if SD is not mounted.
 * NOTE: returns a pointer to a static buffer — use before calling again.
 */
const char *os_storage_dir(app_id_t id);

/**
 * Build a full path for a file within the app's storage directory.
 * @param buf  Caller-provided output buffer.
 * @param len  Buffer size.
 * @return buf on success, NULL if SD is not mounted.
 */
const char *os_storage_path(app_id_t id, const char *name, char *buf, size_t len);

/** Open a file relative to the app's storage directory. */
FILE       *os_storage_fopen(app_id_t id, const char *rel, const char *mode);

/**
 * Read entire file into buf. Updates *len with bytes actually read.
 * Returns ESP_ERR_NOT_FOUND if file does not exist.
 */
esp_err_t   os_storage_read(app_id_t id, const char *name, void *buf, size_t *len);

/** Write data to file (creates or overwrites). */
esp_err_t   os_storage_write(app_id_t id, const char *name, const void *data, size_t len);

/** Check whether a file exists in the app's storage directory. */
bool        os_storage_exists(app_id_t id, const char *name);

/** Delete a file from the app's storage directory. */
esp_err_t   os_storage_delete(app_id_t id, const char *name);

#ifdef __cplusplus
}
#endif
