/*
 * CyberDeck — OS Core: Deferred execution
 *
 * os_defer  — ejecuta fn(arg) una vez después de delay_ms (usa esp_timer one-shot).
 * os_ui_post — ejecuta fn(arg) en el LVGL task en el siguiente tick (usa lv_async_call).
 *              Reemplaza el patrón: if (ui_lock(200)) { update_widget(); ui_unlock(); }
 *
 * Thread-safety:
 *   os_defer y os_ui_post son seguros llamarlos desde cualquier task.
 *   El callback de os_ui_post se ejecuta ya con el mutex de LVGL tomado.
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*os_fn_t)(void *arg);

/**
 * Ejecuta fn(arg) una vez después de delay_ms.
 * Internamente crea un esp_timer one-shot. El timer se autodestruye al disparar.
 * @return ESP_OK o ESP_ERR_NO_MEM si no hay timers disponibles.
 */
esp_err_t os_defer(os_fn_t fn, void *arg, uint32_t delay_ms);

/**
 * Ejecuta fn(arg) en el LVGL task en el siguiente tick (lv_async_call).
 * No requiere ui_lock/ui_unlock en el handler — ya está tomado.
 * Compatible con lv_async_call signature: fn recibe (void *arg).
 */
void os_ui_post(os_fn_t fn, void *arg);

#ifdef __cplusplus
}
#endif
