/*
 * CyberDeck — SD App Manifest Parser (G1)
 *
 * Parses manifest.json files found in /sdcard/apps/<appname>/ and fills
 * sd_manifest_t. All string data lives in the struct's own buffers —
 * the struct must remain allocated as long as the app is registered
 * (the registry stores pointers into these buffers).
 *
 * Expected JSON schema (all fields except "name" are optional):
 * {
 *   "id":         256,               // uint16, 0 = auto-assign from APP_ID_DYNAMIC_BASE
 *   "name":       "My App",          // display name (ALL CAPS recommended)
 *   "icon":       "My",              // 1-3 char launcher icon
 *   "type":       "script",          // "script" (default) or "builtin" (reserved)
 *   "runtime":    "lua",             // script runtime name, e.g. "lua", "micropython"
 *   "entry":      "main.lua",        // entry-point file relative to app directory
 *   "permissions": ["wifi", "sd"],   // array of permission strings
 *   "version":    "1.0.0",           // semver string
 *   "min_os_api": 1                  // minimum OS API level required
 * }
 *
 * Lifecycle:
 *   sd_manifest_t *mf = malloc(sizeof(*mf));
 *   os_manifest_parse(path, mf);        // fills mf
 *   // build app_manifest_t with .name = mf->name, .icon = mf->icon, etc.
 *   os_app_register(&m, NULL, NULL);   // registry keeps shallow ptr into mf
 *   // mf must stay allocated for the lifetime of the OS
 */

#pragma once

#include "app_registry.h"   /* app_id_t, app_type_t, APP_PERM_*, APP_ID_INVALID */
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_MANIFEST_FILENAME  "manifest.json"

/**
 * Parsed SD-app manifest.
 * All strings are null-terminated, stored in inline buffers.
 * Pointers in app_manifest_t (name, icon, storage_dir) should point
 * into these buffers — keep this struct allocated indefinitely.
 */
typedef struct {
    /* Parsed from JSON */
    app_id_t    id;                 /**< APP_ID_INVALID = auto-assign in G2. */
    char        name[64];           /**< Display name. */
    char        icon[8];            /**< Launcher icon (1-3 chars). */
    app_type_t  type;               /**< APP_TYPE_SCRIPT or APP_TYPE_BUILTIN. */
    uint8_t     permissions;        /**< Bitmask of APP_PERM_* flags. */
    char        runtime[16];        /**< Script runtime name, e.g. "lua". */
    char        entry[64];          /**< Entry-point filename within base_dir. */
    char        version[16];        /**< Version string. */
    uint16_t    min_os_api;         /**< Minimum OS API level (0 = any). */

    /* Set by the caller (G2), not from JSON */
    char        base_dir[72];       /**< Full path to app directory on SD. */
    char        storage_dir[72];    /**< Same as base_dir for SD apps. */
} sd_manifest_t;

/**
 * Parse a manifest.json file into an sd_manifest_t.
 *
 * @param path   Absolute path to the manifest.json file.
 * @param out    Output — zero-initialized by the caller before calling.
 *               base_dir and storage_dir must be set by the caller after
 *               this returns (they are derived from the directory path, not JSON).
 * @return ESP_OK on success,
 *         ESP_ERR_NOT_FOUND  if the file does not exist or cannot be read,
 *         ESP_ERR_INVALID_ARG if JSON is malformed or "name" is missing,
 *         ESP_ERR_NO_MEM      if cJSON allocation fails.
 */
esp_err_t os_manifest_parse(const char *path, sd_manifest_t *out);

#ifdef __cplusplus
}
#endif
