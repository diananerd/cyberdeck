/*
 * CyberDeck — Task Manager app
 * Shows running FreeRTOS tasks via os_task_list().
 * Accessible from the launcher and from the navbar □ button.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the Task Manager app with the app registry.
 * Call during boot after app_registry_init().
 */
esp_err_t app_taskman_register(void);

#ifdef __cplusplus
}
#endif
