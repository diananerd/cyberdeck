idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES board ui_engine sys_services app_framework launcher settings
             os_core esp_driver_usb_serial_jtag
)
