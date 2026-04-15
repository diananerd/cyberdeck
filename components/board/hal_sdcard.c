/*
 * S3 Cyber-Deck — SD Card driver (SPI mode)
 * CS via CH422G EXIO4. USB_SEL (bit 5) MUST stay LOW.
 * Based on Waveshare SD example, adapted for new I2C API.
 */

#include "hal_sdcard.h"
#include "hal_pins.h"
#include "hal_ch422g.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include <sys/statvfs.h>

static const char *TAG = "hal_sdcard";

static sdmmc_card_t *s_card = NULL;
static sdmmc_host_t s_host = SDSPI_HOST_DEFAULT();
static bool s_mounted = false;

/*
 * SD card CS is on CH422G EXIO4 (bit 4 of OUT register).
 * The SDSPI driver uses a GPIO CS, but our CS is on an I/O expander.
 * Workaround: set CS LOW via CH422G before SPI init, and use CS = -1
 * in the SDSPI config (manual CS management).
 *
 * CH422G OUT bits we need active:
 *   bit 1: TP_RST  (touch reset, should be HIGH after init)
 *   bit 2: BL      (backlight ON)
 *   bit 3: LCD_RST (LCD reset, should be HIGH after init)
 *   bit 4: SD_CS   (LOW = selected for SPI)
 *   bit 5: USB_SEL (MUST be LOW = USB native)
 */

static esp_err_t sd_cs_low(void)
{
    /* Set SD_CS=0, keep BL, LCD_RST, TP_RST high, USB_SEL=0 */
    uint8_t val = HAL_CH422G_BIT_TP_RST | HAL_CH422G_BIT_BL | HAL_CH422G_BIT_LCD_RST;
    /* SD_CS bit is 0 (active low for SPI), USB_SEL bit is 0 */
    return hal_ch422g_write(HAL_CH422G_OUT_ADDR, val);
}

static esp_err_t sd_cs_high(void)
{
    /* Set SD_CS=1 (deselect), keep BL, LCD_RST, TP_RST high, USB_SEL=0 */
    uint8_t val = HAL_CH422G_BIT_TP_RST | HAL_CH422G_BIT_BL |
                  HAL_CH422G_BIT_LCD_RST | HAL_CH422G_BIT_SD_CS;
    return hal_ch422g_write(HAL_CH422G_OUT_ADDR, val);
}

esp_err_t hal_sdcard_mount(void)
{
    if (s_mounted) return ESP_OK;

    /* Pull CS low via CH422G to enable SD card */
    sd_cs_low();

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = HAL_SD_MOSI_PIN,
        .miso_io_num = HAL_SD_MISO_PIN,
        .sclk_io_num = HAL_SD_CLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t ret = spi_bus_initialize(s_host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        sd_cs_high();
        return ret;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = -1;           /* CS managed by CH422G, not GPIO */
    slot_cfg.host_id = s_host.slot;

    ret = esp_vfs_fat_sdspi_mount(HAL_SDCARD_MOUNT_POINT, &s_host,
                                   &slot_cfg, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        spi_bus_free(s_host.slot);
        sd_cs_high();
        return ret;
    }

    s_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD card mounted at %s", HAL_SDCARD_MOUNT_POINT);
    return ESP_OK;
}

esp_err_t hal_sdcard_unmount(void)
{
    if (!s_mounted) return ESP_OK;

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(HAL_SDCARD_MOUNT_POINT, s_card);
    spi_bus_free(s_host.slot);
    sd_cs_high();
    s_card = NULL;
    s_mounted = false;

    ESP_LOGI(TAG, "SD card unmounted");
    return ret;
}

bool hal_sdcard_is_mounted(void)
{
    return s_mounted;
}

esp_err_t hal_sdcard_get_space(uint32_t *total_kb, uint32_t *used_kb)
{
    if (!s_mounted || !total_kb || !used_kb) return ESP_ERR_INVALID_STATE;

    struct statvfs stat;
    if (statvfs(HAL_SDCARD_MOUNT_POINT, &stat) != 0) {
        return ESP_FAIL;
    }

    uint64_t total = (uint64_t)stat.f_blocks * stat.f_frsize / 1024;
    uint64_t free  = (uint64_t)stat.f_bfree  * stat.f_frsize / 1024;
    *total_kb = (uint32_t)total;
    *used_kb  = (uint32_t)(total - free);
    return ESP_OK;
}
