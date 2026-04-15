/*
 * S3 Cyber-Deck — OS Core: tipos base y constantes del sistema operativo.
 *
 * Este header define los tipos fundamentales que comparten todos los tracks
 * del refactor (task factory, event bus, registro dinámico de apps, storage).
 * No contiene implementación — solo tipos, constantes y macros.
 */

#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * App Identity
 * =========================================================================
 * IDs 0–255: apps built-in (definidas en tiempo de compilación).
 * IDs 256+:  apps dinámicas registradas en runtime desde SD.
 */
typedef uint16_t app_id_t;

#define OS_OWNER_SYSTEM     ((app_id_t)0xFFFF)  /* tasks/eventos del propio OS */
#define APP_ID_LAUNCHER     ((app_id_t)0)
#define APP_ID_BOOKS        ((app_id_t)1)
#define APP_ID_NOTES        ((app_id_t)2)
#define APP_ID_TASKS        ((app_id_t)3)
#define APP_ID_MUSIC        ((app_id_t)4)
#define APP_ID_PODCASTS     ((app_id_t)5)
#define APP_ID_CALC         ((app_id_t)6)
#define APP_ID_BLUESKY      ((app_id_t)7)
#define APP_ID_FILES        ((app_id_t)8)
#define APP_ID_SETTINGS     ((app_id_t)9)
#define APP_ID_BUILTIN_MAX  ((app_id_t)255)
#define APP_ID_DYNAMIC_BASE ((app_id_t)256)
#define APP_ID_INVALID      ((app_id_t)0xFFFE)

/* =========================================================================
 * Task Priorities
 * =========================================================================
 * Mapeadas a valores FreeRTOS relativos al configMAX_PRIORITIES del proyecto.
 * Úsalas siempre en lugar de números mágicos.
 */
#define OS_PRIO_LOW         (2)   /* pollers, background I/O */
#define OS_PRIO_MEDIUM      (5)   /* servicios del sistema (WiFi, battery) */
#define OS_PRIO_HIGH        (7)   /* descarga, OTA */
#define OS_PRIO_REALTIME    (10)  /* LVGL task — nunca bloquear */

/* =========================================================================
 * CPU Core Affinity
 * =========================================================================
 */
#define OS_CORE_BG          (0)   /* PRO_CPU — servicios de fondo */
#define OS_CORE_UI          (1)   /* APP_CPU — LVGL task (exclusivo) */
#define OS_CORE_ANY         (-1)  /* sin afinidad */

/* =========================================================================
 * Task Config
 * =========================================================================
 * Pasada a os_task_create() (implementado en Track A2).
 */
#define OS_TASK_NAME_LEN    (configMAX_TASK_NAME_LEN)
#define OS_MAX_TASKS        (32)

typedef struct {
    char        name[OS_TASK_NAME_LEN]; /* nombre de la task, para diagnóstico */
    TaskFunction_t fn;                  /* función de entrada */
    void       *arg;                    /* argumento pasado a fn */
    uint32_t    stack_size;             /* tamaño del stack en bytes */
    uint8_t     priority;              /* OS_PRIO_* */
    int8_t      core;                   /* OS_CORE_* */
    app_id_t    owner;                  /* app propietaria (OS_OWNER_SYSTEM si es del OS) */
    bool        stack_in_psram;         /* si true, alloca el stack en PSRAM vía SPIRAM caps */
} os_task_config_t;

/* =========================================================================
 * Error codes propios del OS
 * =========================================================================
 */
#define OS_ERR_STACK_FULL   (0x1001)  /* activity stack lleno, push rechazado */
#define OS_ERR_NOT_FOUND    (0x1002)  /* app_id no registrado */
#define OS_ERR_RUNNING      (0x1003)  /* app ya está en foreground */
#define OS_ERR_DENIED       (0x1004)  /* operación bloqueada (nav_lock activo) */

#ifdef __cplusplus
}
#endif
