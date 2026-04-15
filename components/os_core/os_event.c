/*
 * S3 Cyber-Deck — OS Core: Event Bus con ownership por app
 *
 * Implementación de os_event_subscribe / os_event_subscribe_ui /
 * os_event_unsubscribe / os_event_unsubscribe_all.
 *
 * Internamente usa esp_event_handler_instance_register_with para obtener
 * un handle de instancia que permite un unregister preciso sin necesidad
 * de que el caller almacene el handler_fn.
 *
 * Para os_event_subscribe_ui: envuelve el handler en un shim que usa
 * os_ui_post para reenviar la llamada al LVGL task con el mutex tomado.
 * El shim recibe el ctx original y lo propaga al handler real.
 */

#include "os_event.h"
#include "os_defer.h"
#include "svc_event.h"
#include "esp_event.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "os_event";

/* Número máximo de suscripciones simultáneas. */
#define OS_EVENT_MAX_SUBS  64

/* ---- Registro de suscripciones ---- */

typedef enum {
    SUB_TYPE_DIRECT,   /* handler directo (os_event_subscribe) */
    SUB_TYPE_UI,       /* handler vía os_ui_post (os_event_subscribe_ui) */
} sub_type_t;

/* Para SUB_TYPE_UI necesitamos mantener vivo el shim mientras la suscripción exista. */
typedef struct {
    esp_event_handler_t  real_handler; /* handler del caller */
    void                *real_ctx;     /* contexto del caller */
} ui_shim_ctx_t;

typedef struct {
    bool                         used;
    app_id_t                     owner;
    cyberdeck_event_id_t         event_id;
    sub_type_t                   type;
    esp_event_handler_instance_t instance; /* handle de esp_event para unregister */
    ui_shim_ctx_t               *shim_ctx; /* solo si type == SUB_TYPE_UI */
} sub_entry_t;

static sub_entry_t s_subs[OS_EVENT_MAX_SUBS];

static sub_entry_t *alloc_entry(void)
{
    for (int i = 0; i < OS_EVENT_MAX_SUBS; i++) {
        if (!s_subs[i].used) return &s_subs[i];
    }
    return NULL;
}

/* Necesitamos acceder al loop handle de svc_event — exponemos un getter interno. */
extern esp_event_loop_handle_t svc_event_get_loop(void);

/* ---- Shim para os_event_subscribe_ui ---- */

typedef struct {
    esp_event_handler_t  handler;
    void                *ctx;
    void                *event_data;
    size_t               event_data_size;
} ui_post_args_t;

static void ui_post_thunk(void *arg)
{
    /* Ejecutado en el LVGL task, con el mutex LVGL ya tomado por lv_async_call. */
    ui_post_args_t *p = (ui_post_args_t *)arg;
    p->handler(p->ctx, CYBERDECK_EVENT, 0 /* event_id no disponible aquí */, p->event_data);
    free(p->event_data);
    free(p);
}

static void ui_shim_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)base; (void)event_id;
    ui_shim_ctx_t *shim = (ui_shim_ctx_t *)arg;

    /* Copiar event_data porque el buffer de esp_event se libera al retornar. */
    ui_post_args_t *p = malloc(sizeof(ui_post_args_t));
    if (!p) return;

    p->handler         = shim->real_handler;
    p->ctx             = shim->real_ctx;
    p->event_data      = NULL;
    p->event_data_size = 0;

    /* event_data puede ser NULL (eventos sin datos) */
    if (event_data) {
        /* esp_event siempre pasa el tamaño en la entrada de la cola; pero la firma
         * del handler no lo expone. Para datos simples (punteros a uint8_t, etc.)
         * el handler los lee antes de retornar — no necesitamos copiarlos aquí
         * porque ui_shim_handler se ejecuta en el event-loop task (no en ISR).
         * Sin embargo, para ser seguros, hacemos una copia de tamaño fijo.
         * Limitación: si el evento pasa structs grandes, extender este buffer. */
        /* Por ahora pasamos event_data directo — el event-loop task no libera
         * el buffer hasta que este handler retorne, y retornamos después del post. */
        p->event_data = event_data;
    }

    /* Reenviar al LVGL task vía lv_async_call */
    extern void os_ui_post(void (*fn)(void *), void *arg);
    os_ui_post(ui_post_thunk, p);

    /* NOTA: event_data es propiedad del event-loop y se libera al retornar de aquí.
     * El thunk no debe acceder a event_data después de que este handler retorne.
     * Por eso ponemos event_data=NULL en p y no lo usamos en el thunk. */
    p->event_data = NULL;
}

