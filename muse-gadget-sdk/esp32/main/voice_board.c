/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "voice_board.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "link.voice_board";

// ESP32-C3 SuperMini Pin Configuration
#define PIN_I2S_BCLK      GPIO_NUM_6
#define PIN_I2S_WS        GPIO_NUM_7
#define PIN_MIC_DIN       GPIO_NUM_10  // INMP441 SD
#define PIN_SPK_DOUT      GPIO_NUM_5   // MAX98357A DIN

#define MIC_CHUNK_FRAMES  320
#define DEFAULT_VOLUME    80

static i2s_chan_handle_t s_tx_chan = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;
static int32_t *s_mic_raw = NULL;
static int s_volume = DEFAULT_VOLUME;
static bool s_mic_active = false;

esp_err_t voice_board_init(void) {
    ESP_LOGI(TAG, "Initializing ESP32-C3 Duplex I2S (BCLK=%d, WS=%d, DIN=%d, DOUT=%d)...",
             PIN_I2S_BCLK, PIN_I2S_WS, PIN_MIC_DIN, PIN_SPK_DOUT);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear_after_cb = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_MIC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_SPK_DOUT,
            .din = PIN_MIC_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    s_mic_raw = (int32_t *)malloc(MIC_CHUNK_FRAMES * 2 * sizeof(int32_t));
    if (!s_mic_raw) {
        ESP_LOGE(TAG, "Failed to allocate mic buffer");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ESP32-C3 I2S voice board ready (16 kHz duplex)");
    return ESP_OK;
}

esp_err_t voice_board_mic_start(void) {
    if (!s_rx_chan) return ESP_ERR_INVALID_STATE;
    if (!s_mic_active) {
        esp_err_t err = i2s_channel_enable(s_rx_chan);
        if (err == ESP_OK) s_mic_active = true;
        return err;
    }
    return ESP_OK;
}

void voice_board_mic_stop(void) {
    if (s_rx_chan && s_mic_active) {
        i2s_channel_disable(s_rx_chan);
        s_mic_active = false;
    }
}

size_t voice_board_mic_read(int16_t *pcm, size_t frames, int *peak) {
    *peak = 0;
    if (!s_rx_chan || !s_mic_raw || !pcm) return 0;
    if (frames > MIC_CHUNK_FRAMES) frames = MIC_CHUNK_FRAMES;

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_rx_chan, s_mic_raw, frames * 2 * sizeof(int32_t), &bytes_read, pdMS_TO_TICKS(100));
    if (err != ESP_OK || bytes_read == 0) return 0;

    size_t n = bytes_read / (2 * sizeof(int32_t));
    for (size_t i = 0; i < n; i++) {
        // INMP441 left channel (L/R to GND) is raw 24-bit MSB-aligned
        int32_t raw = s_mic_raw[2 * i];
        int16_t sample = (int16_t)(raw >> 14); // 24-bit to 16-bit with gain
        pcm[i] = sample;
        int a = sample < 0 ? -sample : sample;
        if (a > *peak) *peak = a;
    }
    return n;
}

esp_err_t voice_board_speaker_write(const int32_t *frames, size_t count) {
    if (!s_tx_chan || !frames || count == 0) return ESP_ERR_INVALID_STATE;
    size_t written = 0;
    return i2s_channel_write(s_tx_chan, frames, count * 2 * sizeof(int32_t), &written, pdMS_TO_TICKS(500));
}

void voice_board_amp(bool on) {
    (void)on;
}

void voice_board_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
}

bool voice_board_muted(void) {
    return false;
}

int voice_board_dial_steps(void) {
    return 0;
}
