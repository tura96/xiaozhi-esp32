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
    lv_obj_t* wave_line_ = nullptr;
    lv_obj_t* vu_bar_ = nullptr;
    lv_point_precise_t points_[64];

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

        // Waveform Line (64 points across 128px)
        for (int i = 0; i < 64; i++) {
            points_[i].x = i * 2;
            points_[i].y = 40;
        }
        wave_line_ = lv_line_create(test_container_);
        lv_line_set_points(wave_line_, points_, 64);
        lv_obj_set_style_line_width(wave_line_, 1, 0);
        lv_obj_set_style_line_color(wave_line_, lv_color_white(), 0);

        // VU meter bar at bottom
        vu_bar_ = lv_bar_create(test_container_);
        lv_obj_set_pos(vu_bar_, 0, 58);
        lv_obj_set_size(vu_bar_, 128, 6);
        lv_bar_set_range(vu_bar_, 0, 100);
        lv_bar_set_value(vu_bar_, 0, LV_ANIM_OFF);
        lv_obj_set_style_radius(vu_bar_, 0, 0);
        lv_obj_set_style_radius(vu_bar_, 0, LV_PART_INDICATOR);
        lv_obj_set_style_border_width(vu_bar_, 0, 0);
        lv_obj_set_style_bg_color(vu_bar_, lv_color_black(), 0);
        lv_obj_set_style_bg_color(vu_bar_, lv_color_white(), LV_PART_INDICATOR);
    }

    void UpdateWaveform(const int16_t* samples, int count, int32_t peak, int32_t rms) {
        DisplayLockGuard lock(this);
        if (!test_container_) return;

        // Downsample to 64 points
        int step = (count >= 64) ? (count / 64) : 1;
        for (int i = 0; i < 64; i++) {
            int idx = i * step;
            if (idx >= count) idx = count - 1;
            int16_t s = samples[idx];
            // Center is Y = 40, range +/- 16px
            int y = 40 - (s * 16 / 32768);
            if (y < 24) y = 24;
            if (y > 56) y = 56;
            points_[i].y = y;
        }
        lv_line_set_points(wave_line_, points_, 64);

        // Update Labels
        char buf[32];
        if (peak == 0) {
            snprintf(buf, sizeof(buf), "NO SIGNAL (0)");
        } else {
            snprintf(buf, sizeof(buf), "Pk:%ld R:%ld", (long)peak, (long)rms);
        }
        lv_label_set_text(label_val_, buf);

        // Update VU Bar (0 - 100%)
        int pct = (peak * 100) / 32768;
        if (pct > 100) pct = 100;
        lv_bar_set_value(vu_bar_, pct, LV_ANIM_OFF);
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
