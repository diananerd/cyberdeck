#include "hal_ch422g.h"
#include "hal_pins.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "hal_ch422g";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_ch422g_oc = NULL;   /* OC addr 0x24 */
static i2c_master_dev_handle_t s_ch422g_out = NULL;   /* OUT addr 0x38 */

i2c_master_bus_handle_t hal_i2c_bus_handle(void)
{
    return s_i2c_bus;
}

esp_err_t hal_ch422g_init(void)
{
    /* Initialize I2C master bus (shared by CH422G, GT911, RTC) */
    i2c_master_bus_config_t bus_config = {
        .i2c_port = HAL_I2C_PORT,
        .sda_io_num = HAL_I2C_SDA_PIN,
        .scl_io_num = HAL_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &s_i2c_bus));
    ESP_LOGI(TAG, "I2C bus initialized");

    /* Add CH422G OC device (0x24) */
    i2c_device_config_t oc_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = HAL_CH422G_OC_ADDR,
        .scl_speed_hz = HAL_I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &oc_cfg, &s_ch422g_oc));

    /* Add CH422G OUT device (0x38) */
    i2c_device_config_t out_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = HAL_CH422G_OUT_ADDR,
        .scl_speed_hz = HAL_I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &out_cfg, &s_ch422g_out));

    /* Set CH422G to output mode */
    uint8_t write_buf = 0x01;
    esp_err_t ret = i2c_master_transmit(s_ch422g_oc, &write_buf, 1, HAL_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init CH422G: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "CH422G initialized");
    }
    return ret;
}

esp_err_t hal_ch422g_write(uint8_t addr, uint8_t data)
{
    i2c_master_dev_handle_t dev;
    if (addr == HAL_CH422G_OC_ADDR) {
        dev = s_ch422g_oc;
    } else if (addr == HAL_CH422G_OUT_ADDR) {
        dev = s_ch422g_out;
    } else {
        /* For other addresses, add a temporary device */
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = HAL_I2C_FREQ_HZ,
        };
        i2c_master_dev_handle_t tmp = NULL;
        esp_err_t ret = i2c_master_bus_add_device(s_i2c_bus, &cfg, &tmp);
        if (ret != ESP_OK) return ret;
        ret = i2c_master_transmit(tmp, &data, 1, HAL_I2C_TIMEOUT_MS);
        i2c_master_bus_rm_device(tmp);
        return ret;
    }
    return i2c_master_transmit(dev, &data, 1, HAL_I2C_TIMEOUT_MS);
}
