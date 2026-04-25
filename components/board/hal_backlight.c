#include "hal_backlight.h"
#include "hal_ch422g.h"
#include "hal_pins.h"
#include "esp_log.h"

static const char *TAG = "hal_backlight";

esp_err_t hal_backlight_on(void)
{
    ESP_LOGI(TAG, "Backlight ON");
    hal_ch422g_write(HAL_CH422G_OC_ADDR, 0x01);
    /* TP_RST + BL + LCD_RST + SD_CS high, USB_SEL=0 (USB enabled) */
    return hal_ch422g_write(HAL_CH422G_OUT_ADDR,
        HAL_CH422G_BIT_TP_RST | HAL_CH422G_BIT_BL | HAL_CH422G_BIT_LCD_RST | HAL_CH422G_BIT_SD_CS);
}

esp_err_t hal_backlight_off(void)
{
    ESP_LOGI(TAG, "Backlight OFF");
    hal_ch422g_write(HAL_CH422G_OC_ADDR, 0x01);
    /* Same but BL off, USB_SEL=0 (USB enabled) */
    return hal_ch422g_write(HAL_CH422G_OUT_ADDR,
        HAL_CH422G_BIT_TP_RST | HAL_CH422G_BIT_LCD_RST | HAL_CH422G_BIT_SD_CS);
}

esp_err_t hal_backlight_set(bool on)
{
    return on ? hal_backlight_on() : hal_backlight_off();
}

static float s_level = 1.0f;

esp_err_t hal_backlight_set_level(float level)
{
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    s_level = level;
    /* CH422G EXIO2 is binary; quantise. Future board with a
     * PWM-capable BL line can use s_level for ledc duty. */
    return (level > 0.01f) ? hal_backlight_on() : hal_backlight_off();
}

float hal_backlight_get_level(void)
{
    return s_level;
}
