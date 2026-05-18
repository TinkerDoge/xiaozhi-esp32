#ifndef _DOGEPET_CONFIG_H_
#define _DOGEPET_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// Flash/PSRAM info (documentary; sizing is from sdkconfig)
// 4MB flash, 2MB PSRAM (QUAD)

// If you are using Duplex I2S mode, comment the following line
#define AUDIO_I2S_METHOD_DUPLEX

#ifdef AUDIO_I2S_METHOD_SIMPLEX

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_NC
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_16
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_17
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_13
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_33
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_17
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

#else

// Shared I2S clock pins for both microphone and speaker
#define AUDIO_I2S_GPIO_WS GPIO_NUM_16   // Shared WS/LRCLK
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_17 // Shared BCLK
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_13 // INMP441 SD pin
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_33 // MAX98357A DIN pin
// Dual INMP441: one L/R=GND (left), one L/R=VCC (right) — read both slots

#endif

#define BUILTIN_LED_GPIO        GPIO_NUM_48
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define TOUCH_BUTTON_GPIO       GPIO_NUM_NC
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC

// Buttons
#define BUTTON_A_GPIO           GPIO_NUM_39
#define BUTTON_B_GPIO           GPIO_NUM_40

#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_5
#define DISPLAY_MISO_PIN      GPIO_NUM_4
#define DISPLAY_MOSI_PIN      GPIO_NUM_2
#define DISPLAY_CLK_PIN       GPIO_NUM_3
#define DISPLAY_DC_PIN        GPIO_NUM_7
#define DISPLAY_RST_PIN       GPIO_NUM_NC
#define DISPLAY_CS_PIN        GPIO_NUM_6

// Audio quality enhancements
//#define AUDIO_INPUT_GAIN        2.0f    // Microphone gain multiplier (1.0-4.0 recommended)
//#define AUDIO_HPF_CUTOFF_HZ     80      // High-pass filter cutoff to remove low-freq noise
//#define AUDIO_NOISE_GATE_THRESH 200     // Noise gate threshold (0=off, 100-500 typical)
//#define AUDIO_OUTPUT_LIMITER    true    // Enable soft limiting to prevent clipping

//#define AUDIO_PA_CTRL_GPIO      GPIO_NUM_NC  // PA power control (optional)
//#define AUDIO_CODEC_PA_PIN      GPIO_NUM_NC  // Same as PA_CTRL for compatibility
//#endif



// IMU removed to save space

// VBAT ADC (optional). If connected through divider to a GPIO with ADC1 channel,
// you can wire it here. Example uses ADC1_CH0 on GPIO1 or customize as needed.
#define VBAT_ADC_UNIT   ADC_UNIT_2
#define VBAT_ADC_CH     ADC_CHANNEL_4  // GPIO15 on ESP32-S3
#define VBAT_UPPER_R    1000000.0f  // 1M ohm
#define VBAT_LOWER_R    1000000.0f  // 1M ohm
/* No charge detect pin for now */

// SD card (shares SPI3_HOST bus with display, separate CS)
#define SD_CARD_CS_PIN        GPIO_NUM_8
#define SD_SPI_HOST           SPI3_HOST
#define SD_MOUNT_POINT        "/sdcard"

// Video player defaults
#define VIDEO_FPS             15
#define VIDEO_DIR             "/sdcard"

// I2C (optional, for sensors or expansion)
#define I2C_SDA_PIN           GPIO_NUM_21
#define I2C_SCL_PIN           GPIO_NUM_18
//MPU6050 connected but not having any function atm
//#define MPU6050_I2C_SCL GPIO_NUM_18
//#define MPU6050_I2C_SDA GPIO_NUM_21 

#ifdef CONFIG_LCD_ST7789_240X320
#define LCD_TYPE_ST7789_SERIAL
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  320
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false
#define DISPLAY_INVERT_COLOR    true
#define DISPLAY_RGB_ORDER  LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE 0
#endif

#ifdef CONFIG_LCD_ST7789_240X280
#define LCD_TYPE_ST7789_SERIAL
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  280
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false
#define DISPLAY_INVERT_COLOR    true
#define DISPLAY_RGB_ORDER  LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  20
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE 0
#endif

#ifdef CONFIG_LCD_ST7789_240X240
#define LCD_TYPE_ST7789_SERIAL
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  240
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false
#define DISPLAY_INVERT_COLOR    true
#define DISPLAY_RGB_ORDER  LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_SPI_MODE 0
#endif

#endif // _DOGEPET_CONFIG_H_
