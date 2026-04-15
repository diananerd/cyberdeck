idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES board ui_engine esp_driver_usb_serial_jtag
)
