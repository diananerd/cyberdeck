/*
 * CyberDeck — SD App Discovery (G2) + Script Stub (G3)
 *
 * Scans /sdcard/apps/ for subdirectories with manifest.json, parses them
 * via os_manifest_parse (G1), and registers them in the app registry (C2).
 *
 * Each discovered app:
 *   - Gets a dynamic app_id (APP_ID_DYNAMIC_BASE + n) unless manifest specifies one.
 *   - Registers storage path via os_storage_register so os_storage_dir() works.
 *   - Registers with cbs = NULL (no on_create) — the launcher handles the G3 stub
 *     message ("Script runtime not available") by checking manifest.type.
 *   - Fires EVT_SD_APP_DISCOVERED after each successful registration.
 */

#include "os_app_discover.h"
#include "os_manifest.h"
#include "os_storage.h"
#include "app_registry.h"
#include "svc_event.h"
#include "esp_log.h"
#include "hal_sdcard.h"
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *TAG = "os_discover";

/* OS API level — apps with min_os_api > this are rejected. */
#define OS_API_VERSION  1u

/* Dynamic ID counter — starts fresh each boot. */
static app_id_t s_next_id = APP_ID_DYNAMIC_BASE;

/* Apps root on SD */
#define SD_APPS_DIR  HAL_SDCARD_MOUNT_POINT "/apps"

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/* Returns true if path is a directory. */
static bool is_dir(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

/* Process one candidate directory.
 * Returns true if an app was registered. */
static bool process_app_dir(const char *dir_path)
{
    /* Build path to manifest.json */
    char mf_path[96];
    int n = snprintf(mf_path, sizeof(mf_path), "%s/" SD_MANIFEST_FILENAME, dir_path);
    if (n < 0 || (size_t)n >= sizeof(mf_path)) return false;

    /* Allocate manifest on heap — stays alive so registry pointers remain valid */
    sd_manifest_t *mf = (sd_manifest_t *)calloc(1, sizeof(sd_manifest_t));
    if (!mf) {
        ESP_LOGE(TAG, "OOM allocating sd_manifest_t");
        return false;
    }

    /* Parse */
    esp_err_t ret = os_manifest_parse(mf_path, mf);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "No valid manifest in %s (%s)", dir_path, esp_err_to_name(ret));
        free(mf);
        return false;
    }

    /* Set paths (not from JSON — derived from the directory location) */
    strncpy(mf->base_dir,    dir_path, sizeof(mf->base_dir) - 1);
    strncpy(mf->storage_dir, dir_path, sizeof(mf->storage_dir) - 1);

    /* Compatibility check */
    if (mf->min_os_api > OS_API_VERSION) {
        ESP_LOGW(TAG, "Skipping %s: requires OS API %u (have %u)",
                 mf->name, mf->min_os_api, OS_API_VERSION);
        free(mf);
        return false;
    }

    /* Assign ID */
    if (mf->id == APP_ID_INVALID) {
        mf->id = s_next_id++;
    } else if (mf->id < APP_ID_DYNAMIC_BASE) {
        ESP_LOGW(TAG, "Manifest %s claims reserved id %u — reassigning",
                 mf->name, mf->id);
        mf->id = s_next_id++;
    }

    /* Skip if already registered (e.g. re-scan after SD remount) */
    if (app_registry_get_raw(mf->id) != NULL) {
        ESP_LOGD(TAG, "App %u already registered — skipping", mf->id);
        free(mf);
        return false;
    }

    /* Register storage path */
    os_storage_register(mf->id, mf->storage_dir);

    /* Register app — cbs = NULL means launcher shows G3 stub behavior */
    app_manifest_t m = {
        .id          = mf->id,
        .name        = mf->name,         /* points into heap-allocated mf */
        .icon        = mf->icon[0] ? mf->icon : "?",
        .type        = mf->type,
        .permissions = mf->permissions,
        .storage_dir = mf->storage_dir,  /* points into heap-allocated mf */
    };
    os_app_register(&m, NULL, NULL);

    /* Notify the launcher that a new app appeared */
    svc_event_post(EVT_SD_APP_DISCOVERED, &mf->id, sizeof(mf->id));

    ESP_LOGI(TAG, "Discovered: %s (id=%u, type=%d, rt=%s) at %s",
             mf->name, (unsigned)mf->id, mf->type,
             mf->runtime[0] ? mf->runtime : "none", dir_path);
    return true;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t os_app_discover_sd(void)
{
    if (!hal_sdcard_is_mounted()) {
        ESP_LOGD(TAG, "SD not mounted");
        return ESP_ERR_NOT_FOUND;
    }

    DIR *d = opendir(SD_APPS_DIR);
    if (!d) {
        ESP_LOGW(TAG, "Cannot open %s", SD_APPS_DIR);
        return ESP_ERR_NOT_FOUND;
    }

    int discovered = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        /* Skip . and .. */
        if (entry->d_name[0] == '.') continue;

        char subdir[96];
        int n = snprintf(subdir, sizeof(subdir), "%s/%s", SD_APPS_DIR, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(subdir)) continue;

        if (!is_dir(subdir)) continue;

        if (process_app_dir(subdir)) {
            discovered++;
        }
    }
    closedir(d);

    ESP_LOGI(TAG, "Scan complete: %d app(s) discovered", discovered);
    return ESP_OK;
}
