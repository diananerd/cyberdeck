/*
 * CyberDeck — App Registry
 *
 * C1: app_manifest_t, app_ops_t, app_type_t — formal OS app descriptor types.
 * C2: dynamic list (os_app_register, os_app_enumerate) alongside legacy API.
 * C3: built-ins registered via os_app_register; hardcoded slots eliminated.
 * J4: app_ops_t con on_launch/on_terminate completos; view_cbs_t en app_entry_t.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "ui_activity.h"
#include "os_core.h"          /* app_id_t, APP_ID_* constants */
#include "os_app_storage.h"   /* os_app_storage_t, db_migration_t */

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * C1 — App manifest and ops types
 * =========================================================================
 */

/** Permission flags for app manifests. */
#define APP_PERM_WIFI          (1u << 0)
#define APP_PERM_SD            (1u << 1)
#define APP_PERM_NETWORK       (1u << 2)
#define APP_PERM_SETTINGS      (1u << 3)
#define APP_PERM_HIDE_LAUNCHER (1u << 7) /**< App does not appear in launcher grid. */

typedef enum {
    APP_TYPE_BUILTIN = 0,  /**< Compiled into firmware, always available. */
    APP_TYPE_SCRIPT,       /**< Interpreted from SD card at runtime. */
} app_type_t;

/** Static descriptor for an app — filled at compile time or loaded from JSON. */
typedef struct {
    app_id_t        id;
    const char     *name;        /**< Display name, e.g. "SETTINGS". */
    const char     *icon;        /**< Short string for launcher card, e.g. "St". */
    app_type_t      type;
    uint8_t         permissions; /**< Bitmask of APP_PERM_* flags. */
    const char     *storage_dir; /**< e.g. "notes" → /sdcard/apps/notes/; NULL = sin storage. */
} app_manifest_t;

/**
 * High-level app lifecycle vtable — proceso-level (J4).
 *
 *  on_launch    — [1] Antes de pushear ninguna view. Retorna app_data* (L1).
 *                 Si retorna NULL → launch abortado.
 *                 storage->db_ready==false → SD offline, continuar sin DB.
 *                 Puede ser NULL para apps sin L1 state.
 *  on_terminate — [6] Tras destruir todas las views. Flush a DB, free(app_data).
 *                 El OS cierra storage DESPUÉS de este call.
 *                 Puede ser NULL.
 *  on_suspend   — App enviada al home (proceso sigue, screen oculta). Opcional.
 *  on_foreground — App traída de vuelta del home. Opcional.
 */
typedef struct {
    void *(*on_launch    )(app_id_t id, const view_args_t *args,
                           os_app_storage_t *storage);
    void  (*on_terminate )(app_id_t id, void *app_data);
    void  (*on_suspend   )(app_id_t id, void *app_data);
    void  (*on_foreground)(app_id_t id, void *app_data);
} app_ops_t;

/* =========================================================================
 * app_entry_t — internal registry slot
 * =========================================================================
 */
typedef struct {
    app_manifest_t  manifest;
    app_ops_t       ops;
    view_cbs_t      cbs;       /**< Main-screen (screen_id==0) lifecycle callbacks. */
    bool            available; /**< false when SD app is unavailable. */

    /* Convenience aliases — mirror manifest fields for existing callers. */
    app_id_t        app_id;   /**< == manifest.id */
    const char     *name;     /**< == manifest.name */
    const char     *icon;     /**< == manifest.icon */
} app_entry_t;

/* =========================================================================
 * C2 — Dynamic registry API
 * =========================================================================
 */

/** Initialize the registry. Call once before any register/get. */
void               app_registry_init(void);

/**
 * Register an app with the new manifest+ops API (C2).
 * Overwrites any prior registration for the same app_id.
 */
void               os_app_register(const app_manifest_t *manifest,
                                   const app_ops_t      *ops,
                                   const view_cbs_t     *cbs);

/**
 * Enumerate all registered apps (C2/C4).
 * @param cb   Callback invoked for each entry. Return false to stop iteration.
 * @param ctx  Passed through to cb unchanged.
 */
void os_app_enumerate(void (*cb)(const app_entry_t *entry, void *ctx), void *ctx);

/**
 * Legacy: register an app entry directly.
 * Fills manifest from entry's app_id/name/icon fields.
 * Kept for backward compatibility — prefer os_app_register for new code.
 */
void               app_registry_register(const app_entry_t *entry);

/**
 * Look up a registered app by ID.
 * @return Entry if on_create is set, NULL otherwise (stub/unimplemented).
 */
const app_entry_t *app_registry_get(app_id_t app_id);

/**
 * Look up by ID regardless of whether it has callbacks.
 * Use for display purposes (launcher) where stubs should still be shown.
 * @return Entry if name is set, NULL otherwise.
 */
const app_entry_t *app_registry_get_raw(app_id_t app_id);

#ifdef __cplusplus
}
#endif
