/*
 * CyberDeck — System Monitor Service
 *
 * Agrega estado de múltiples fuentes en un snapshot consistente y lo
 * publica periódicamente vía EVT_MONITOR_UPDATED. Los clientes suscriben
 * ese evento y llaman svc_monitor_get_snapshot() — nunca pollan.
 *
 * Doble buffer ping-pong: el monitor escribe en el buffer "write", luego
 * intercambia atómicamente los índices. El cliente siempre lee el buffer
 * "read" que el monitor no está tocando.
 *
 * Fuentes:
 *   os_process_list()     → apps en ejecución
 *   os_service_list()     → servicios de core
 *   os_task_list()        → FreeRTOS tasks (solo con CYBERDECK_MONITOR_DEV_MODE)
 *   heap_caps_*           → memoria libre/total
 *   esp_timer_get_time()  → uptime
 */

#pragma once

#include "os_process.h"
#include "os_service.h"
#include "os_task.h"
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MON_MAX_APPS      12
#define MON_MAX_SERVICES  12

/* ---- Snapshot types ---- */

typedef struct {
    app_id_t     app_id;
    char         name[OS_PROCESS_NAME_LEN];
    proc_state_t state;
    uint8_t      view_count;
    uint8_t      bg_task_count;
    size_t       heap_delta;    /* heap_before - heap_now; aproximado */
    uint32_t     uptime_s;      /* segundos desde que el proceso fue lanzado */
} mon_app_entry_t;

/* mon_service_entry_t es un alias de os_service_entry_t */
typedef os_service_entry_t mon_service_entry_t;

typedef struct {
    char         name[OS_TASK_NAME_LEN];
    app_id_t     owner;
    uint32_t     stack_hwm;    /* palabras de stack libres (high water mark) */
    uint8_t      priority;
    uint8_t      core;
    uint8_t      rtos_state;   /* eTaskState — usar int para evitar include FreeRTOS en header */
} mon_task_entry_t;

typedef struct {
    /* Memoria */
    size_t   heap_internal_free;
    size_t   heap_internal_total;
    size_t   heap_psram_free;
    size_t   heap_psram_total;

    /* Apps en ejecución */
    mon_app_entry_t  apps[MON_MAX_APPS];
    uint8_t          app_count;

    /* Servicios de core */
    mon_service_entry_t services[MON_MAX_SERVICES];
    uint8_t             service_count;

    /* FreeRTOS tasks — solo con CONFIG_CYBERDECK_MONITOR_DEV_MODE=y */
    mon_task_entry_t tasks[OS_MAX_TASKS];
    uint8_t          task_count;   /* 0 en producción */

    /* Metadatos del snapshot */
    uint32_t uptime_s;       /* segundos desde boot */
    uint32_t snapshot_tick;  /* xTaskGetTickCount() al tomar el snapshot */
    uint16_t refresh_count;  /* número de snapshots tomados desde boot */
} sys_snapshot_t;

/* ---- API ---- */

/**
 * Inicializa el monitor y arranca la task de refresco.
 * Llamar en app_main() después de os_process_init() y os_service_init().
 * @param refresh_interval_ms Intervalo entre snapshots (recomendado: 2000).
 */
esp_err_t svc_monitor_init(uint32_t refresh_interval_ms);

/**
 * Retorna puntero al último snapshot completo.
 * Siempre válido tras svc_monitor_init(). El puntero no cambia — solo el contenido.
 * No retener entre eventos EVT_MONITOR_UPDATED — releer en cada notificación.
 */
const sys_snapshot_t *svc_monitor_get_snapshot(void);

/**
 * Fuerza un refresco inmediato (fuera del intervalo normal).
 * Útil al abrir la app Processes para tener datos frescos.
 */
void svc_monitor_force_refresh(void);

#ifdef __cplusplus
}
#endif
