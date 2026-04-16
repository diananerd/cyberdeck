idf_component_register(
    SRCS "os_task.c"
         "os_process.c"
         "os_service.c"
         "os_defer.c"
         "os_event.c"
         "os_poller.c"
         "os_settings.c"
         "os_app_storage.c"
         "os_db.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_event esp_timer lvgl sys_services
)
