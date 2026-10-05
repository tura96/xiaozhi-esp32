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

#include "led_status.h"
#include "pixel_font.h"
#include "stack_monitor.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

static const char *TAG = "link.ssd1306";

#define OLED_WIDTH   128
#define OLED_HEIGHT  64
#define OLED_FB_SIZE (OLED_WIDTH * OLED_HEIGHT / 8)

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_i2c_dev = NULL;
static uint16_t s_dev_addr = 0x3C;

static SemaphoreHandle_t s_mutex = NULL;
static led_state_t s_state = LED_STATE_BOOT;
static char s_title[48] = {0};
static bool s_image_mode = false;

static led_voice_t s_voice = LED_VOICE_IDLE;
static float s_level = 0.0f;
static int s_volume = 0;
static int64_t s_volume_until = 0;

static uint8_t s_fb[OLED_FB_SIZE];

// ---- Direct SSD1306 I2C Driver ---------------------------------------------

static esp_err_t ssd1306_send_cmd(uint8_t cmd) {
    if (!s_i2c_dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = {0x00, cmd}; // Co=0, D/C#=0 (Command)
    return i2c_master_transmit(s_i2c_dev, buf, sizeof(buf), 100);
}

static esp_err_t ssd1306_send_cmds(const uint8_t *cmds, size_t len) {
    if (!s_i2c_dev || !cmds || len == 0) return ESP_ERR_INVALID_STATE;
    uint8_t buf[64];
    buf[0] = 0x00;
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    memcpy(&buf[1], cmds, len);
    return i2c_master_transmit(s_i2c_dev, buf, len + 1, 100);
}

static esp_err_t ssd1306_hw_init(void) {
    static const uint8_t init_seq1[] = {
        0xAE,        // Display OFF
        0xD5, 0x80,  // Set Display Clock Divide Ratio
        0xA8, 0x3F,  // Set Multiplex Ratio (64 lines: 0x3F)
        0xD3, 0x00,  // Set Display Offset = 0
        0x40,        // Set Display Start Line = 0
        0x8D, 0x14,  // Enable Charge Pump (0x14)
        0x20, 0x00,  // Memory Addressing Mode: Horizontal
        0xA1,        // Segment Re-map: column 127 is mapped to SEG0 (mirror X)
        0xC8,        // COM Output Scan Direction: remapped mode (mirror Y)
        0xDA, 0x12,  // Set COM Pins Hardware Configuration
        0x81, 0xCF,  // Set Contrast Control
        0xD9, 0xF1,  // Set Pre-charge Period
        0xDB, 0x40,  // Set VCOMH Deselect Level
        0xA4,        // Entire Display ON (Resume to RAM content)
        0xA6,        // Set Normal Display (0xA6: Normal, 0xA7: Inverted)
        0xAF         // Display ON
    };

    esp_err_t err = ssd1306_send_cmds(init_seq1, sizeof(init_seq1));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306 init commands failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "SSD1306 hardware initialized successfully");
    return ESP_OK;
}

