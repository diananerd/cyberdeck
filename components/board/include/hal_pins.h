#pragma once

#include "driver/gpio.h"

/* I2C bus (shared: GT911, CH422G, PCF85063A RTC) */
#define HAL_I2C_SDA_PIN         GPIO_NUM_8
#define HAL_I2C_SCL_PIN         GPIO_NUM_9
#define HAL_I2C_PORT            I2C_NUM_0
#define HAL_I2C_FREQ_HZ         400000
#define HAL_I2C_TIMEOUT_MS      1000

/* RGB LCD sync signals */
#define HAL_LCD_VSYNC_PIN       GPIO_NUM_3
#define HAL_LCD_HSYNC_PIN       GPIO_NUM_46
#define HAL_LCD_DE_PIN          GPIO_NUM_5
#define HAL_LCD_PCLK_PIN        GPIO_NUM_7
#define HAL_LCD_DISP_PIN        (-1)

/* RGB LCD 16-bit data bus */
#define HAL_LCD_DATA0_PIN       GPIO_NUM_14
#define HAL_LCD_DATA1_PIN       GPIO_NUM_38
#define HAL_LCD_DATA2_PIN       GPIO_NUM_18
#define HAL_LCD_DATA3_PIN       GPIO_NUM_17
#define HAL_LCD_DATA4_PIN       GPIO_NUM_10
#define HAL_LCD_DATA5_PIN       GPIO_NUM_39
#define HAL_LCD_DATA6_PIN       GPIO_NUM_0
#define HAL_LCD_DATA7_PIN       GPIO_NUM_45
#define HAL_LCD_DATA8_PIN       GPIO_NUM_48
#define HAL_LCD_DATA9_PIN       GPIO_NUM_47
#define HAL_LCD_DATA10_PIN      GPIO_NUM_21
#define HAL_LCD_DATA11_PIN      GPIO_NUM_1
#define HAL_LCD_DATA12_PIN      GPIO_NUM_2
#define HAL_LCD_DATA13_PIN      GPIO_NUM_42
#define HAL_LCD_DATA14_PIN      GPIO_NUM_41
#define HAL_LCD_DATA15_PIN      GPIO_NUM_40

/* LCD parameters */
#define HAL_LCD_H_RES           800
#define HAL_LCD_V_RES           480
#define HAL_LCD_PIXEL_CLK_HZ    (16 * 1000 * 1000)
#define HAL_LCD_BIT_PER_PIXEL   16
#define HAL_LCD_DATA_WIDTH      16

/* Touch reset control GPIO */
#define HAL_GPIO4_PIN           GPIO_NUM_4

/* SD Card SPI */
#define HAL_SD_MOSI_PIN         GPIO_NUM_11
#define HAL_SD_CLK_PIN          GPIO_NUM_12
#define HAL_SD_MISO_PIN         GPIO_NUM_13
/* SD CS is via CH422G EXIO4, not a direct GPIO */

/* UART0 (USB-C serial via CH343) */
#define HAL_UART0_TX_PIN        GPIO_NUM_43
#define HAL_UART0_RX_PIN        GPIO_NUM_44

/* UART1 (BT module, recycled from RS485) */
#define HAL_BT_UART_TX_PIN      GPIO_NUM_15
#define HAL_BT_UART_RX_PIN      GPIO_NUM_16

/* Free GPIOs (recycled from CAN, USB OTG capable) */
#define HAL_FREE_GPIO19         GPIO_NUM_19
#define HAL_FREE_GPIO20         GPIO_NUM_20

/* ADC (battery monitoring) */
#define HAL_BATTERY_ADC_PIN     GPIO_NUM_6

/* CH422G I2C addresses */
#define HAL_CH422G_OC_ADDR      0x24
#define HAL_CH422G_OUT_ADDR     0x38

/* CH422G OUT register bit map (active-high) */
#define HAL_CH422G_BIT_TP_RST   (1 << 1)   /* EXIO1: Touch reset */
#define HAL_CH422G_BIT_BL       (1 << 2)   /* EXIO2: Backlight */
#define HAL_CH422G_BIT_LCD_RST  (1 << 3)   /* EXIO3: LCD reset */
#define HAL_CH422G_BIT_SD_CS    (1 << 4)   /* EXIO4: SD card CS */
#define HAL_CH422G_BIT_USB_SEL  (1 << 5)   /* EXIO5: USB mux (0=USB, 1=CAN) */
