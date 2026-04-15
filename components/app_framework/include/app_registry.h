/*
 * S3 Cyber-Deck — App Registry
 * Central table of registered apps and their activity lifecycle callbacks.
 */

#pragma once

#include <stdint.h>
#include "ui_activity.h"
#include "os_core.h"   /* app_id_t, APP_ID_* constants */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- App IDs (legacy numeric aliases for count — IDs are in os_core.h) ---- */
#define APP_ID_COUNT  10

/** One entry per registered app. */
typedef struct {
    uint8_t        app_id;
    const char    *name;     /**< Display name, e.g. "Settings" */
    const char    *icon;     /**< Short string for launcher card, e.g. "St" */
    activity_cbs_t cbs;      /**< Lifecycle callbacks for main screen (screen_id == 0) */
} app_entry_t;

/** Initialize the registry. Call once before any register/get. */
void               app_registry_init(void);

/** Register an app. Overwrites any prior registration for the same app_id. */
void               app_registry_register(const app_entry_t *entry);

/**
 * Look up an app by ID.
 * @return Pointer to entry, or NULL if not registered or no on_create callback.
 */
const app_entry_t *app_registry_get(uint8_t app_id);

/**
 * Look up an app by ID regardless of whether it has callbacks.
 * Use this for display purposes (e.g. launcher) where stubs should be shown.
 * @return Pointer to entry if name is set, NULL otherwise.
 */
const app_entry_t *app_registry_get_raw(uint8_t app_id);

#ifdef __cplusplus
}
#endif
