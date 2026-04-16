/*
 * CyberDeck — OS Core: SQLite Migration API (J2)
 *
 * Schema versioning para bases de datos SQLite por app.
 * Solo disponible con CONFIG_OS_ENABLE_SQLITE=y (idf-sqlite3 component).
 *
 * Convenciones de schema:
 *   - id INTEGER PRIMARY KEY en todas las tablas
 *   - Timestamps: INTEGER Unix seconds
 *   - Texto: TEXT NOT NULL DEFAULT ''
 *   - Booleans: INTEGER NOT NULL DEFAULT 0
 *   - La tabla _schema_version es gestionada por el OS — no tocar
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Un paso de migración de schema. Las migraciones deben tener versión
 * monótonamente creciente (1, 2, 3...) y estar ordenadas en el array.
 */
typedef struct {
    int         version; /**< Número de versión target. Debe ser > 0. */
    const char *up_sql;  /**< DDL/DML para subir a esta versión. */
} db_migration_t;

#ifdef CONFIG_OS_ENABLE_SQLITE

#include <sqlite3.h>

/**
 * Corre las migrations pendientes en orden ascendente.
 * Lee y escribe la versión actual en la tabla _schema_version (autocreada).
 * Cada migration se ejecuta en su propia transacción — fallo → rollback.
 *
 * @param db         Handle SQLite abierto.
 * @param migrations Array de migraciones, ordenado por .version ascendente.
 * @param count      Número de entradas en el array.
 * @return ESP_OK si todas las migrations pendientes se aplicaron con éxito.
 */
esp_err_t os_db_migrate(sqlite3 *db, const db_migration_t *migrations, int count);

/** Ejecuta SQL sin resultado (CREATE, INSERT, UPDATE, DELETE). */
esp_err_t os_db_run(sqlite3 *db, const char *sql);

/** Retorna el rowid del último INSERT exitoso. */
int64_t os_db_last_rowid(sqlite3 *db);

/**
 * Retorna el primer entero de la primera fila del query.
 * Útil para COUNT(*), MAX(id), etc.
 * @param default_val Valor retornado si el query no da resultados o falla.
 */
int os_db_get_int(sqlite3 *db, const char *sql, int default_val);

#endif /* CONFIG_OS_ENABLE_SQLITE */

#ifdef __cplusplus
}
#endif
