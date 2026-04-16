/*
 * CyberDeck — OS Core: Per-app Storage Sandbox (J1)
 *
 * Gestiona el espacio de almacenamiento de cada app en la SD:
 *   /sdcard/apps/<storage_dir>/           ← base_path
 *   /sdcard/apps/<storage_dir>/files/     ← files_path (datos de usuario)
 *   /sdcard/apps/<storage_dir>/cache/     ← cache_path (descartable)
 *   /sdcard/apps/<storage_dir>/<name>.db  ← SQLite (si CONFIG_OS_ENABLE_SQLITE)
 *
 * El OS abre el storage ANTES de llamar app_ops.on_launch() y lo cierra
 * DESPUÉS de app_ops.on_terminate().
 * Si la SD no está montada → ESP_OK con db=NULL, db_ready=false.
 * La app debe funcionar en modo degradado cuando db_ready==false.
 */

#pragma once

#include "os_db.h"
#include "os_core.h"
#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char  base_path[64];   /**< /sdcard/apps/<storage_dir>           */
    char  files_path[72];  /**< /sdcard/apps/<storage_dir>/files     */
    char  cache_path[72];  /**< /sdcard/apps/<storage_dir>/cache     */
    void *db;              /**< sqlite3* cast; NULL si SQLite off o SD offline */
    bool  db_ready;        /**< true si DB abierta + migrations corridas OK    */
} os_app_storage_t;

/**
 * Abre el storage para un app.
 * Crea los directorios base, files, cache si no existen.
 * Si storage_dir es NULL → retorna ESP_OK con storage vacío (db=NULL, db_ready=false).
 * Si SD no montada → retorna ESP_OK con db=NULL, db_ready=false.
 *
 * @param id            App ID (para logging).
 * @param storage_dir   Nombre del directorio, e.g. "notes". NULL = sin storage.
 * @param migrations    Migraciones de DB, o NULL si no hay.
 * @param migration_count Número de entradas en migrations.
 * @param out           Estructura de output. Inicializada a cero en caso de error.
 */
esp_err_t os_app_storage_open(app_id_t id, const char *storage_dir,
                               const db_migration_t *migrations,
                               int migration_count,
                               os_app_storage_t *out);

/**
 * Cierra el storage: WAL checkpoint + sqlite3_close_v2.
 * Los directorios y archivos se conservan en SD.
 * No-op si storage ya está cerrado.
 */
void os_app_storage_close(os_app_storage_t *storage);

/**
 * Construye un path absoluto dentro del sandbox.
 *   os_app_storage_path(&st, "files/doc.txt", buf, sizeof(buf))
 *   → "/sdcard/apps/notes/files/doc.txt"
 * @return buf en éxito; NULL si storage no tiene base_path.
 */
const char *os_app_storage_path(const os_app_storage_t *storage,
                                const char *rel, char *buf, size_t buflen);

/**
 * Borra todos los archivos en cache_path.
 * No toca files/ ni la DB.
 */
esp_err_t os_app_storage_clear_cache(os_app_storage_t *storage);

/**
 * Reabre el storage tras un EVT_SDCARD_MOUNTED si db_ready==false.
 * Solo abre DB + migrations; los paths ya están configurados.
 */
esp_err_t os_app_storage_reopen(os_app_storage_t *storage,
                                 const char *storage_dir,
                                 const db_migration_t *migrations,
                                 int migration_count);

#ifdef __cplusplus
}
#endif
