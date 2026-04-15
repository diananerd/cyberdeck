/*
 * CyberDeck — Event bus service
 * Wraps esp_event with a dedicated cyberdeck event loop.
 */

#include "svc_event.h"
#include "esp_log.h"

static const char *TAG = "svc_event";

ESP_EVENT_DEFINE_BASE(CYBERDECK_EVENT);

static esp_event_loop_handle_t s_loop = NULL;

/* Getter interno para os_event.c — no exponer en el header público. */
esp_event_loop_handle_t svc_event_get_loop(void) { return s_loop; }

esp_err_t svc_event_init(void)
{
    if (s_loop != NULL) {
        return ESP_OK; /* already initialized */
    }

    esp_event_loop_args_t args = {
        .queue_size      = 16,
        .task_name       = "cyberdeck_evt",
        .task_priority   = 4,
        .task_stack_size = 4096,
        .task_core_id    = 0,
    };

    esp_err_t ret = esp_event_loop_create(&args, &s_loop);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Event loop created");
    } else {
        ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t svc_event_post(cyberdeck_event_id_t event_id,
                         const void *data, size_t data_size)
{
    if (!s_loop) return ESP_ERR_INVALID_STATE;
    return esp_event_post_to(s_loop, CYBERDECK_EVENT, (int32_t)event_id,
                             data, data_size, pdMS_TO_TICKS(100));
}

esp_err_t svc_event_register(cyberdeck_event_id_t event_id,
                             esp_event_handler_t handler, void *ctx)
{
    if (!s_loop) return ESP_ERR_INVALID_STATE;
    return esp_event_handler_register_with(s_loop, CYBERDECK_EVENT,
                                           (int32_t)event_id, handler, ctx);
}

esp_err_t svc_event_register_all(esp_event_handler_t handler, void *ctx)
{
    if (!s_loop) return ESP_ERR_INVALID_STATE;
    return esp_event_handler_register_with(s_loop, CYBERDECK_EVENT,
                                           ESP_EVENT_ANY_ID, handler, ctx);
}

esp_err_t svc_event_unregister(cyberdeck_event_id_t event_id,
                               esp_event_handler_t handler)
{
    if (!s_loop) return ESP_ERR_INVALID_STATE;
    return esp_event_handler_unregister_with(s_loop, CYBERDECK_EVENT,
                                             (int32_t)event_id, handler);
}
