#include "wifi_board.h"
#include "audio_codec.h"
#include "supermini_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <esp_netif.h>
#include <wifi_manager.h>
#include <cJSON.h>
#include <vector>
#include <mutex>
#include <string>

#define TAG "Esp32C3SuperminiBoard"

class SuperminiAudioCodec : public AudioCodec {
private:
    std::mutex data_if_mutex_;

public:
    SuperminiAudioCodec(int input_sample_rate, int output_sample_rate,
                        gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
        duplex_ = true;
        input_sample_rate_ = input_sample_rate;
        output_sample_rate_ = output_sample_rate;
        input_channels_ = 1;
        output_channels_ = 1;
        input_gain_ = 2.0f; // 2x digital pre-amp boost for INMP441 MEMS mic

        i2s_chan_config_t chan_cfg = {
            .id = XIAOZHI_I2S_PORT(0),
            .role = I2S_ROLE_MASTER,
            .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
            .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
            .auto_clear_after_cb = true,
            .auto_clear_before_cb = true,
            .intr_priority = 0,
        };
        ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

        // 32-bit slot format: Provides the 64 BCLK clock frame required for INMP441 24-bit MEMS microphone
        i2s_std_config_t std_cfg = {
            .clk_cfg = {
                .sample_rate_hz = (uint32_t)output_sample_rate_,
                .clk_src = I2S_CLK_SRC_DEFAULT,
                .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            },
            .slot_cfg = {
                .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
                .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                .slot_mode = I2S_SLOT_MODE_MONO,
                .slot_mask = I2S_STD_SLOT_LEFT,
                .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
                .ws_pol = false,
                .bit_shift = true,
            },
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = bclk,
                .ws = ws,
                .dout = dout,
                .din = din,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false
                }
            }
        };

        ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));

        // Keep continuous I2S clock running so MAX98357A internal PLL never floats/buzzes
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));

        // Prime DMA buffer with digital silence (32-bit zeros)
        std::vector<int32_t> silence(AUDIO_CODEC_DMA_FRAME_NUM * 4, 0);
        size_t written = 0;
        i2s_channel_write(tx_handle_, silence.data(), silence.size() * sizeof(int32_t), &written, pdMS_TO_TICKS(100));

        ESP_LOGI(TAG, "Supermini 32-bit I2S initialized with INMP441 mic boost (BCLK=%d, WS=%d, DOUT=%d, DIN=%d)",
                 bclk, ws, dout, din);
    }

    virtual ~SuperminiAudioCodec() {
        if (rx_handle_) {
            i2s_channel_disable(rx_handle_);
            i2s_del_channel(rx_handle_);
        }
        if (tx_handle_) {
            i2s_channel_disable(tx_handle_);
            i2s_del_channel(tx_handle_);
        }
    }

    virtual void EnableInput(bool enable) override {
        std::lock_guard<std::mutex> lock(data_if_mutex_);
        input_enabled_ = enable;
        AudioCodec::EnableInput(enable);
    }

    virtual void EnableOutput(bool enable) override {
        std::lock_guard<std::mutex> lock(data_if_mutex_);
        output_enabled_ = enable;
        AudioCodec::EnableOutput(enable);
    }

    virtual int Write(const int16_t* data, int samples) override {
        std::lock_guard<std::mutex> lock(data_if_mutex_);
        if (!tx_handle_ || samples <= 0) return 0;

        std::vector<int32_t> buffer(samples);
        if (!output_enabled_) {
            std::fill(buffer.begin(), buffer.end(), 0);
        } else {
            // Smooth linear volume scaling with 6dB headroom to prevent DAC clipping/distortion
            float volScale = (float)output_volume_ / 100.0f;
            for (int i = 0; i < samples; i++) {
                buffer[i] = static_cast<int32_t>(data[i] * volScale) << 14;
            }
        }

        size_t bytes_written = 0;
        esp_err_t ret = i2s_channel_write(tx_handle_, buffer.data(), buffer.size() * sizeof(int32_t), &bytes_written, portMAX_DELAY);
        if (ret != ESP_OK) {
            return 0;
        }
        return bytes_written / sizeof(int32_t);
    }

    virtual int Read(int16_t* dest, int samples) override {
        if (!rx_handle_ || !input_enabled_ || samples <= 0) return 0;

        size_t bytes_read = 0;
        constexpr uint32_t kReadTimeoutMs = 200;
        std::vector<int32_t> bit32_buf(samples);

        esp_err_t ret = i2s_channel_read(rx_handle_, bit32_buf.data(), samples * sizeof(int32_t), &bytes_read, kReadTimeoutMs);
        if (ret != ESP_OK) {
            return 0;
        }

        int read_samples = bytes_read / sizeof(int32_t);
        float gain_factor = (input_gain_ > 0) ? input_gain_ : 1.0f;

        for (int i = 0; i < read_samples; i++) {
            // INMP441 24-bit data is in upper 24 bits.
            // Shifting >> 11 with gain_factor provides clear, loud, sensitive voice capture.
            int32_t val = (bit32_buf[i] >> 11);
            if (gain_factor != 1.0f) {
                val = static_cast<int32_t>(val * gain_factor);
            }
            dest[i] = (val > INT16_MAX) ? INT16_MAX : (val < -INT16_MAX) ? -INT16_MAX : static_cast<int16_t>(val);
        }
        return read_samples;
    }
};

