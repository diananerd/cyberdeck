/*
 * S3 Cyber-Deck — OS Core: Poller
 *
 * Un único FreeRTOS task (`os_poller_task`) que ejecuta funciones periódicas
 * de forma cooperativa, reemplazando N tasks del patrón:
 *   while(1) { do_work(); vTaskDelay(interval); }
 *
 * Ahorra stack (~3 KB por task eliminada) y centraliza el scheduling de fondo.
 *
 * Uso:
 *   os_poller_register("battery", poll_battery, NULL, 30000, OS_OWNER_SYSTEM);
 *   os_poller_start();  // llamar una sola vez desde app_main
 *
 * Restricciones:
 *   - Los callbacks deben ser no-bloqueantes (o bloquear <100 ms).
 *   - No llamar os_poller_register después de os_poller_start (no thread-safe).
 */

#pragma once

#include "os_core.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*os_poll_fn_t)(void *arg);

/** Número máximo de pollers registrados. */
#define OS_POLLER_MAX  16

/**
 * Registra una función periódica en el poller.
 * @param name        Nombre para diagnóstico (truncado a 15 chars).
 * @param fn          Función a llamar periódicamente.
 * @param arg         Argumento pasado a fn.
 * @param interval_ms Intervalo en milisegundos.
 * @param owner       App propietaria (OS_OWNER_SYSTEM para tasks del OS).
 * @return ESP_OK o ESP_ERR_NO_MEM si no hay slots disponibles.
 */
esp_err_t os_poller_register(const char *name, os_poll_fn_t fn, void *arg,
                             uint32_t interval_ms, app_id_t owner);

/**
 * Inicia el poller task. Llamar una vez, después de todos los registros.
 * @return ESP_OK o error de creación de task.
 */
esp_err_t os_poller_start(void);

/**
 * Cancela todos los pollers de una app (para cleanup al terminarla).
 */
void os_poller_remove_all_for_app(app_id_t owner);

#ifdef __cplusplus
}
#endif
