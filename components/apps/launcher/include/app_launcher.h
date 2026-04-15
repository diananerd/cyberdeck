/*
 * CyberDeck — Launcher App
 * Home screen grid + lock screen.
 */

#pragma once

#include "esp_err.h"
#include "ui_activity.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register launcher (and lockscreen) with the app registry and app_manager.
 * Call once during boot, after app_registry_init() and app_manager_init().
 */
esp_err_t app_launcher_register(void);

/**
 * Get the launcher activity callbacks for pushing the initial home screen.
 * Used by main.c to push the first activity.
 */
const activity_cbs_t *app_launcher_get_cbs(void);

#ifdef __cplusplus
}
#endif
