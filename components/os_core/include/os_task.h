/*
 * CyberDeck — OS Core: Task Factory API
 */

#pragma once

#include "os_core.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * FreeRTOS task info snapshot — usado por el monitor del sistema.
 */
typedef struct {
    char         name[OS_TASK_NAME_LEN];
    TaskHandle_t handle;
    app_id_t     owner;
    uint32_t     stack_high_water; /* palabras libres en el stack */
    uint8_t      priority;
    uint8_t      core;
    bool         is_killable;      /* false = OS_OWNER_SYSTEM, no se puede terminar */
} os_task_info_t;

/**
 * Crea una task y la registra en el OS.
 * @param cfg        Configuración de la task (ver os_core.h).
 * @param out_handle Handle de la task creada, o NULL si no se necesita.
 * @return ESP_OK, ESP_ERR_NO_MEM, ESP_ERR_INVALID_ARG, ESP_FAIL.
 */
esp_err_t os_task_create(const os_task_config_t *cfg, TaskHandle_t *out_handle);

/**
 * Destruye una task y la elimina del registro.
 */
esp_err_t os_task_destroy(TaskHandle_t handle);

/**
 * Destruye todas las tasks registradas con el owner dado.
 * Llamar desde os_app_terminate() en el future.
 */
void os_task_destroy_all_for_app(app_id_t id);

/**
 * Devuelve una snapshot de las tasks activas (para el Task Manager).
 * @param buf Buffer donde escribir los registros.
 * @param max Capacidad máxima del buffer.
 * @return Número de entradas escritas.
 */
uint8_t os_task_list(os_task_info_t *buf, uint8_t max);

#ifdef __cplusplus
}
#endif
