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
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

static const char *TAG = "link.ssd1306";

#define OLED_WIDTH   128
#define OLED_HEIGHT  64
#define OLED_FB_SIZE (OLED_WIDTH * OLED_HEIGHT / 8)

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static esp_lcd_panel_io_handle_t s_panel_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;

static SemaphoreHandle_t s_mutex = NULL;
static led_state_t s_state = LED_STATE_BOOT;
static char s_title[48] = {0};
static bool s_image_mode = false;

static led_voice_t s_voice = LED_VOICE_IDLE;
static float s_level = 0.0f;
static int s_volume = 0;
static int64_t s_volume_until = 0;

static uint8_t s_fb[OLED_FB_SIZE];

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

// ---- UI Components ---------------------------------------------------------

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

// ---- Render Frame ----------------------------------------------------------

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
        if (s_panel) {
            esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, OLED_WIDTH, OLED_HEIGHT, s_fb);
            if (err != ESP_OK) {
                int64_t now = esp_timer_get_time();
                if (now - last_err_time > 5000000) {
                    ESP_LOGW(TAG, "SSD1306 refresh err: %s", esp_err_to_name(err));
                    last_err_time = now;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50)); // 20 FPS refresh
    }
}

// ---- led_status.h Interface Implementation ---------------------------------

bool led_status_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return false;

    int sda = 8;
    int scl = 9;
#ifdef CONFIG_HOMEHUB_SSD1306_SDA_GPIO
    sda = CONFIG_HOMEHUB_SSD1306_SDA_GPIO;
#endif
#ifdef CONFIG_HOMEHUB_SSD1306_SCL_GPIO
    scl = CONFIG_HOMEHUB_SSD1306_SCL_GPIO;
#endif

    ESP_LOGI(TAG, "Initializing SSD1306 OLED (SDA=%d, SCL=%d)...", sda, scl);

    gpio_reset_pin((gpio_num_t)sda);
    gpio_reset_pin((gpio_num_t)scl);
    gpio_set_pull_mode((gpio_num_t)sda, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)scl, GPIO_PULLUP_ONLY);

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t)sda,
        .scl_io_num = (gpio_num_t)scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return false;
    }

    uint16_t dev_addr = 0x3C;
    if (i2c_master_probe(s_i2c_bus, 0x3C, 100) == ESP_OK) {
        dev_addr = 0x3C;
        ESP_LOGI(TAG, "Found SSD1306 at I2C address 0x3C");
    } else if (i2c_master_probe(s_i2c_bus, 0x3D, 100) == ESP_OK) {
        dev_addr = 0x3D;
        ESP_LOGI(TAG, "Found SSD1306 at I2C address 0x3D");
    } else {
        ESP_LOGW(TAG, "I2C probe returned no ACK at 0x3C/0x3D, trying 0x3C at 100kHz...");
        dev_addr = 0x3C;
    }

    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = dev_addr,
        .scl_speed_hz = 100 * 1000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            .dc_low_on_data = 0,
            .disable_control_phase = 0,
        },
    };
    err = esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_config, &s_panel_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_i2c failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .bits_per_pixel = 1,
    };
    esp_lcd_panel_ssd1306_config_t ssd1306_cfg = {
        .height = OLED_HEIGHT,
    };
    panel_cfg.vendor_config = &ssd1306_cfg;

    err = esp_lcd_new_panel_ssd1306(s_panel_io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_ssd1306 failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_mirror(s_panel, true, true);
    esp_lcd_panel_disp_on_off(s_panel, true);

    fb_clear();
    fb_draw_string_centered(26, "MUSE CHARM", true, 2);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, OLED_WIDTH, OLED_HEIGHT, s_fb);

    xTaskCreate(ssd1306_task, "ssd1306_task", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "SSD1306 OLED status ready: 128x64 I2C (addr=0x%02X)", dev_addr);
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
    if (s_panel) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, OLED_WIDTH, OLED_HEIGHT, s_fb);
    }
}

void led_status_show_animation(void) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_image_mode = false;
    xSemaphoreGive(s_mutex);
}
