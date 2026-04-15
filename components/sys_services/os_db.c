/*
 * CyberDeck -- SQLite3 wrapper (F3)
 *
 * Compiled only when CONFIG_OS_ENABLE_SQLITE=y.
 * Depends on the idf-sqlite3 managed component (espressif/idf-sqlite3).
 *
 * Usage notes:
 *   - os_db_open() builds the path via os_storage_path(), which requires
 *     the SD card to be mounted and the app to have a storage directory.
 *   - All SQLite page cache memory is allocated from PSRAM when available
 *     (the idf-sqlite3 component handles SPIRAM mapping at init time).
 *   - Not thread-safe: each caller must serialize access or open separate
 *     handles per task.
 */

#include "os_db.h"

#ifdef CONFIG_OS_ENABLE_SQLITE

#include "os_storage.h"
#include "esp_log.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "os_db";

struct os_db_s {
    sqlite3 *db;
};

os_db_t *os_db_open(app_id_t id, const char *filename)
{
    char path[96];
    if (!os_storage_path(id, filename, path, sizeof(path))) {
        ESP_LOGE(TAG, "os_db_open: SD not mounted or path error");
        return NULL;
    }

    os_db_t *handle = (os_db_t *)malloc(sizeof(os_db_t));
    if (!handle) return NULL;

    int rc = sqlite3_open(path, &handle->db);
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "sqlite3_open(%s): %s", path, sqlite3_errmsg(handle->db));
        sqlite3_close(handle->db);
        free(handle);
        return NULL;
    }

    /* Increase page cache timeout -- avoid locking errors under brief contention */
    sqlite3_busy_timeout(handle->db, 1000);

    ESP_LOGD(TAG, "Opened: %s", path);
    return handle;
}

/* Adapter: sqlite3_exec callback -> os_db_row_cb_t */
typedef struct {
    os_db_row_cb_t user_cb;
    void          *user_ctx;
    int            rc;
} exec_ctx_t;

static int sqlite_row_cb(void *vctx, int n_cols, char **values, char **names)
{
    exec_ctx_t *ctx = (exec_ctx_t *)vctx;
    if (ctx->user_cb) {
        int stop = ctx->user_cb(n_cols, values, names, ctx->user_ctx);
        if (stop) {
            ctx->rc = SQLITE_ABORT;
            return 1;  /* tell sqlite3 to stop iteration */
        }
    }
    return 0;
}

esp_err_t os_db_exec(os_db_t *db, const char *sql, os_db_row_cb_t cb, void *ctx)
{
    if (!db || !sql) return ESP_ERR_INVALID_ARG;

    exec_ctx_t ectx = { .user_cb = cb, .user_ctx = ctx, .rc = SQLITE_OK };
    char *err_msg = NULL;

    int rc = sqlite3_exec(db->db, sql, sqlite_row_cb, &ectx, &err_msg);
    if (rc != SQLITE_OK && rc != SQLITE_ABORT) {
        ESP_LOGE(TAG, "os_db_exec: %s", err_msg ? err_msg : "unknown error");
        if (err_msg) sqlite3_free(err_msg);
        return ESP_FAIL;
    }
    if (err_msg) sqlite3_free(err_msg);
    return ESP_OK;
}

void os_db_close(os_db_t *db)
{
    if (!db) return;
    sqlite3_close(db->db);
    free(db);
}

#endif /* CONFIG_OS_ENABLE_SQLITE */
