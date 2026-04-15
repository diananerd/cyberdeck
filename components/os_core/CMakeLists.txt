idf_component_register(
    SRCS "os_task.c"
         "os_defer.c"
         "os_event.c"
         "os_poller.c"
         "os_settings.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_event esp_timer lvgl sys_services
)
