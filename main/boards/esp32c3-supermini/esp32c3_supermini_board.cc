#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "press_to_talk_mcp_tool.h"
#include "assets/lang_config.h"
#include "wifi_manager.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_system.h>
#include <cmath>
#include <vector>
#include <cstdlib>

#define TAG "Esp32C3SuperminiBoard"

class SuperminiMicTesterDisplay : public OledDisplay {
private:
    lv_obj_t* test_container_ = nullptr;
    lv_obj_t* label_title_ = nullptr;
    lv_obj_t* label_val_ = nullptr;
    lv_obj_t* label_status_ = nullptr;
    lv_obj_t* bars_[16];

public:
    SuperminiMicTesterDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                              int width, int height, bool mirror_x, bool mirror_y)
        : OledDisplay(panel_io, panel, width, height, mirror_x, mirror_y) {}

    virtual void SetupUI() override {
        OledDisplay::SetupUI();

        DisplayLockGuard lock(this);
        auto screen = lv_screen_active();

        test_container_ = lv_obj_create(screen);
        lv_obj_set_pos(test_container_, 0, 0);
        lv_obj_set_size(test_container_, 128, 64);
        lv_obj_set_style_pad_all(test_container_, 0, 0);
        lv_obj_set_style_border_width(test_container_, 0, 0);
        lv_obj_set_style_radius(test_container_, 0, 0);
        lv_obj_set_style_bg_opa(test_container_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(test_container_, lv_color_black(), 0);
        lv_obj_remove_flag(test_container_, LV_OBJ_FLAG_SCROLLABLE);

        // Header Title
        label_title_ = lv_label_create(test_container_);
        lv_obj_set_pos(label_title_, 2, 0);
        lv_label_set_text(label_title_, "MIC OSCILLOSCOPE");

        // Peak / RMS info
        label_val_ = lv_label_create(test_container_);
        lv_obj_set_pos(label_val_, 2, 14);
        lv_label_set_text(label_val_, "Peak: 0 | RMS: 0");

        // Status string
        label_status_ = lv_label_create(test_container_);
        lv_obj_set_pos(label_status_, 2, 28);
        lv_label_set_text(label_status_, "WAITING SIGNAL...");

        // 16 Vertical Wave Bars (X: 0 to 127, Y: 42 to 62)
        for (int i = 0; i < 16; i++) {
            bars_[i] = lv_bar_create(test_container_);
            lv_obj_set_pos(bars_[i], i * 8, 42);
            lv_obj_set_size(bars_[i], 6, 20);
            lv_bar_set_range(bars_[i], 0, 100);
            lv_bar_set_value(bars_[i], 0, LV_ANIM_OFF);
            lv_obj_set_style_radius(bars_[i], 0, 0);
            lv_obj_set_style_radius(bars_[i], 0, LV_PART_INDICATOR);
            lv_obj_set_style_border_width(bars_[i], 0, 0);
            lv_obj_set_style_bg_color(bars_[i], lv_color_black(), 0);
            lv_obj_set_style_bg_color(bars_[i], lv_color_white(), LV_PART_INDICATOR);
        }
    }

    void UpdateWaveform(const int16_t* samples, int count, int32_t peak, int32_t rms) {
        DisplayLockGuard lock(this);
        if (!test_container_) return;

        // Update 16 bars with amplitude of 16 subdivisions
        int chunk_size = (count >= 16) ? (count / 16) : 1;
        for (int i = 0; i < 16; i++) {
            int32_t sub_peak = 0;
            for (int j = 0; j < chunk_size; j++) {
                int idx = i * chunk_size + j;
                if (idx < count) {
                    int32_t v = std::abs(samples[idx]);
                    if (v > sub_peak) sub_peak = v;
                }
            }
            int pct = (sub_peak * 100) / 32768;
            if (pct > 100) pct = 100;
            lv_bar_set_value(bars_[i], pct, LV_ANIM_OFF);
        }

        // Update Labels
        char buf[32];
        snprintf(buf, sizeof(buf), "Pk:%ld R:%ld", (long)peak, (long)rms);
        lv_label_set_text(label_val_, buf);

        if (peak <= 50) {
            lv_label_set_text(label_status_, "NO SIGNAL (Check Mic)");
        } else if (peak > 15000) {
            lv_label_set_text(label_status_, "MIC OK: LOUD VOICE");
        } else {
            lv_label_set_text(label_status_, "MIC OK: DETECTED");
        }
    }
};

class Esp32C3SuperminiBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    SuperminiMicTesterDisplay* mic_display_ = nullptr;

    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        esp_lcd_panel_io_i2c_config_t io_config = {};
        io_config.dev_addr = 0x3C;
        io_config.scl_speed_hz = 400 * 1000;
        io_config.control_phase_bytes = 1;
        io_config.dc_bit_offset = 6;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        io_config.on_color_trans_done = nullptr;
        io_config.user_ctx = nullptr;
        io_config.flags.dc_low_on_data = 0;
        io_config.flags.disable_control_phase = 0;

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
        ESP_LOGI(TAG, "SSD1306 driver installed");

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }

        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        mic_display_ = new SuperminiMicTesterDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_ = mic_display_;
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) volume = 100;
            codec->SetOutputVolume(volume);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) volume = 0;
            codec->SetOutputVolume(volume);
        });
    }

    static void MicTesterTask(void* pvParameters) {
        auto board = static_cast<Esp32C3SuperminiBoard*>(pvParameters);
        auto codec = board->GetAudioCodec();
        codec->SetOutputVolume(90);
        codec->EnableInput(true);
        codec->EnableOutput(true);

        constexpr int kSamples = 128;
        std::vector<int16_t> buffer(kSamples);
        int log_counter = 0;

        while (1) {
            int read_count = codec->Read(buffer.data(), kSamples);
            if (read_count > 0) {
                int32_t peak = 0;
                int64_t sum_sq = 0;
                for (int i = 0; i < read_count; i++) {
                    int32_t abs_val = std::abs(buffer[i]);
                    if (abs_val > peak) peak = abs_val;
                    sum_sq += int64_t(buffer[i]) * buffer[i];
                }
                int32_t rms = static_cast<int32_t>(std::sqrt(sum_sq / read_count));

                // Loopback audio to speaker
                codec->Write(buffer.data(), read_count);

                // Update OLED waveform
                if (board->mic_display_) {
                    board->mic_display_->UpdateWaveform(buffer.data(), read_count, peak, rms);
                }

                if (++log_counter >= 10) {
                    log_counter = 0;
                    ESP_LOGI("MicTester", "[INMP441 TEST] Peak: %ld | RMS: %ld", (long)peak, (long)rms);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(25));
        }
    }

public:
    Esp32C3SuperminiBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        xTaskCreate(MicTesterTask, "mic_tester", 3584, this, 2, nullptr);
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(Esp32C3SuperminiBoard);
