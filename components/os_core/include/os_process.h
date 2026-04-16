/*
 * CyberDeck — OS Core: Process Registry
 *
 * Mantiene un registro de instancias de app en ejecución, independiente
 * del stack LVGL. Cada entrada representa un "proceso" en el sentido OS:
 * una app lanzada, con su app_data (L1), sus tasks FreeRTOS y su estado.
 *
 * Ciclo de vida:
 *   os_process_start()  — llamado por app_manager al lanzar una app
 *   os_process_stop()   — llamado por app_manager al terminar una app
 *
 * Publica EVT_APP_LAUNCHED / EVT_APP_TERMINATED al bus de eventos.
 */

#pragma once

#include "os_core.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OS_MAX_PROCESSES  12   /* máximo de apps en ejecución simultánea */

typedef enum {
    PROC_STATE_STOPPED    = 0,
    PROC_STATE_RUNNING    = 1,  /* en foreground, pantalla activa */
    PROC_STATE_SUSPENDED  = 2,  /* HOME, pantalla oculta, bg tasks vivas */
    PROC_STATE_BACKGROUND = 3,  /* sin pantalla, solo bg tasks */
} proc_state_t;

#define OS_PROCESS_NAME_LEN  24

typedef struct {
    app_id_t      app_id;
    char          name[OS_PROCESS_NAME_LEN]; /* display name del app */
    proc_state_t  state;
    void         *app_data;      /* L1: datos de app, viven on_launch→on_terminate */
    uint32_t      launched_ms;   /* ms desde boot al momento de start */
    uint8_t       view_count;    /* pantallas LVGL activas en el stack para este app */
    uint8_t       task_count;    /* FreeRTOS tasks registradas con este owner */
    size_t        heap_before;   /* heap libre antes de on_launch (para delta) */
} os_process_t;

/**
 * Inicializa el registry. Llamar una sola vez en boot, antes de registrar apps.
 */
void os_process_init(void);

/**
 * Registra un proceso al lanzar una app.
 * @param id          App ID.
 * @param name        Nombre display del app (e.g. "Settings"). Puede ser NULL.
 * @param app_data    Puntero L1 retornado por app_ops.on_launch(). Puede ser NULL por ahora.
 * @param heap_before Heap libre antes de on_launch (snapshot para calcular delta).
 * @return ESP_OK | ESP_ERR_NO_MEM (registry lleno) | ESP_ERR_INVALID_STATE (ya existe)
 */
esp_err_t os_process_start(app_id_t id, const char *name, void *app_data, size_t heap_before);

/**
 * Elimina un proceso al terminar una app. No libera app_data (eso es tarea
 * de app_ops.on_terminate). Publica EVT_APP_TERMINATED.
 */
void os_process_stop(app_id_t id);

/**
 * Retorna puntero directo a la entrada del proceso, o NULL si no existe.
 * El puntero es válido mientras el proceso esté registrado — no cachear.
 */
os_process_t *os_process_get(app_id_t id);

/** True si la app tiene un proceso activo (cualquier estado != STOPPED). */
bool os_process_is_running(app_id_t id);

/** Actualiza el estado de un proceso. No-op si el proceso no existe. */
void os_process_set_state(app_id_t id, proc_state_t state);

/**
 * Actualiza los contadores de vistas y tasks del proceso.
 * Llamar desde ui_activity cuando el stack cambia.
 */
void os_process_update_counts(app_id_t id, uint8_t views, uint8_t tasks);

/**
 * Copia hasta max entradas activas en buf.
 * @return Número de entradas escritas.
 */
uint8_t os_process_list(os_process_t *buf, uint8_t max);

#ifdef __cplusplus
}
#endif
