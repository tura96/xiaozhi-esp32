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

#include "button.h"
#include "stack_monitor.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "link.button";

#define BTN_GPIO           CONFIG_HOMEHUB_BUTTON_GPIO // Key 1 / BOOT (GPIO 0)
#define KEY2_GPIO          1                          // Key 2 (GPIO 1)
#define KEY3_GPIO          3                          // Key 3 (GPIO 3)

#define LONG_PRESS_MS      5000
#define SHORT_PRESS_MAX_MS 1000
#define DOUBLE_CLICK_MS    400
#define POLL_MS            40

static button_cb s_short_press_cb = NULL;
static button_cb s_double_press_cb = NULL;
static button_cb s_long_press_cb = NULL;
static button_cb s_key2_cb = NULL;
static button_cb s_key3_cb = NULL;

#if CONFIG_HOMEHUB_VOICE
static volatile button_press_cb s_press_cb = NULL;

void button_set_press_cb(button_press_cb cb) {
    s_press_cb = cb;
}
#endif

void button_set_aux_callbacks(button_cb on_key2, button_cb on_key3) {
    s_key2_cb = on_key2;
    s_key3_cb = on_key3;
}

static void button_task(void *arg) {
    stack_monitor_t stack = STACK_MONITOR_INIT;
    bool was_pressed1 = false;
    bool was_pressed2 = false;
    bool was_pressed3 = false;
    int64_t press_start1 = 0;
    bool fired1 = false;
    int click_count1 = 0;
    int64_t last_release1 = 0;

#if CONFIG_HOMEHUB_VOICE
    bool claimed = false;
#endif

    while (1) {
        // Key 1 (GPIO 0)
        bool pressed1 = (gpio_get_level(BTN_GPIO) == 0);

        if (pressed1 && !was_pressed1) {
            press_start1 = esp_timer_get_time();
            fired1 = false;
#if CONFIG_HOMEHUB_VOICE
            button_press_cb press_cb = s_press_cb;
            claimed = press_cb && press_cb(true);
            if (claimed) {
                fired1 = true;
                click_count1 = 0;
            }
        } else if (!pressed1 && was_pressed1 && claimed) {
            claimed = false;
            button_press_cb press_cb = s_press_cb;
            if (press_cb) press_cb(false);
#endif
        } else if (pressed1 && !fired1) {
            int64_t held_ms = (esp_timer_get_time() - press_start1) / 1000;
            if (held_ms >= LONG_PRESS_MS) {
                ESP_LOGI(TAG, "Key 1 long press detected (5s)");
                if (s_long_press_cb) s_long_press_cb();
                fired1 = true;
                click_count1 = 0;
            }
        } else if (!pressed1 && was_pressed1 && !fired1) {
            int64_t held_ms = (esp_timer_get_time() - press_start1) / 1000;
            if (held_ms >= 50 && held_ms <= SHORT_PRESS_MAX_MS) {
                click_count1++;
                last_release1 = esp_timer_get_time();
            }
        }

        if (!pressed1 && click_count1 > 0) {
            int64_t since_release = (esp_timer_get_time() - last_release1) / 1000;
            if (since_release >= DOUBLE_CLICK_MS) {
                if (click_count1 >= 2) {
                    ESP_LOGI(TAG, "Key 1 double press detected");
                    if (s_double_press_cb) s_double_press_cb();
                } else {
                    ESP_LOGI(TAG, "Key 1 short press detected");
                    if (s_short_press_cb) s_short_press_cb();
                }
                click_count1 = 0;
            }
        }
        was_pressed1 = pressed1;

        // Key 2 (GPIO 1)
        bool pressed2 = (gpio_get_level(KEY2_GPIO) == 0);
        if (pressed2 && !was_pressed2) {
            ESP_LOGI(TAG, "Key 2 press detected");
            if (s_key2_cb) s_key2_cb();
        }
        was_pressed2 = pressed2;

        // Key 3 (GPIO 3)
        bool pressed3 = (gpio_get_level(KEY3_GPIO) == 0);
        if (pressed3 && !was_pressed3) {
            ESP_LOGI(TAG, "Key 3 press detected");
            if (s_key3_cb) s_key3_cb();
        }
        was_pressed3 = pressed3;

        stack_monitor_poll(&stack);
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

bool button_init(button_cb on_short_press, button_cb on_double_press,
                 button_cb on_long_press) {
    s_short_press_cb = on_short_press;
    s_double_press_cb = on_double_press;
    s_long_press_cb = on_long_press;

    uint64_t pin_mask = (1ULL << BTN_GPIO) | (1ULL << KEY2_GPIO) | (1ULL << KEY3_GPIO);
    gpio_config_t cfg = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio config failed: %s", esp_err_to_name(err));
        return false;
    }

    xTaskCreate(button_task, "btn", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "buttons ready: Key1=GPIO%d (tap/2x/5s), Key2=GPIO%d, Key3=GPIO%d",
             BTN_GPIO, KEY2_GPIO, KEY3_GPIO);
    return true;
}
