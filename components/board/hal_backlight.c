#include "hal_backlight.h"
#include "hal_ch422g.h"
#include "hal_pins.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if defined(CONFIG_HAL_BACKLIGHT_PWM_GPIO) && CONFIG_HAL_BACKLIGHT_PWM_GPIO >= 0
#include "driver/ledc.h"
#define BL_USE_PWM 1
#define BL_PWM_GPIO     CONFIG_HAL_BACKLIGHT_PWM_GPIO
#define BL_PWM_FREQ_HZ  CONFIG_HAL_BACKLIGHT_PWM_FREQ_HZ
#define BL_PWM_RES      LEDC_TIMER_10_BIT
#define BL_PWM_MAX_DUTY ((1U << 10) - 1U)
#define BL_PWM_TIMER    LEDC_TIMER_0
#define BL_PWM_CHAN     LEDC_CHANNEL_0
#define BL_PWM_MODE     LEDC_LOW_SPEED_MODE
#else
#define BL_USE_PWM 0
#endif

static const char *TAG = "hal_backlight";
static float s_level = 1.0f;

#if BL_USE_PWM
static bool s_pwm_inited = false;

static esp_err_t bl_pwm_init(void)
{
    if (s_pwm_inited) return ESP_OK;
    ledc_timer_config_t t = {
        .speed_mode      = BL_PWM_MODE,
        .timer_num       = BL_PWM_TIMER,
        .duty_resolution = BL_PWM_RES,
        .freq_hz         = BL_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t e = ledc_timer_config(&t);
    if (e != ESP_OK) return e;
    ledc_channel_config_t c = {
        .gpio_num   = BL_PWM_GPIO,
        .speed_mode = BL_PWM_MODE,
        .channel    = BL_PWM_CHAN,
        .timer_sel  = BL_PWM_TIMER,
        .duty       = 0,
        .hpoint     = 0,
        .intr_type  = LEDC_INTR_DISABLE,
    };
    e = ledc_channel_config(&c);
    if (e != ESP_OK) return e;
    s_pwm_inited = true;
    ESP_LOGI(TAG, "PWM init: gpio=%d freq=%dHz res=10bit", BL_PWM_GPIO, BL_PWM_FREQ_HZ);
    return ESP_OK;
}

static esp_err_t bl_pwm_set_duty(uint32_t duty)
{
    esp_err_t e = ledc_set_duty(BL_PWM_MODE, BL_PWM_CHAN, duty);
    if (e != ESP_OK) return e;
    return ledc_update_duty(BL_PWM_MODE, BL_PWM_CHAN);
}
#endif

static esp_err_t bl_ch422g_on(void)
{
    hal_ch422g_write(HAL_CH422G_OC_ADDR, 0x01);
    return hal_ch422g_write(HAL_CH422G_OUT_ADDR,
        HAL_CH422G_BIT_TP_RST | HAL_CH422G_BIT_BL | HAL_CH422G_BIT_LCD_RST | HAL_CH422G_BIT_SD_CS);
}

static esp_err_t bl_ch422g_off(void)
{
    hal_ch422g_write(HAL_CH422G_OC_ADDR, 0x01);
    return hal_ch422g_write(HAL_CH422G_OUT_ADDR,
        HAL_CH422G_BIT_TP_RST | HAL_CH422G_BIT_LCD_RST | HAL_CH422G_BIT_SD_CS);
}

esp_err_t hal_backlight_on(void)
{
    ESP_LOGI(TAG, "Backlight ON");
    s_level = 1.0f;
#if BL_USE_PWM
    if (bl_pwm_init() == ESP_OK) return bl_pwm_set_duty(BL_PWM_MAX_DUTY);
#endif
    return bl_ch422g_on();
}

esp_err_t hal_backlight_off(void)
{
    ESP_LOGI(TAG, "Backlight OFF");
    s_level = 0.0f;
#if BL_USE_PWM
    if (bl_pwm_init() == ESP_OK) return bl_pwm_set_duty(0);
#endif
    return bl_ch422g_off();
}

esp_err_t hal_backlight_set(bool on)
{
    return on ? hal_backlight_on() : hal_backlight_off();
}

esp_err_t hal_backlight_set_level(float level)
{
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    s_level = level;
#if BL_USE_PWM
    if (bl_pwm_init() == ESP_OK) {
        uint32_t duty = (uint32_t)(level * (float)BL_PWM_MAX_DUTY + 0.5f);
        if (duty > BL_PWM_MAX_DUTY) duty = BL_PWM_MAX_DUTY;
        return bl_pwm_set_duty(duty);
    }
#endif
    /* CH422G EXIO2 is binary — quantise. */
    return (level > 0.01f) ? bl_ch422g_on() : bl_ch422g_off();
}

float hal_backlight_get_level(void)
{
    return s_level;
}
