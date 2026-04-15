/*
 * S3 Cyber-Deck — OS Core: Event Bus con ownership por app
 *
 * Extiende svc_event con:
 *  - Tracking de owner por suscripción.
 *  - os_event_unsubscribe_all() para limpieza automática al terminar una app.
 *  - os_event_subscribe_ui() que entrega el evento en el LVGL task (sin boilerplate lock).
 *
 * Las APIs antiguas (svc_event_register / svc_event_unregister) siguen funcionando
 * como wrappers — backward compatible durante la migración.
 */

#pragma once

#include "os_core.h"
#include "svc_event.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Handle opaco de suscripción. -1 = inválido. */
typedef int event_sub_t;
#define EVENT_SUB_INVALID  (-1)

/**
 * Suscribe un handler con ownership.
 * @param owner    App propietaria de la suscripción (OS_OWNER_SYSTEM para el OS).
 * @param evt      Event ID a escuchar.
 * @param handler  Callback esp_event estándar.
 * @param ctx      Contexto pasado al handler.
 * @param out_sub  Handle de suscripción (para unsubscribe manual). Puede ser NULL.
 * @return ESP_OK o error.
 */
esp_err_t os_event_subscribe(app_id_t owner,
                             cyberdeck_event_id_t evt,
                             esp_event_handler_t handler,
                             void *ctx,
                             event_sub_t *out_sub);

/**
 * Suscribe un handler que se ejecuta en el LVGL task (via os_ui_post).
 * El handler se llama con el mutex LVGL ya tomado — no se necesita ui_lock().
 * Reemplaza el patrón manual de double-check + ui_lock en event handlers UI.
 *
 * @param owner   App propietaria.
 * @param evt     Event ID.
 * @param handler Callback — se ejecuta en el LVGL task con LVGL mutex tomado.
 * @param ctx     Contexto pasado al handler.
 * @return Handle de suscripción o EVENT_SUB_INVALID en error.
 */
event_sub_t os_event_subscribe_ui(app_id_t owner,
                                  cyberdeck_event_id_t evt,
                                  esp_event_handler_t handler,
                                  void *ctx);

/**
 * Cancela una suscripción específica.
 */
esp_err_t os_event_unsubscribe(event_sub_t sub);

/**
 * Cancela todas las suscripciones de una app.
 * Llamar desde on_destroy o antes de terminar la app.
 */
void os_event_unsubscribe_all(app_id_t owner);

#ifdef __cplusplus
}
#endif
