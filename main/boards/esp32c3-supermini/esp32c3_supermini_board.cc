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
#include <ctime>

#define TAG "Esp32C3SuperminiBoard"

class SuperminiAudioCodec : public NoAudioCodecDuplex {
public:
    SuperminiAudioCodec(int input_sample_rate, int output_sample_rate,
                        gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din)
        : NoAudioCodecDuplex(input_sample_rate, output_sample_rate, bclk, ws, dout, din) {
        input_gain_ = 4.0f; // 4x digital gain (+12dB boost)
    }

    virtual int Read(int16_t* dest, int samples) override {
        int read_samples = NoAudioCodecDuplex::Read(dest, samples);
        if (read_samples > 0) {
            for (int i = 0; i < read_samples; i++) {
                int32_t val = static_cast<int32_t>(dest[i]) * 4;
                if (val > INT16_MAX) {
                    val = INT16_MAX;
                } else if (val < -INT16_MAX) {
                    val = -INT16_MAX;
                }
                dest[i] = static_cast<int16_t>(val);
            }
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

    enum ScreenMode {
        kScreenDark = 0,
        kScreenLight = 1,
        kScreenSleep = 2
    };
    ScreenMode screen_mode_ = kScreenDark;

    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    PressToTalkMcpTool* press_to_talk_tool_ = nullptr;

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

    void CycleScreenMode() {
        if (!panel_) return;
        if (screen_mode_ == kScreenDark) {
            screen_mode_ = kScreenLight;
            esp_lcd_panel_disp_on_off(panel_, true);
            esp_lcd_panel_invert_color(panel_, true);
            GetDisplay()->ShowNotification("Light Mode", 2000);
        } else if (screen_mode_ == kScreenLight) {
            screen_mode_ = kScreenSleep;
            esp_lcd_panel_disp_on_off(panel_, false);
        } else {
            screen_mode_ = kScreenDark;
            esp_lcd_panel_disp_on_off(panel_, true);
            esp_lcd_panel_invert_color(panel_, false);
            GetDisplay()->ShowNotification("Dark Mode", 2000);
        }
    }

    void EnsureScreenAwake() {
        if (screen_mode_ == kScreenSleep && panel_) {
            screen_mode_ = kScreenDark;
            esp_lcd_panel_disp_on_off(panel_, true);
            esp_lcd_panel_invert_color(panel_, false);
        }
    }

    void ShowDeskSystemInfo() {
        auto& wifi = WifiManager::GetInstance();
        time_t now = time(nullptr);
        struct tm tm_info;
        localtime_r(&now, &tm_info);

        char time_str[16];
        if (tm_info.tm_year > 120) {
            snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d",
                     tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        } else {
            snprintf(time_str, sizeof(time_str), "Ready");
        }

        char info_str[48];
        snprintf(info_str, sizeof(info_str), "IP: %s\nRAM: %luKB",
                 wifi.GetIpAddress().empty() ? "Offline" : wifi.GetIpAddress().c_str(),
                 (unsigned long)(esp_get_free_heap_size() / 1024));

        GetDisplay()->SetStatus(time_str);
        GetDisplay()->SetChatMessage("system", info_str);
    }

    void InitializeButtons() {
        // Key 1 (GPIO 0): PTT / Toggle Chat / Wi-Fi Config
        boot_button_.OnClick([this]() {
            EnsureScreenAwake();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            if (!press_to_talk_tool_ || !press_to_talk_tool_->IsPressToTalkEnabled()) {
                app.ToggleChatState();
            }
        });
        boot_button_.OnPressDown([this]() {
            EnsureScreenAwake();
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StartListening();
            }
        });
        boot_button_.OnPressUp([this]() {
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StopListening();
            }
        });

        // Key 2 (GPIO 1): Volume Up / Screen Mode (Light - Dark - Sleep)
        volume_up_button_.OnClick([this]() {
            EnsureScreenAwake();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });
        volume_up_button_.OnDoubleClick([this]() {
            CycleScreenMode();
        });
        volume_up_button_.OnLongPress([this]() {
            EnsureScreenAwake();
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        // Key 3 (GPIO 3): Volume Down / Mute / Desk Info
        volume_down_button_.OnClick([this]() {
            EnsureScreenAwake();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });
        volume_down_button_.OnDoubleClick([this]() {
            EnsureScreenAwake();
            ShowDeskSystemInfo();
        });
        volume_down_button_.OnLongPress([this]() {
            EnsureScreenAwake();
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    void InitializeTools() {
        press_to_talk_tool_ = new PressToTalkMcpTool();
        press_to_talk_tool_->Initialize();

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