class Esp32C3SuperminiBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    SuperminiOledDisplay* supermini_display_ = nullptr;

    float last_ctx_ = 0.0f;
    float last_week_ = 0.0f;
    std::string last_r5h_ = "--";
    std::string last_rwk_ = "--";

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

        supermini_display_ = new SuperminiOledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_ = supermini_display_;
    }

    void InitializeButtons() {
        // Key 1 (GPIO 0): PTT / Toggle Chat / Wi-Fi Config
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        boot_button_.OnPressDown([this]() {
            Application::GetInstance().StartListening();
        });
        boot_button_.OnPressUp([this]() {
            Application::GetInstance().StopListening();
        });

        // Key 2 (GPIO 1): Volume Up
        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });
        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        // Key 3 (GPIO 3): Volume Down / Mute
        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });
        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    void InitializeTools() {
        // Register buzzer control MCP tool
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << BUZZER_GPIO);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);
        gpio_set_level(BUZZER_GPIO, 0);

        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.buzzer.beep",
            "Emit a short hardware beep on the piezo buzzer",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                gpio_set_level(BUZZER_GPIO, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(BUZZER_GPIO, 0);
                return true;
            });

        mcp_server.AddTool("self.quota.get",
            "Get current Antigravity AI quota remaining percentages and reset ETA",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                char reply[128];
                snprintf(reply, sizeof(reply),
                         "Hạn mức 5 giờ còn %.0f%% (reset sau %s), hạn mức 7 ngày còn %.0f%% (reset sau %s)",
                         last_ctx_, last_r5h_.c_str(), last_week_, last_rwk_.c_str());
                return std::string(reply);
            });
    }

    void UpdateQuota(float ctx, float week, const char* r5h, const char* rwk) {
        last_ctx_ = ctx;
        last_week_ = week;
        last_r5h_ = (r5h && r5h[0]) ? r5h : "--";
        last_rwk_ = (rwk && rwk[0]) ? rwk : "--";
        if (supermini_display_) {
            supermini_display_->UpdateQuota(ctx, week, r5h, rwk);
        }
    }

    static void QuotaListenerTask(void* pvParameters) {
        auto board = static_cast<Esp32C3SuperminiBoard*>(pvParameters);
        int sock = -1;
        char rx_buffer[256];

        while (1) {
            auto& wifi = WifiManager::GetInstance();
            if (!wifi.IsConnected() || wifi.GetIpAddress().empty()) {
                if (sock >= 0) {
                    close(sock);
                    sock = -1;
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            if (sock < 0) {
                sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
                if (sock < 0) {
                    ESP_LOGE(TAG, "Unable to create UDP quota socket");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    continue;
                }

                struct sockaddr_in saddr = {};
                saddr.sin_family = AF_INET;
                saddr.sin_port = htons(58922);
                saddr.sin_addr.s_addr = htonl(INADDR_ANY);

                int opt = 1;
                setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

                struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

                if (bind(sock, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
                    ESP_LOGE(TAG, "Failed to bind UDP socket to port 58922");
                    close(sock);
                    sock = -1;
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    continue;
                }
                ESP_LOGI(TAG, "UDP Quota listener successfully bound to 0.0.0.0:58922 (IP: %s)",
                         wifi.GetIpAddress().c_str());
            }

            struct sockaddr_in source_addr;
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                               (struct sockaddr*)&source_addr, &socklen);
            if (len > 0) {
                rx_buffer[len] = '\0';
                ESP_LOGI(TAG, "Received UDP quota packet (%d bytes): %s", len, rx_buffer);
                cJSON* root = cJSON_Parse(rx_buffer);
                if (root) {
                    cJSON* ctx_item = cJSON_GetObjectItem(root, "ctx");
                    cJSON* week_item = cJSON_GetObjectItem(root, "week");
                    cJSON* r5h_item = cJSON_GetObjectItem(root, "r5h");
                    cJSON* rwk_item = cJSON_GetObjectItem(root, "rwk");
                    if (ctx_item && week_item) {
                        float ctx = (float)ctx_item->valuedouble;
                        float week = (float)week_item->valuedouble;
                        const char* r5h = (r5h_item && r5h_item->valuestring) ? r5h_item->valuestring : "--";
                        const char* rwk = (rwk_item && rwk_item->valuestring) ? rwk_item->valuestring : "--";
                        board->UpdateQuota(ctx, week, r5h, rwk);
                    }
                    cJSON_Delete(root);
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
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
        InitializeTools();
        xTaskCreate(QuotaListenerTask, "quota_listener", 3072, this, 1, nullptr);
    }

    virtual AudioCodec* GetAudioCodec() override {
        static SuperminiAudioCodec audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(Esp32C3SuperminiBoard);
