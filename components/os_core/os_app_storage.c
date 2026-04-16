/*
 * CyberDeck — OS Core: Per-app Storage Sandbox (J1)
 */

#include "os_app_storage.h"
#include "esp_log.h"
#include <sys/stat.h>
#include <string.h>
#include <dirent.h>

static const char *TAG = "os_app_storage";

#define SDCARD_APPS_ROOT  "/sdcard/apps"

/* ---- Internal helpers ---- */

static esp_err_t ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return ESP_OK;  /* exists */
    if (mkdir(path, 0777) != 0) {
        ESP_LOGW(TAG, "mkdir('%s') failed", path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---- Public API ---- */

esp_err_t os_app_storage_open(app_id_t id, const char *storage_dir,
                               const db_migration_t *migrations,
                               int migration_count,
                               os_app_storage_t *out)
{
    memset(out, 0, sizeof(*out));

    if (!storage_dir || storage_dir[0] == '\0') {
        /* App sin storage configurado — OK, db=NULL, db_ready=false */
        return ESP_OK;
    }

    /* Build paths */
    snprintf(out->base_path,  sizeof(out->base_path),
             "%s/%s", SDCARD_APPS_ROOT, storage_dir);
    snprintf(out->files_path, sizeof(out->files_path),
             "%s/files", out->base_path);
    snprintf(out->cache_path, sizeof(out->cache_path),
             "%s/cache", out->base_path);

    /* Create directories — fail gracefully if SD not mounted */
    if (ensure_dir(SDCARD_APPS_ROOT) != ESP_OK ||
        ensure_dir(out->base_path)   != ESP_OK ||
        ensure_dir(out->files_path)  != ESP_OK ||
        ensure_dir(out->cache_path)  != ESP_OK) {
        /* SD not mounted or not writable — degraded mode */
        ESP_LOGW(TAG, "app %u: SD not available, degraded mode", (unsigned)id);
        return ESP_OK;  /* not an error — app handles db_ready=false */
    }

#ifdef CONFIG_OS_ENABLE_SQLITE
    char db_path[88];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", out->base_path, storage_dir);

    sqlite3 *db = NULL;
    int rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "sqlite3_open(%s): %s", db_path, sqlite3_errmsg(db));
        if (db) sqlite3_close(db);
        return ESP_OK;  /* degraded: db_ready stays false */
    }

    sqlite3_busy_timeout(db, 1000);

    if (migrations && migration_count > 0) {
        if (os_db_migrate(db, migrations, migration_count) != ESP_OK) {
            ESP_LOGE(TAG, "app %u: migration failed", (unsigned)id);
            sqlite3_close(db);
            return ESP_OK;  /* degraded */
        }
    }

    out->db       = (void *)db;
    out->db_ready = true;
    ESP_LOGI(TAG, "app %u: storage opened (%s)", (unsigned)id, db_path);
#else
    ESP_LOGI(TAG, "app %u: storage dirs created (SQLite disabled)", (unsigned)id);
#endif

    return ESP_OK;
}

void os_app_storage_close(os_app_storage_t *storage)
{
    if (!storage) return;

#ifdef CONFIG_OS_ENABLE_SQLITE
    if (storage->db) {
        sqlite3 *db = (sqlite3 *)storage->db;
        /* WAL checkpoint before close */
        sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
        sqlite3_close_v2(db);
        storage->db = NULL;
    }
#endif

    storage->db_ready = false;
}

const char *os_app_storage_path(const os_app_storage_t *storage,
                                const char *rel, char *buf, size_t buflen)
{
    if (!storage || storage->base_path[0] == '\0' || !rel || !buf) return NULL;
    snprintf(buf, buflen, "%s/%s", storage->base_path, rel);
    return buf;
}

esp_err_t os_app_storage_clear_cache(os_app_storage_t *storage)
{
    if (!storage || storage->cache_path[0] == '\0') return ESP_ERR_INVALID_ARG;

    DIR *dir = opendir(storage->cache_path);
    if (!dir) return ESP_ERR_NOT_FOUND;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;
        char path[384];
        snprintf(path, sizeof(path), "%s/%s", storage->cache_path, entry->d_name);
        remove(path);
    }
    closedir(dir);
    return ESP_OK;
}

esp_err_t os_app_storage_reopen(os_app_storage_t *storage,
                                 const char *storage_dir,
                                 const db_migration_t *migrations,
                                 int migration_count)
{
    if (!storage || storage->db_ready) return ESP_OK;

    /* Rebuild paths if missing (e.g. first open was degraded) */
    if (storage->base_path[0] == '\0' && storage_dir) {
        snprintf(storage->base_path,  sizeof(storage->base_path),
                 "%s/%s", SDCARD_APPS_ROOT, storage_dir);
        snprintf(storage->files_path, sizeof(storage->files_path),
                 "%s/files", storage->base_path);
        snprintf(storage->cache_path, sizeof(storage->cache_path),
                 "%s/cache", storage->base_path);
    }

    ensure_dir(SDCARD_APPS_ROOT);
    ensure_dir(storage->base_path);
    ensure_dir(storage->files_path);
    ensure_dir(storage->cache_path);

#ifdef CONFIG_OS_ENABLE_SQLITE
    if (!storage_dir) return ESP_ERR_INVALID_ARG;

    char db_path[88];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", storage->base_path, storage_dir);

    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return ESP_FAIL;
    }
    sqlite3_busy_timeout(db, 1000);

    if (migrations && migration_count > 0) {
        if (os_db_migrate(db, migrations, migration_count) != ESP_OK) {
            sqlite3_close(db);
            return ESP_FAIL;
        }
    }

    storage->db       = (void *)db;
    storage->db_ready = true;
#endif

    return ESP_OK;
}
