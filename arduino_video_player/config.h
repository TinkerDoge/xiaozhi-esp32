#ifndef _DOGEPET_CONFIG_H_
#define _DOGEPET_CONFIG_H_

#include <driver/gpio.h>

// Pins
#define BUILTIN_LED_GPIO        48
#define BOOT_BUTTON_GPIO        0

// Buttons
#define BUTTON_A_GPIO           39
#define BUTTON_B_GPIO           40

#define DISPLAY_BACKLIGHT_PIN   5
#define DISPLAY_MISO_PIN        4
#define DISPLAY_MOSI_PIN        2
#define DISPLAY_CLK_PIN         3
#define DISPLAY_DC_PIN          7
#define DISPLAY_RST_PIN         -1  // No reset pin (NC)
#define DISPLAY_CS_PIN          6

// SD card (shares SPI3_HOST bus with display, separate CS)
#define SD_CARD_CS_PIN          8
#define SD_SPI_HOST             SPI3_HOST
#define SD_MOUNT_POINT          "/sdcard"

// Video player defaults
#define VIDEO_FPS               15
#define VIDEO_DIR               "/sdcard"

// Choose your display resolution variant by uncommenting one of the following blocks:
#define CONFIG_LCD_ST7789_240X320
// #define CONFIG_LCD_ST7789_240X280
// #define CONFIG_LCD_ST7789_240X240

#ifdef CONFIG_LCD_ST7789_240X320
#define DISPLAY_WIDTH                   240
#define DISPLAY_HEIGHT                  320
#define DISPLAY_MIRROR_X                false
#define DISPLAY_MIRROR_Y                false
#define DISPLAY_SWAP_XY                 false
#define DISPLAY_INVERT_COLOR            true
#define DISPLAY_RGB_ORDER               0 // 0 = RGB, 1 = BGR (used by esp_lcd)
#define DISPLAY_OFFSET_X                0
#define DISPLAY_OFFSET_Y                0
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE                0
#endif

#ifdef CONFIG_LCD_ST7789_240X280
#define DISPLAY_WIDTH                   240
#define DISPLAY_HEIGHT                  280
#define DISPLAY_MIRROR_X                false
#define DISPLAY_MIRROR_Y                false
#define DISPLAY_SWAP_XY                 false
#define DISPLAY_INVERT_COLOR            true
#define DISPLAY_RGB_ORDER               0
#define DISPLAY_OFFSET_X                0
#define DISPLAY_OFFSET_Y                20
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE                0
#endif

#ifdef CONFIG_LCD_ST7789_240X240
#define DISPLAY_WIDTH                   240
#define DISPLAY_HEIGHT                  240
#define DISPLAY_MIRROR_X                false
#define DISPLAY_MIRROR_Y                false
#define DISPLAY_SWAP_XY                 false
#define DISPLAY_INVERT_COLOR            true
#define DISPLAY_RGB_ORDER               0
#define DISPLAY_OFFSET_X                0
#define DISPLAY_OFFSET_Y                0
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE                0
#endif

#endif // _DOGEPET_CONFIG_H_
