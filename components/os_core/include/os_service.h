/*
 * CyberDeck — OS Core: Service Registry
 *
 * Registro de servicios de sistema (svc_wifi, svc_battery, svc_time, etc.).
 * Cada servicio se registra en su init() y actualiza su estado/texto de status
 * conforme cambia su condición interna.
 *
 * El monitor (svc_monitor, I3) consume este registry para construir snapshots.
 * Las apps nunca consultan esto directamente — usan el snapshot del monitor.
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OS_SERVICE_NAME_LEN   24
#define OS_SERVICE_STATUS_LEN 32
#define OS_MAX_SERVICES       12

typedef enum {
    SVC_STATE_INIT    = 0,   /* recién registrado, aún no operativo */
    SVC_STATE_RUNNING = 1,   /* operativo, activamente haciendo trabajo */
    SVC_STATE_IDLE    = 2,   /* operativo, sin trabajo activo */
    SVC_STATE_ERROR   = 3,   /* fallo recuperable */
    SVC_STATE_OFFLINE = 4,   /* recurso no disponible (SD, WiFi, HW) */
} svc_state_t;

typedef struct {
    char        name[OS_SERVICE_NAME_LEN];
    svc_state_t state;
    char        status_text[OS_SERVICE_STATUS_LEN]; /* "192.168.1.5", "78%", "NTP ok" */
} os_service_entry_t;

/**
 * Inicializa el registry. Llamar una sola vez en boot.
 */
void os_service_init(void);

/**
 * Registra un servicio. Idempotente — si el nombre ya existe, no hace nada.
 * Estado inicial: SVC_STATE_INIT, status_text vacío.
 * @param name Nombre del servicio (e.g. "svc_wifi"). Truncado a OS_SERVICE_NAME_LEN-1.
 */
void os_service_register(const char *name);

/**
 * Actualiza el estado y texto de status de un servicio registrado.
 * Si el nombre no existe, loguea un warning y no hace nada.
 * @param name        Nombre del servicio.
 * @param state       Nuevo estado.
 * @param status_text Texto descriptivo (NULL → vacío). Truncado a OS_SERVICE_STATUS_LEN-1.
 */
void os_service_update(const char *name, svc_state_t state, const char *status_text);

/**
 * Copia hasta max entradas del registry en buf.
 * Solo incluye servicios previamente registrados (name[0] != '\0').
 * @return Número de entradas escritas.
 */
uint8_t os_service_list(os_service_entry_t *buf, uint8_t max);

#ifdef __cplusplus
}
#endif
