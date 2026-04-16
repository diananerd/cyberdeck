/*
 * CyberDeck — App Manager
 * Bridges ui_intent -> app_registry, and provides navigation helpers
 * for use from non-LVGL task contexts.
 *
 * J6: Integra os_process_start/stop con el flujo de launch/terminate.
 */

#include "app_manager.h"
#include "app_registry.h"
#include "ui_activity.h"
#include "ui_intent.h"
#include "ui_engine.h"
#include "ui_effect.h"
#include "os_task.h"
#include "os_process.h"
#include "os_app_storage.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "app_manager";

static const view_cbs_t *s_lockscreen_cbs = NULL;
static bool              s_locked         = false;

/* ---------- H2 + J6: OS-level close hook ----------
 *
 * Contrato:
 *  - Llamado UNA VEZ por app_id que fue cerrada permanentemente (no en rotación).
 *  - Llamado desde LVGL task, después de que on_destroy ya liberó view_state.
 *  - Seguro llamar vTaskDelete aquí.
 */
static void on_app_closed(app_id_t app_id)
{
    /* 1. Capturar app_data del proceso ANTES de stop (stop lo zeroes) */
    os_process_t *proc = os_process_get(app_id);
    void *app_data = proc ? proc->app_data : NULL;

    /* 2. Llamar on_terminate (flush a DB, free(app_data)) */
    const app_entry_t *entry = app_registry_get_raw(app_id);
    if (entry && entry->ops.on_terminate) {
        entry->ops.on_terminate(app_id, app_data);
    }

    /* 3. Matar FreeRTOS tasks del app */
    os_task_destroy_all_for_app(app_id);

    /* 4. Eliminar proceso del registry */
    os_process_stop(app_id);

    ESP_LOGI(TAG, "App %d fully terminated", (int)app_id);
}

/* ---------- navigate_fn registered with ui_intent ---------- */

static bool manager_navigate_fn(const intent_t *intent)
{
    /* Singleton check: si ya está en el stack, raise en lugar de duplicar. */
    if (ui_activity_raise(intent->app_id)) {
        os_process_set_state(intent->app_id, PROC_STATE_RUNNING);
        ESP_LOGI(TAG, "App %d raised to front", (int)intent->app_id);
        return true;
    }

    const app_entry_t *app = app_registry_get(intent->app_id);
    if (!app) {
        ESP_LOGW(TAG, "App %d not registered", (int)intent->app_id);
        ui_effect_toast("Coming soon...", 1500);
        return false;
    }

    /* J6: Launch flow — on_launch → os_process_start → ui_activity_push */
    view_args_t args = {
        .data  = intent->data,
        .size  = intent->data_size,
        .owned = false,
    };
    const view_args_t *args_ptr = intent->data ? &args : NULL;

    /* Abrir storage (vacío si storage_dir == NULL) */
    os_app_storage_t storage = {0};
    os_app_storage_open(intent->app_id, app->manifest.storage_dir,
                        NULL, 0, &storage);

    /* on_launch: alloca L1 app_data, carga caché inicial desde DB */
    void *app_data = NULL;
    if (app->ops.on_launch) {
        size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        app_data = app->ops.on_launch(intent->app_id, args_ptr, &storage);
        if (!app_data && app->ops.on_launch) {
            /* on_launch retornó NULL → abort, no storage open to close here */
            ESP_LOGE(TAG, "App %d: on_launch returned NULL, aborting", (int)intent->app_id);
            os_app_storage_close(&storage);
            return false;
        }
        (void)heap_before;
    }

    /* Registrar proceso */
    size_t heap_snap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    esp_err_t rc = os_process_start(intent->app_id, app->manifest.name,
                                    app_data, heap_snap);
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        /* Registry lleno — continuar de todas formas, solo sin tracking */
        ESP_LOGW(TAG, "App %d: process_start failed (%s)", (int)intent->app_id,
                 esp_err_to_name(rc));
    }

    return ui_activity_push(intent->app_id, intent->screen_id,
                            &app->cbs, args_ptr);
}

/* ---------- Public API ---------- */

esp_err_t app_manager_init(void)
{
    ui_intent_set_navigate_fn(manager_navigate_fn);
    ui_activity_set_close_hook(on_app_closed);  /* H2: wire lifecycle → process cleanup */
    ESP_LOGI(TAG, "Initialized");
    return ESP_OK;
}

void app_manager_launch(app_id_t app_id, void *data, size_t data_size)
{
    if (ui_lock(500)) {
        intent_t intent = {
            .app_id    = app_id,
            .screen_id = 0,
            .data      = data,
            .data_size = data_size,
        };
        ui_intent_navigate(&intent);
        ui_unlock();
    }
}

void app_manager_go_back(void)
{
    if (s_locked) return;
    if (ui_lock(500)) {
        ui_activity_pop();
        ui_unlock();
    }
}

void app_manager_go_home(void)
{
    if (s_locked) return;
    if (ui_lock(500)) {
        ui_activity_suspend_to_home();  /* apps stay in background; CLOSE to free memory */
        ui_unlock();
    }
}

void app_manager_lock(void)
{
    if (!s_lockscreen_cbs) {
        ESP_LOGW(TAG, "No lockscreen registered");
        return;
    }
    s_locked = true;
    ui_activity_set_nav_lock(true);
    if (ui_lock(500)) {
        ui_activity_push(APP_ID_LAUNCHER, 1, s_lockscreen_cbs, NULL);
        ui_unlock();
    }
}

void app_manager_clear_lock(void)
{
    s_locked = false;
    ui_activity_set_nav_lock(false);
}

void app_manager_set_lock(void)
{
    s_locked = true;
    ui_activity_set_nav_lock(true);
}

void app_manager_set_lockscreen_cbs(const view_cbs_t *cbs)
{
    s_lockscreen_cbs = cbs;
}