static esp_err_t ssd1306_flush(const uint8_t *fb) {
    if (!s_i2c_dev) return ESP_ERR_INVALID_STATE;

    // Set Column (0..127) and Page (0..7) Address Bounds
    uint8_t pos_cmds[] = {
        0x21, 0, 127,
        0x22, 0, 7
    };
    esp_err_t err = ssd1306_send_cmds(pos_cmds, sizeof(pos_cmds));
    if (err != ESP_OK) return err;

    // Send 8 pages of 128 bytes each
    uint8_t chunk[129];
    chunk[0] = 0x40; // Co=0, D/C#=1 (Data)
    for (int page = 0; page < 8; page++) {
        memcpy(&chunk[1], &fb[page * 128], 128);
        err = i2c_master_transmit(s_i2c_dev, chunk, sizeof(chunk), 100);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

// ---- Basic Drawing Primitives ----------------------------------------------

static void fb_clear(void) {
    memset(s_fb, 0, sizeof(s_fb));
}

static inline void fb_set_pixel(int x, int y, bool color) {
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    int page = y / 8;
    int bit = y % 8;
    int index = x + page * OLED_WIDTH;
    if (color) {
        s_fb[index] |= (1 << bit);
    } else {
        s_fb[index] &= ~(1 << bit);
    }
}

static void fb_draw_hline(int x, int y, int w, bool color) {
    for (int i = 0; i < w; i++) {
        fb_set_pixel(x + i, y, color);
    }
}

static void fb_draw_vline(int x, int y, int h, bool color) {
    for (int i = 0; i < h; i++) {
        fb_set_pixel(x, y + i, color);
    }
}

static void fb_draw_rect(int x, int y, int w, int h, bool color) {
    fb_draw_hline(x, y, w, color);
    fb_draw_hline(x, y + h - 1, w, color);
    fb_draw_vline(x, y, h, color);
    fb_draw_vline(x + w - 1, y, h, color);
}

static void fb_fill_rect(int x, int y, int w, int h, bool color) {
    for (int r = 0; r < h; r++) {
        fb_draw_hline(x, y + r, w, color);
    }
}

static void fb_draw_char(int x, int y, char c, bool color, int scale) {
    if (c < PIXEL_FONT_FIRST || c > PIXEL_FONT_LAST) c = '?';
    const uint8_t *glyph = pixel_font[c - PIXEL_FONT_FIRST];
    for (int col = 0; col < PIXEL_FONT_WIDTH; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < PIXEL_FONT_HEIGHT; row++) {
            if ((bits >> row) & 1) {
                if (scale == 1) {
                    fb_set_pixel(x + col, y + row, color);
                } else {
                    for (int sx = 0; sx < scale; sx++) {
                        for (int sy = 0; sy < scale; sy++) {
                            fb_set_pixel(x + col * scale + sx, y + row * scale + sy, color);
                        }
                    }
                }
            }
        }
    }
}

static void fb_draw_string(int x, int y, const char *str, bool color, int scale) {
    if (!str) return;
    int cur_x = x;
    int adv = (PIXEL_FONT_WIDTH + 1) * scale;
    while (*str) {
        if (cur_x + adv > OLED_WIDTH + adv) break;
        fb_draw_char(cur_x, y, *str, color, scale);
        cur_x += adv;
        str++;
    }
}

static void fb_draw_string_centered(int y, const char *str, bool color, int scale) {
    if (!str) return;
    int len = strlen(str);
    int adv = (PIXEL_FONT_WIDTH + 1) * scale;
    int w = len * adv - scale;
    int x = (OLED_WIDTH - w) / 2;
    if (x < 0) x = 0;
    fb_draw_string(x, y, str, color, scale);
}

static void fb_fill_circle(int x0, int y0, int r, bool color) {
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r) {
                fb_set_pixel(x0 + x, y0 + y, color);
            }
        }
    }
}

// ---- UI Layout & Rendering -------------------------------------------------

static void draw_top_bar(const char *state_tag, const char *title, bool dot_on) {
    fb_draw_hline(0, 11, OLED_WIDTH, true);

    if (state_tag && state_tag[0]) {
        fb_draw_string(2, 2, state_tag, true, 1);
    }

    if (title && title[0]) {
        int title_len = strlen(title);
        int adv = PIXEL_FONT_WIDTH + 1;
        int title_w = title_len * adv;
        int tx = OLED_WIDTH - 12 - title_w;
        if (tx < 50) tx = 50;
        fb_draw_string(tx, 2, title, true, 1);
    }

    if (dot_on) {
        fb_fill_circle(OLED_WIDTH - 5, 5, 2, true);
    } else {
        fb_set_pixel(OLED_WIDTH - 5, 5, true);
    }
}

static void draw_avatar(int cx, int cy, int anim_phase, bool mouth_open, bool happy) {
    int eye_spacing = 16;
    int left_eye_x = cx - eye_spacing;
    int right_eye_x = cx + eye_spacing;
    int eye_y = cy - 4;

    bool blinking = (anim_phase % 75) >= 71;

    if (blinking) {
        fb_draw_hline(left_eye_x - 5, eye_y, 11, true);
        fb_draw_hline(right_eye_x - 5, eye_y, 11, true);
    } else if (happy) {
        fb_draw_hline(left_eye_x - 4, eye_y - 2, 9, true);
        fb_draw_hline(left_eye_x - 5, eye_y - 1, 3, true);
        fb_draw_hline(left_eye_x + 3, eye_y - 1, 3, true);
        fb_draw_hline(left_eye_x - 6, eye_y, 2, true);
        fb_draw_hline(left_eye_x + 5, eye_y, 2, true);

        fb_draw_hline(right_eye_x - 4, eye_y - 2, 9, true);
        fb_draw_hline(right_eye_x - 5, eye_y - 1, 3, true);
        fb_draw_hline(right_eye_x + 3, eye_y - 1, 3, true);
        fb_draw_hline(right_eye_x - 6, eye_y, 2, true);
        fb_draw_hline(right_eye_x + 5, eye_y, 2, true);
    } else {
        fb_fill_circle(left_eye_x, eye_y, 5, true);
        fb_fill_circle(right_eye_x, eye_y, 5, true);
        fb_set_pixel(left_eye_x - 1, eye_y - 1, false);
        fb_set_pixel(right_eye_x - 1, eye_y - 1, false);
    }

    fb_fill_circle(left_eye_x - 9, eye_y + 6, 2, true);
    fb_fill_circle(right_eye_x + 9, eye_y + 6, 2, true);

    int mouth_y = cy + 9;
    if (mouth_open) {
        int mouth_h = 3 + (anim_phase % 4) * 2;
        fb_fill_rect(cx - 5, mouth_y - mouth_h / 2, 11, mouth_h, true);
    } else {
        fb_draw_hline(cx - 5, mouth_y, 11, true);
        fb_set_pixel(cx - 6, mouth_y - 1, true);
        fb_set_pixel(cx + 6, mouth_y - 1, true);
        fb_set_pixel(cx - 7, mouth_y - 2, true);
        fb_set_pixel(cx + 7, mouth_y - 2, true);
    }
}

