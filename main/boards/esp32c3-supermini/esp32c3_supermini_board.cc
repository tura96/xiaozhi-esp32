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
#include <atomic>
#include <cmath>
#include <vector>
#include <cstdlib>

#define TAG "Esp32C3SuperminiBoard"

class Esp32C3SuperminiBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;

    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;

    std::atomic<bool> play_echo_requested_{false};

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

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            ESP_LOGI(TAG, "Key1 Clicked -> Playback Echo Triggered");
            play_echo_requested_ = true;
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) volume = 100;
            codec->SetOutputVolume(volume);
            ESP_LOGI(TAG, "Volume UP: %d%%", volume);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) volume = 0;
            codec->SetOutputVolume(volume);
            ESP_LOGI(TAG, "Volume DOWN: %d%%", volume);
        });
    }

    static void MicTesterTask(void* pvParameters) {
        auto board = static_cast<Esp32C3SuperminiBoard*>(pvParameters);
        auto codec = board->GetAudioCodec();
        codec->SetOutputVolume(90);
        codec->EnableInput(true);
        codec->EnableOutput(true);

        constexpr int kChunkSamples = 128;
        // 1.5 seconds ring buffer at 16000 Hz = 24000 samples (~48 KB)
        constexpr size_t kRingCapacity = 24000;
        std::vector<int16_t> ring_buffer(kRingCapacity, 0);
        size_t write_pos = 0;
        std::vector<int16_t> chunk(kChunkSamples);
        int log_counter = 0;
        int display_update_counter = 0;

        vTaskDelay(pdMS_TO_TICKS(1000)); // Wait for display to initialize

        while (1) {
            // Check if user requested Key1 playback
            if (board->play_echo_requested_.exchange(false)) {
                ESP_LOGI("MicTester", "Starting Echo Playback (%d samples)...", (int)kRingCapacity);
                if (board->display_) {
                    board->display_->SetStatus("PLAYING ECHO...");
                    board->display_->SetChatMessage("system", "Playing to speaker...\nListen closely");
                }

                // Playback ring buffer from oldest to newest
                size_t play_read_pos = write_pos; // oldest sample is at current write_pos
                constexpr int kPlayChunk = 256;
                std::vector<int16_t> play_chunk(kPlayChunk);

                for (size_t total_played = 0; total_played < kRingCapacity; total_played += kPlayChunk) {
                    for (int i = 0; i < kPlayChunk; i++) {
                        play_chunk[i] = ring_buffer[(play_read_pos + i) % kRingCapacity];
                    }
                    play_read_pos = (play_read_pos + kPlayChunk) % kRingCapacity;

                    codec->Write(play_chunk.data(), kPlayChunk);
                    vTaskDelay(pdMS_TO_TICKS(15));
                }
                ESP_LOGI("MicTester", "Echo Playback Finished.");
                if (board->display_) {
                    board->display_->SetStatus("ECHO FINISHED");
                }
            }

            // Normal microphone recording
            int read_count = codec->Read(chunk.data(), kChunkSamples);
            if (read_count > 0) {
                int32_t peak = 0;
                int64_t sum_sq = 0;
                for (int i = 0; i < read_count; i++) {
                    int16_t sample = chunk[i];
                    ring_buffer[write_pos] = sample;
                    write_pos = (write_pos + 1) % kRingCapacity;

                    int32_t abs_val = std::abs(sample);
                    if (abs_val > peak) peak = abs_val;
                    sum_sq += int64_t(sample) * sample;
                }
                int32_t rms = static_cast<int32_t>(std::sqrt(sum_sq / read_count));

                // Update Display every ~200ms
                if (++display_update_counter >= 10) {
                    display_update_counter = 0;
                    if (board->display_) {
                        char msg_buf[64];
                        if (peak <= 50) {
                            board->display_->SetStatus("NO MIC SIGNAL");
                            snprintf(msg_buf, sizeof(msg_buf), "Pk:%ld R:%ld\nCheck Mic Soldering", (long)peak, (long)rms);
                        } else {
                            board->display_->SetStatus("MIC DETECTED!");
                            snprintf(msg_buf, sizeof(msg_buf), "Pk:%ld R:%ld\n[Key1: Replay Echo]", (long)peak, (long)rms);
                        }
                        board->display_->SetChatMessage("mic", msg_buf);
                    }
                }

                if (++log_counter >= 10) {
                    log_counter = 0;
                    ESP_LOGI("MicTester", "[INMP441 REC] Peak: %ld | RMS: %ld", (long)peak, (long)rms);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
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
        xTaskCreate(MicTesterTask, "mic_tester", 4096, this, 2, nullptr);
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
