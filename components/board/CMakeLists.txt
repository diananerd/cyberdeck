idf_component_register(
    SRCS "hal_ch422g.c" "hal_backlight.c" "hal_lcd.c"
         "hal_rtc.c" "hal_battery.c" "hal_sdcard.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2c esp_driver_gpio esp_lcd
             espressif__esp_lcd_touch espressif__esp_lcd_touch_gt911
    PRIV_REQUIRES esp_timer esp_adc fatfs sdmmc
)