static void draw_vu_meter(int y, float level, int frame) {
    int bar_w = 90;
    int bar_x = (OLED_WIDTH - bar_w) / 2;
    int filled = (int)(level * bar_w);
    if (filled > bar_w) filled = bar_w;
    if (filled < 4) filled = 4;

    int mid_y = y + 10;
    for (int x = bar_x; x < bar_x + bar_w; x += 4) {
        float wave = sinf((float)(x + frame * 8) * 0.2f) * (level * 12.0f + 2.0f);
        int h = (int)fabsf(wave);
        if (h < 1) h = 1;
        fb_draw_vline(x, mid_y - h, h * 2 + 1, true);
    }
}

static void render_screen_frame(int frame) {
    if (s_image_mode) {
        return;
    }

    fb_clear();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    led_state_t state = s_state;
    led_voice_t voice = s_voice;
    float level = s_level;
    int volume = s_volume;
    bool show_volume = esp_timer_get_time() < s_volume_until;
    char title_buf[48];
    strncpy(title_buf, s_title[0] ? s_title : "MUSE CHARM", sizeof(title_buf) - 1);
    title_buf[sizeof(title_buf) - 1] = '\0';
    xSemaphoreGive(s_mutex);

    if (show_volume) {
        draw_top_bar("VOL", title_buf, true);
        fb_draw_string_centered(20, "VOLUME", true, 1);
        char vol_str[16];
        snprintf(vol_str, sizeof(vol_str), "%d%%", volume);
        fb_draw_string_centered(32, vol_str, true, 2);
        int bx = 20, by = 50, bw = 88, bh = 8;
        fb_draw_rect(bx, by, bw, bh, true);
        int filled = (volume * (bw - 4)) / 100;
        if (filled > 0) fb_fill_rect(bx + 2, by + 2, filled, bh - 4, true);
        return;
    }

    if (voice != LED_VOICE_IDLE) {
        switch (voice) {
            case LED_VOICE_LISTENING:
                draw_top_bar("PTT", "LISTENING", true);
                fb_draw_string_centered(18, "Listening...", true, 1);
                draw_vu_meter(30, level, frame);
                fb_draw_string_centered(54, "Release when done", true, 1);
                break;
            case LED_VOICE_TRANSCRIBING:
                draw_top_bar("AI", "TRANSCRIBING", (frame / 5) % 2);
                fb_draw_string_centered(22, "Transcribing...", true, 1);
                int dots = (frame / 6) % 4;
                char dot_str[8] = "";
                for (int i = 0; i < dots; i++) strcat(dot_str, " .");
                fb_draw_string_centered(38, dot_str, true, 2);
                break;
            case LED_VOICE_THINKING:
                draw_top_bar("AI", "THINKING", true);
                draw_avatar(64, 34, frame, false, false);
                fb_draw_string_centered(54, "Thinking...", true, 1);
                break;
            case LED_VOICE_BUFFERING:
                draw_top_bar("AI", "RECEIVING", true);
                draw_avatar(64, 34, frame, true, true);
                fb_draw_string_centered(54, "Receiving reply...", true, 1);
                break;
            case LED_VOICE_SPEAKING:
                draw_top_bar("AI", "SPEAKING", (frame / 4) % 2);
                draw_avatar(64, 34, frame, true, true);
                fb_draw_string_centered(54, "Speaking...", true, 1);
                break;
            case LED_VOICE_ERROR:
                draw_top_bar("ERR", "VOICE ERROR", true);
                fb_draw_string_centered(26, "! VOICE ERROR !", true, 1);
                fb_draw_string_centered(42, "Try again", true, 1);
                break;
            default:
                break;
        }
        return;
    }

    switch (state) {
        case LED_STATE_BOOT:
            draw_top_bar("BOOT", "STARTING", true);
            fb_draw_string_centered(22, "MUSE CHARM", true, 2);
            fb_draw_string_centered(46, "ESP32-C3 AI Companion", true, 1);
            break;

        case LED_STATE_SETUP_IDLE:
        case LED_STATE_BLE_ADVERTISING:
            draw_top_bar("PAIR", "PAIRING", (frame / 10) % 2);
            fb_draw_string_centered(16, "Open Meta Muse App", true, 1);
            int r = 8 + (frame % 16) / 2;
            fb_draw_rect(64 - r, 36 - r / 2, r * 2, r, true);
            fb_draw_string_centered(48, "Connect Gadget", true, 1);
            break;

        case LED_STATE_BLE_CONNECTED:
            draw_top_bar("BLE", "CONNECTED", true);
            fb_draw_string_centered(20, "App Connected", true, 1);
            draw_avatar(64, 40, frame, false, true);
            break;

        case LED_STATE_PAIRING_CONFIRM_REQUIRED: {
            bool flash = (frame / 4) % 2;
            draw_top_bar("PAIR", "CONFIRM", flash);
            fb_draw_string_centered(16, "PAIRING REQUEST", true, 1);
            if (flash) {
                fb_draw_string_centered(30, "PRESS KEY 1", true, 2);
            }
            fb_draw_string_centered(50, "Tap BOOT to confirm", true, 1);
            break;
        }

        case LED_STATE_WIFI_CONNECTING:
            draw_top_bar("WIFI", "CONNECTING", (frame / 8) % 2);
            fb_draw_string_centered(22, "Connecting Wi-Fi...", true, 1);
            for (int i = 0; i < 4; i++) {
                int h = (i + 1) * 4;
                if ((frame / 6) % 5 > i) {
                    fb_fill_rect(54 + i * 6, 48 - h, 4, h, true);
                } else {
                    fb_draw_rect(54 + i * 6, 48 - h, 4, h, true);
                }
            }
            break;

        case LED_STATE_WIFI_CONNECTED:
        case LED_STATE_AUTH_OK:
        case LED_STATE_VM_SWITCHING:
        case LED_STATE_VM_OK:
        case LED_STATE_WS_CONNECTED:
            draw_top_bar("ONLINE", title_buf, true);
            draw_avatar(64, 32, frame, false, true);
            fb_draw_string_centered(54, "Hold BOOT to Talk", true, 1);
            break;

        case LED_STATE_WS_DISCONNECTED:
            draw_top_bar("WAIT", title_buf, false);
            draw_avatar(64, 32, frame, false, false);
            fb_draw_string_centered(54, "Reconnecting...", true, 1);
            break;

        case LED_STATE_UNPAIRED:
            draw_top_bar("UNPAIR", "RESET", true);
            fb_draw_string_centered(24, "Device Unpaired", true, 1);
            fb_draw_string_centered(40, "Ready to re-pair", true, 1);
            break;

        case LED_STATE_ERROR:
        default:
            draw_top_bar("ERR", "ERROR", true);
            fb_draw_string_centered(22, "! ERROR !", true, 2);
            fb_draw_string_centered(46, "Check connection", true, 1);
            break;
    }
}

