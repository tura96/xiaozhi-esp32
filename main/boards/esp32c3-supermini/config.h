#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// Audio sample rate (Duplex mode shares I2S BCLK/WS, rates must match)
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

// I2S duplex pin mapping (MAX98357A DAC Amp & INMP441 MEMS Mic)
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_6   // Shared BCLK / SCK
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_7   // Shared LRC / WS
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_5   // MAX98357A DIN
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_10  // INMP441 SD

// Mechanical Keys (Active LOW with internal pull-up)
#define BOOT_BUTTON_GPIO        GPIO_NUM_0  // Key 1: PTT Voice / Toggle Chat / Wi-Fi Config
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_1  // Key 2: Volume Up (+10%) / Max
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_3  // Key 3: Volume Down (-10%) / Mute

// 0.96" I2C OLED Display (SSD1306 128x64)
#define DISPLAY_SDA_PIN GPIO_NUM_8
#define DISPLAY_SCL_PIN GPIO_NUM_9
#define DISPLAY_WIDTH   128
#define DISPLAY_HEIGHT  64
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true

// Buzzer & Auxiliary
#define BUZZER_GPIO      GPIO_NUM_4
#define BUILTIN_LED_GPIO GPIO_NUM_NC  // GPIO 8 is used for OLED SDA, avoid pin conflict

#endif // _BOARD_CONFIG_H_
