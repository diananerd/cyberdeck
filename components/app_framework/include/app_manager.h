/*
 * CyberDeck — App Manager
 * High-level API for launching apps and managing navigation.
 * Wires the app registry into the intent system at boot.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "ui_activity.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the app manager. Must be called after app_registry_init().
 * Registers the navigate callback with ui_intent so that ui_intent_navigate()
 * resolves app_ids through the registry.
 */
esp_err_t app_manager_init(void);

/**
 * Launch an app by ID. Takes the LVGL mutex internally.
 * Use this from non-LVGL contexts (event handlers, tasks).
 * From inside LVGL callbacks use ui_intent_navigate() directly.
 */
void app_manager_launch(uint8_t app_id, void *data, size_t data_size);

/** Pop the top activity (go back). Takes LVGL mutex. */
void app_manager_go_back(void);

/** Pop all activities to launcher (go home). Takes LVGL mutex. */
void app_manager_go_home(void);

/** Push the lockscreen on top. Takes LVGL mutex. */
void app_manager_lock(void);

/** Clear lock state after the lockscreen is dismissed. */
void app_manager_clear_lock(void);

/** Re-apply lock state (called from lockscreen on_create, e.g. after recreate_all). */
void app_manager_set_lock(void);

/**
 * Register the lockscreen activity callbacks.
 * Call from app_launcher_register() before app_manager_lock() is used.
 */
void app_manager_set_lockscreen_cbs(const activity_cbs_t *cbs);

#ifdef __cplusplus
}
#endif