// ---- FreeRTOS SSD1306 Task -------------------------------------------------

static void ssd1306_task(void *arg) {
    stack_monitor_t stack = STACK_MONITOR_INIT;
    int frame = 0;
    int64_t last_err_time = 0;

    while (1) {
        render_screen_frame(frame++);
        esp_err_t err = ssd1306_flush(s_fb);
        if (err != ESP_OK) {
            int64_t now = esp_timer_get_time();
            if (now - last_err_time > 5000000) {
                ESP_LOGW(TAG, "SSD1306 refresh err: %s (addr=0x%02X)", esp_err_to_name(err), s_dev_addr);
                last_err_time = now;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50)); // 20 FPS refresh
    }
}

// ---- led_status.h Interface Implementation ---------------------------------

static bool probe_pins_and_addr(int sda, int scl, uint16_t *out_addr, i2c_master_bus_handle_t *out_bus, i2c_master_dev_handle_t *out_dev) {
    gpio_reset_pin((gpio_num_t)sda);
    gpio_reset_pin((gpio_num_t)scl);
    gpio_set_direction((gpio_num_t)sda, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_direction((gpio_num_t)scl, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_pull_mode((gpio_num_t)sda, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)scl, GPIO_PULLUP_ONLY);

    i2c_master_bus_config_t bus_config = {
        .i2c_port = -1,
        .sda_io_num = (gpio_num_t)sda,
        .scl_io_num = (gpio_num_t)scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bus_config, &bus) != ESP_OK) {
        return false;
    }

    uint16_t addrs[] = {0x3C, 0x3D};
    for (int i = 0; i < 2; i++) {
        if (i2c_master_probe(bus, addrs[i], 50) == ESP_OK) {
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = addrs[i],
                .scl_speed_hz = 400000,
            };
            i2c_master_dev_handle_t dev = NULL;
            if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) == ESP_OK) {
                *out_addr = addrs[i];
                *out_bus = bus;
                *out_dev = dev;
                ESP_LOGI(TAG, "SUCCESS: Found OLED at 0x%02X on SDA=%d, SCL=%d", addrs[i], sda, scl);
                return true;
            }
        }
    }
    i2c_del_master_bus(bus);
    return false;
}

