/*
 * CyberDeck — PCF85063A RTC driver
 * I2C real-time clock on the shared bus (addr 0x51).
 */

#include "hal_rtc.h"
#include "hal_ch422g.h"
#include "hal_pins.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include <sys/time.h>

static const char *TAG = "hal_rtc";

#define PCF85063A_ADDR      0x51
#define PCF85063A_REG_SEC   0x04    /* Seconds register (BCD) */

static i2c_master_dev_handle_t s_rtc_dev = NULL;

static uint8_t bcd_to_dec(uint8_t bcd)  { return (bcd >> 4) * 10 + (bcd & 0x0F); }
static uint8_t dec_to_bcd(uint8_t dec)  { return ((dec / 10) << 4) | (dec % 10); }

esp_err_t hal_rtc_init(void)
{
    i2c_master_bus_handle_t bus = hal_i2c_bus_handle();
    if (!bus) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063A_ADDR,
        .scl_speed_hz = HAL_I2C_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &cfg, &s_rtc_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add RTC device: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "PCF85063A RTC initialized");
    return ESP_OK;
}

esp_err_t hal_rtc_get_time(struct tm *t)
{
    if (!s_rtc_dev || !t) return ESP_ERR_INVALID_STATE;

    /* Read 7 bytes starting from seconds register */
    uint8_t reg = PCF85063A_REG_SEC;
    uint8_t data[7];

    esp_err_t ret = i2c_master_transmit_receive(s_rtc_dev, &reg, 1, data, 7,
                                                 HAL_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RTC read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    t->tm_sec  = bcd_to_dec(data[0] & 0x7F);   /* bit 7 = OS flag */
    t->tm_min  = bcd_to_dec(data[1] & 0x7F);
    t->tm_hour = bcd_to_dec(data[2] & 0x3F);    /* 24h mode */
    t->tm_mday = bcd_to_dec(data[3] & 0x3F);
    t->tm_wday = data[4] & 0x07;
    t->tm_mon  = bcd_to_dec(data[5] & 0x1F) - 1; /* struct tm: 0-11 */
    t->tm_year = bcd_to_dec(data[6]) + 100;       /* struct tm: years since 1900 */
    t->tm_isdst = -1;

    return ESP_OK;
}

esp_err_t hal_rtc_set_time(const struct tm *t)
{
    if (!s_rtc_dev || !t) return ESP_ERR_INVALID_STATE;

    uint8_t buf[8];
    buf[0] = PCF85063A_REG_SEC;
    buf[1] = dec_to_bcd(t->tm_sec);
    buf[2] = dec_to_bcd(t->tm_min);
    buf[3] = dec_to_bcd(t->tm_hour);
    buf[4] = dec_to_bcd(t->tm_mday);
    buf[5] = t->tm_wday & 0x07;
    buf[6] = dec_to_bcd(t->tm_mon + 1);         /* RTC: 1-12 */
    buf[7] = dec_to_bcd(t->tm_year % 100);      /* RTC: 0-99 */

    esp_err_t ret = i2c_master_transmit(s_rtc_dev, buf, 8, HAL_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RTC write failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t hal_rtc_sync_to_system(void)
{
    struct tm t;
    esp_err_t ret = hal_rtc_get_time(&t);
    if (ret != ESP_OK) return ret;

    time_t epoch = mktime(&t);
    if (epoch < 0) {
        ESP_LOGW(TAG, "RTC time invalid, skipping sync");
        return ESP_ERR_INVALID_STATE;
    }

    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "System time set from RTC: %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    return ESP_OK;
}

esp_err_t hal_rtc_sync_from_system(void)
{
    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);
    return hal_rtc_set_time(&t);
}
