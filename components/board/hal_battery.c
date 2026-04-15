/*
 * CyberDeck — Battery ADC driver
 * Reads battery voltage via ADC on GPIO 6.
 */

#include "hal_battery.h"
#include "hal_pins.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "hal_battery";

/* GPIO 6 = ADC1_CHANNEL_5 on ESP32-S3 */
#define BATTERY_ADC_UNIT        ADC_UNIT_1
#define BATTERY_ADC_CHANNEL     ADC_CHANNEL_5
#define BATTERY_ADC_ATTEN       ADC_ATTEN_DB_12

/* Voltage range for LiPo battery (mV) */
#define BATTERY_MV_FULL     4200
#define BATTERY_MV_EMPTY    3300

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_cali_handle = NULL;
static bool s_cali_enabled = false;

esp_err_t hal_battery_init(void)
{
    /* Init ADC oneshot unit */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BATTERY_ADC_UNIT,
    };
    esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure channel */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_oneshot_config_channel(s_adc_handle, BATTERY_ADC_CHANNEL, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Try to init calibration */
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BATTERY_ADC_UNIT,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle) == ESP_OK) {
        s_cali_enabled = true;
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = BATTERY_ADC_UNIT,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali_handle) == ESP_OK) {
        s_cali_enabled = true;
    }
#endif

    ESP_LOGI(TAG, "Battery ADC initialized (calibration: %s)",
             s_cali_enabled ? "yes" : "no");
    return ESP_OK;
}

esp_err_t hal_battery_read_mv(uint32_t *mv)
{
    if (!s_adc_handle || !mv) return ESP_ERR_INVALID_STATE;

    int raw = 0;
    esp_err_t ret = adc_oneshot_read(s_adc_handle, BATTERY_ADC_CHANNEL, &raw);
    if (ret != ESP_OK) return ret;

    if (s_cali_enabled) {
        int voltage = 0;
        ret = adc_cali_raw_to_voltage(s_cali_handle, raw, &voltage);
        if (ret == ESP_OK) {
            *mv = (uint32_t)voltage;
            return ESP_OK;
        }
    }

    /* Fallback: rough conversion without calibration
     * 12-bit ADC with 12dB attenuation: ~0-3100mV range */
    *mv = (uint32_t)((raw * 3100) / 4095);
    return ESP_OK;
}

esp_err_t hal_battery_read_pct(uint8_t *pct)
{
    if (!pct) return ESP_ERR_INVALID_ARG;

    uint32_t mv = 0;
    esp_err_t ret = hal_battery_read_mv(&mv);
    if (ret != ESP_OK) return ret;

    if (mv >= BATTERY_MV_FULL) {
        *pct = 100;
    } else if (mv <= BATTERY_MV_EMPTY) {
        *pct = 0;
    } else {
        *pct = (uint8_t)(((mv - BATTERY_MV_EMPTY) * 100) /
                          (BATTERY_MV_FULL - BATTERY_MV_EMPTY));
    }
    return ESP_OK;
}