bool led_status_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return false;

    static const struct { int sda; int scl; } pairs[] = {
        {8, 9},   // Standard SuperMini OLED
        {9, 8},   // Inverted SDA/SCL
        {4, 5},   // Alt pair
        {5, 4},   // Alt pair
        {6, 7},   // I2S pins (in case shared)
        {10, 8},  // Alt
        {1, 0},   // Alt
        {0, 1}    // Alt
    };

    bool found = false;
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        ESP_LOGI(TAG, "Probing I2C SDA=%d, SCL=%d...", pairs[i].sda, pairs[i].scl);
        if (probe_pins_and_addr(pairs[i].sda, pairs[i].scl, &s_dev_addr, &s_i2c_bus, &s_i2c_dev)) {
            found = true;
            break;
        }
    }

    if (!found) {
        ESP_LOGW(TAG, "No I2C device ACKed on any pin pair! Defaulting to SDA=8, SCL=9 (0x3C)");
        (void)probe_pins_and_addr(8, 9, &s_dev_addr, &s_i2c_bus, &s_i2c_dev);
    }

    // Initialize SSD1306 hardware
    ssd1306_hw_init();
    fb_clear();
    fb_draw_string_centered(26, "MUSE CHARM", true, 2);
    ssd1306_flush(s_fb);

    xTaskCreate(ssd1306_task, "ssd1306_task", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "SSD1306 OLED initialized");
    return true;
}

void led_status_set_state(led_state_t state) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = state;
    xSemaphoreGive(s_mutex);
}

void led_status_set_title(const char *title) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (title) {
        strncpy(s_title, title, sizeof(s_title) - 1);
        s_title[sizeof(s_title) - 1] = '\0';
    } else {
        s_title[0] = '\0';
    }
    xSemaphoreGive(s_mutex);
}

void led_status_set_voice(led_voice_t voice) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_voice = voice;
    xSemaphoreGive(s_mutex);
}

void led_status_set_level(float level) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_level = level;
    xSemaphoreGive(s_mutex);
}

void led_status_show_volume(int percent) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_volume = percent;
    s_volume_until = esp_timer_get_time() + 1500000;
    xSemaphoreGive(s_mutex);
}

bool led_status_display_info(int *width, int *height) {
    if (width) *width = OLED_WIDTH;
    if (height) *height = OLED_HEIGHT;
    return true;
}

int led_status_display_bits(void) {
    return 1;
}

bool led_status_draw_rect(int x, int y, int w, int h, const uint16_t *pixels) {
    if (!pixels || !s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_image_mode = true;
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            uint16_t px = pixels[r * w + c];
            int red = (px >> 11) & 0x1F;
            int green = (px >> 5) & 0x3F;
            int blue = px & 0x1F;
            int luma = (red * 77 + green * 150 + blue * 29) >> 8;
            fb_set_pixel(x + c, y + r, luma > 16);
        }
    }
    xSemaphoreGive(s_mutex);
    return true;
}

void led_status_draw_done(void) {
    ssd1306_flush(s_fb);
}

void led_status_show_animation(void) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_image_mode = false;
    xSemaphoreGive(s_mutex);
}
