/*
 * CyberDeck — SD App Discovery (G2/G3)
 *
 * Scans /sdcard/apps/ for subdirectories containing manifest.json,
 * parses each manifest (G1), and registers the apps with the OS registry (C2).
 *
 * Apps registered here have APP_TYPE_SCRIPT and no activity callbacks.
 * The launcher shows them as available but dimmed (no on_create).
 * Tapping a script app in the launcher shows "Script runtime not available" (G3).
 *
 * Call os_app_discover_sd() after the SD card is mounted.
 * Re-calling it (e.g. on EVT_SDCARD_MOUNTED) re-scans and registers new apps;
 * already-registered apps keep their current state.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Scan /sdcard/apps/ for manifest.json files and register discovered apps.
 *
 * - Each valid app directory must contain a manifest.json.
 * - Apps with min_os_api > OS_API_VERSION are skipped (incompatible).
 * - Apps with the same app_id as an already-registered entry are skipped.
 * - Dynamic IDs are auto-assigned from APP_ID_DYNAMIC_BASE upward.
 *
 * @return ESP_OK            At least one scan was performed (even if no apps found).
 *         ESP_ERR_NOT_FOUND SD not mounted or /sdcard/apps/ does not exist.
 */
esp_err_t os_app_discover_sd(void);

#ifdef __cplusplus
}
#endif
