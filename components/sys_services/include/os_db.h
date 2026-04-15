/*
 * CyberDeck — SQLite3 wrapper (F3)
 *
 * Thin per-app database API built on SQLite3 (via idf-sqlite3 component).
 * Enabled by CONFIG_OS_ENABLE_SQLITE=y in sdkconfig / menuconfig.
 *
 * Database files are stored in the app's os_storage directory:
 *   /sdcard/apps/<id>/<filename>
 *
 * To enable SQLite:
 *   1. Add to main/idf_component.yml:
 *        espressif/idf-sqlite3: "^1"
 *   2. Run: idf.py update-dependencies
 *   3. Enable: idf.py menuconfig -> CyberDeck -> Enable SQLite
 *
 * Requires PSRAM (page cache is allocated via SPIRAM caps).
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "os_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_OS_ENABLE_SQLITE

/* Opaque database handle */
typedef struct os_db_s os_db_t;

/* Row callback — called for each result row from os_db_exec.
 * @param n_cols   Number of columns.
 * @param values   Array of column value strings (NULL if NULL SQL value).
 * @param names    Array of column name strings.
 * @param ctx      User context passed to os_db_exec.
 * @return 0 to continue iteration, non-zero to stop.
 */
typedef int (*os_db_row_cb_t)(int n_cols, char **values,
                               char **names, void *ctx);

/**
 * Open (or create) a SQLite database in the app's storage directory.
 * @param id        App owner.
 * @param filename  Filename within the app's storage dir (e.g. "tasks.db").
 * @return Opaque handle, or NULL on error.
 */
os_db_t   *os_db_open(app_id_t id, const char *filename);

/**
 * Execute one or more SQL statements.
 * @param db   Database handle.
 * @param sql  SQL string (UTF-8, may contain multiple statements separated by ';').
 * @param cb   Row callback (may be NULL for non-SELECT statements).
 * @param ctx  Passed through to cb.
 * @return ESP_OK on success, ESP_FAIL on SQL error.
 */
esp_err_t  os_db_exec(os_db_t *db, const char *sql,
                       os_db_row_cb_t cb, void *ctx);

/**
 * Close the database and free all resources.
 */
void       os_db_close(os_db_t *db);

#endif /* CONFIG_OS_ENABLE_SQLITE */

#ifdef __cplusplus
}
#endif
