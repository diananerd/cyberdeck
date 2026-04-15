#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

esp_err_t hal_ch422g_init(void);
esp_err_t hal_ch422g_write(uint8_t addr, uint8_t data);

/* Get the shared I2C master bus handle (initialized in hal_ch422g_init) */
i2c_master_bus_handle_t hal_i2c_bus_handle(void);
