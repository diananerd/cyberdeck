/*
 * CyberDeck — OS Core: SQLite Migration API (J2)
 * Compiled only when CONFIG_OS_ENABLE_SQLITE=y.
 */

#include "os_db.h"
#include "esp_log.h"

#ifdef CONFIG_OS_ENABLE_SQLITE

#include <string.h>

static const char *TAG = "os_db";

/* ---- Schema version table ---- */

static esp_err_t ensure_version_table(sqlite3 *db)
{
    int rc = sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS _schema_version "
        "(version INTEGER NOT NULL DEFAULT 0);",
        NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "Cannot create _schema_version: %s", sqlite3_errmsg(db));
        return ESP_FAIL;
    }

    /* Insert row if table is empty */
    rc = sqlite3_exec(db,
        "INSERT OR IGNORE INTO _schema_version (rowid, version) VALUES (1, 0);",
        NULL, NULL, NULL);
    return (rc == SQLITE_OK) ? ESP_OK : ESP_FAIL;
}

static int get_version(sqlite3 *db)
{
    sqlite3_stmt *stmt;
    int version = 0;
    if (sqlite3_prepare_v2(db, "SELECT version FROM _schema_version WHERE rowid=1;",
                            -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            version = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return version;
}

static esp_err_t set_version(sqlite3 *db, int version)
{
    char sql[64];
    snprintf(sql, sizeof(sql),
             "UPDATE _schema_version SET version=%d WHERE rowid=1;", version);
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    return (rc == SQLITE_OK) ? ESP_OK : ESP_FAIL;
}

/* ---- Public API ---- */

esp_err_t os_db_migrate(sqlite3 *db, const db_migration_t *migrations, int count)
{
    if (!db || !migrations || count <= 0) return ESP_OK;

    if (ensure_version_table(db) != ESP_OK) return ESP_FAIL;

    int cur = get_version(db);
    ESP_LOGI(TAG, "DB schema version: %d, migrations: %d", cur, count);

    for (int i = 0; i < count; i++) {
        if (migrations[i].version <= cur) continue;

        ESP_LOGI(TAG, "Applying migration v%d", migrations[i].version);

        if (sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL) != SQLITE_OK) {
            ESP_LOGE(TAG, "Cannot begin transaction for v%d", migrations[i].version);
            return ESP_FAIL;
        }

        int rc = sqlite3_exec(db, migrations[i].up_sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            ESP_LOGE(TAG, "Migration v%d failed: %s", migrations[i].version,
                     sqlite3_errmsg(db));
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            return ESP_FAIL;
        }

        if (set_version(db, migrations[i].version) != ESP_OK) {
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            return ESP_FAIL;
        }

        if (sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
            ESP_LOGE(TAG, "Commit failed for v%d", migrations[i].version);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Migration v%d applied", migrations[i].version);
    }

    return ESP_OK;
}

esp_err_t os_db_run(sqlite3 *db, const char *sql)
{
    if (!db || !sql) return ESP_ERR_INVALID_ARG;
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "SQL error: %s", err ? err : "?");
        sqlite3_free(err);
        return ESP_FAIL;
    }
    return ESP_OK;
}

int64_t os_db_last_rowid(sqlite3 *db)
{
    return db ? (int64_t)sqlite3_last_insert_rowid(db) : -1;
}

int os_db_get_int(sqlite3 *db, const char *sql, int default_val)
{
    if (!db || !sql) return default_val;
    sqlite3_stmt *stmt;
    int result = default_val;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            result = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return result;
}

#endif /* CONFIG_OS_ENABLE_SQLITE */