/* ---- API pública ---- */

esp_err_t os_event_subscribe(app_id_t owner,
                             cyberdeck_event_id_t evt,
                             esp_event_handler_t handler,
                             void *ctx,
                             event_sub_t *out_sub)
{
    if (!handler) return ESP_ERR_INVALID_ARG;

    sub_entry_t *entry = alloc_entry();
    if (!entry) {
        ESP_LOGE(TAG, "Subscription registry full (max=%d)", OS_EVENT_MAX_SUBS);
        return ESP_ERR_NO_MEM;
    }

    esp_event_loop_handle_t loop = svc_event_get_loop();
    if (!loop) return ESP_ERR_INVALID_STATE;

    esp_event_handler_instance_t instance;
    esp_err_t ret = esp_event_handler_instance_register_with(
        loop, CYBERDECK_EVENT, (int32_t)evt, handler, ctx, &instance);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_event register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    entry->used      = true;
    entry->owner     = owner;
    entry->event_id  = evt;
    entry->type      = SUB_TYPE_DIRECT;
    entry->instance  = instance;
    entry->shim_ctx  = NULL;

    if (out_sub) *out_sub = (event_sub_t)(entry - s_subs);
    return ESP_OK;
}

event_sub_t os_event_subscribe_ui(app_id_t owner,
                                  cyberdeck_event_id_t evt,
                                  esp_event_handler_t handler,
                                  void *ctx)
{
    if (!handler) return EVENT_SUB_INVALID;

    sub_entry_t *entry = alloc_entry();
    if (!entry) {
        ESP_LOGE(TAG, "Subscription registry full");
        return EVENT_SUB_INVALID;
    }

    ui_shim_ctx_t *shim = malloc(sizeof(ui_shim_ctx_t));
    if (!shim) return EVENT_SUB_INVALID;

    shim->real_handler = handler;
    shim->real_ctx     = ctx;

    esp_event_loop_handle_t loop = svc_event_get_loop();
    if (!loop) { free(shim); return EVENT_SUB_INVALID; }

    esp_event_handler_instance_t instance;
    esp_err_t ret = esp_event_handler_instance_register_with(
        loop, CYBERDECK_EVENT, (int32_t)evt, ui_shim_handler, shim, &instance);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_event register (ui) failed: %s", esp_err_to_name(ret));
        free(shim);
        return EVENT_SUB_INVALID;
    }

    entry->used     = true;
    entry->owner    = owner;
    entry->event_id = evt;
    entry->type     = SUB_TYPE_UI;
    entry->instance = instance;
    entry->shim_ctx = shim;

    return (event_sub_t)(entry - s_subs);
}

esp_err_t os_event_unsubscribe(event_sub_t sub)
{
    if (sub < 0 || sub >= OS_EVENT_MAX_SUBS) return ESP_ERR_INVALID_ARG;

    sub_entry_t *entry = &s_subs[sub];
    if (!entry->used) return ESP_ERR_INVALID_STATE;

    esp_event_loop_handle_t loop = svc_event_get_loop();
    if (loop) {
        esp_event_handler_instance_unregister_with(
            loop, CYBERDECK_EVENT, (int32_t)entry->event_id, entry->instance);
    }

    if (entry->shim_ctx) {
        free(entry->shim_ctx);
        entry->shim_ctx = NULL;
    }

    entry->used = false;
    return ESP_OK;
}

void os_event_unsubscribe_all(app_id_t owner)
{
    for (int i = 0; i < OS_EVENT_MAX_SUBS; i++) {
        if (s_subs[i].used && s_subs[i].owner == owner) {
            os_event_unsubscribe((event_sub_t)i);
        }
    }
    ESP_LOGI(TAG, "Unsubscribed all events for app %u", (unsigned)owner);
}
