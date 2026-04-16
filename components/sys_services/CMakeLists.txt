idf_component_register(
    SRCS "svc_event.c"
         "svc_settings.c"
         "svc_battery.c"
         "svc_wifi.c"
         "svc_time.c"
         "svc_downloader.c"
         "svc_ota.c"
         "svc_monitor.c"
         "os_storage.c"
         "os_crash.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event nvs_flash esp_wifi esp_netif esp_http_client esp_https_ota
             app_update board ui_engine os_core
    PRIV_REQUIRES esp_timer esp_adc
)
