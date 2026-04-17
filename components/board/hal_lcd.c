#include "hal_lcd.h"
#include "hal_pins.h"
#include "hal_ch422g.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"

static const char *TAG = "hal_lcd";

/* RGB panel framebuffer config — double-buffered, 10-line bounce buffer. */
#define HAL_LCD_RGB_NUM_FBS          2
#define HAL_LCD_BOUNCE_BUFFER_LINES  10

/* VSYNC callback — registered by the UI layer (bridge) when present.
 * DL1 platforms with no display leave this NULL; the ISR then returns
 * false (no LVGL refresh pending).
 */
static hal_lcd_vsync_cb_t s_vsync_cb = NULL;

void hal_lcd_set_vsync_cb(hal_lcd_vsync_cb_t cb)
{
    s_vsync_cb = cb;
}

/* Touch reset sequence via CH422G + GPIO4 */
static void touch_reset(void)
{
    /* Configure GPIO4 as output */
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << HAL_GPIO4_PIN),
    };
    gpio_config(&io_conf);

    /* CH422G output mode */
    hal_ch422g_write(HAL_CH422G_OC_ADDR, 0x01);

    /* Reset touch — keep USB_SEL=0 (USB enabled) throughout */
    uint8_t out = HAL_CH422G_BIT_LCD_RST | HAL_CH422G_BIT_SD_CS;  /* TP_RST low, BL off */
    hal_ch422g_write(HAL_CH422G_OUT_ADDR, out);
    esp_rom_delay_us(100 * 1000);
    gpio_set_level(HAL_GPIO4_PIN, 0);
    esp_rom_delay_us(100 * 1000);
    out |= HAL_CH422G_BIT_TP_RST;  /* Release TP_RST */
    hal_ch422g_write(HAL_CH422G_OUT_ADDR, out);
    esp_rom_delay_us(200 * 1000);

    ESP_LOGI(TAG, "Touch reset complete");
}

/* VSYNC callback — fans out to the registered UI-layer hook, if any. */
IRAM_ATTR static bool on_vsync(esp_lcd_panel_handle_t panel,
                                const esp_lcd_rgb_panel_event_data_t *edata,
                                void *user_ctx)
{
    (void)panel; (void)edata; (void)user_ctx;
    return s_vsync_cb ? s_vsync_cb() : false;
}

esp_err_t hal_lcd_init(esp_lcd_panel_handle_t *panel_handle, esp_lcd_touch_handle_t *tp_handle)
{
    ESP_LOGI(TAG, "Installing RGB LCD panel driver");

    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = HAL_LCD_PIXEL_CLK_HZ,
            .h_res = HAL_LCD_H_RES,
            .v_res = HAL_LCD_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = 1,
        },
        .data_width = HAL_LCD_DATA_WIDTH,
        .num_fbs = HAL_LCD_RGB_NUM_FBS,
        .bounce_buffer_size_px = HAL_LCD_H_RES * HAL_LCD_BOUNCE_BUFFER_LINES,
        .dma_burst_size = 64,
        .hsync_gpio_num = HAL_LCD_HSYNC_PIN,
        .vsync_gpio_num = HAL_LCD_VSYNC_PIN,
        .de_gpio_num = HAL_LCD_DE_PIN,
        .pclk_gpio_num = HAL_LCD_PCLK_PIN,
        .disp_gpio_num = HAL_LCD_DISP_PIN,
        .data_gpio_nums = {
            HAL_LCD_DATA0_PIN, HAL_LCD_DATA1_PIN, HAL_LCD_DATA2_PIN, HAL_LCD_DATA3_PIN,
            HAL_LCD_DATA4_PIN, HAL_LCD_DATA5_PIN, HAL_LCD_DATA6_PIN, HAL_LCD_DATA7_PIN,
            HAL_LCD_DATA8_PIN, HAL_LCD_DATA9_PIN, HAL_LCD_DATA10_PIN, HAL_LCD_DATA11_PIN,
            HAL_LCD_DATA12_PIN, HAL_LCD_DATA13_PIN, HAL_LCD_DATA14_PIN, HAL_LCD_DATA15_PIN,
        },
        .flags.fb_in_psram = 1,
    };

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(*panel_handle));

    /* Touch init */
    ESP_LOGI(TAG, "Initializing GT911 touch controller");
    touch_reset();

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(hal_i2c_bus_handle(), &tp_io_config, &tp_io_handle));

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = HAL_LCD_H_RES,
        .y_max = HAL_LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, tp_handle));

    /* Register VSYNC callback */
    esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_vsync = on_vsync,
    };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(*panel_handle, &cbs, NULL));

    ESP_LOGI(TAG, "LCD and touch initialized successfully");
    return ESP_OK;
}
