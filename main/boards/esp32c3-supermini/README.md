# ESP32-C3 Super Mini Board for XiaoZhi AI

This board configuration is tailored for the **ESP32-C3 Super Mini** development board with:
- **0.96" I2C OLED Display** (SSD1306, 128x64)
- **MAX98357A I2S DAC Amp** (Audio output / Speaker)
- **INMP441 MEMS I2S Microphone** (Audio input)
- **3 Mechanical Push Buttons**
- **Mini Piezo Buzzer**

## Hardware Pin Connections

| Module | Pin | ESP32-C3 Pin | Notes |
| :--- | :--- | :--- | :--- |
| **Mechanical Key 1** | Switch to GND | **GPIO 0** | Active LOW (`INPUT_PULLUP`). Short click: Toggle chat / Connect. Hold: Push-To-Talk (PTT). Boot hold: Wi-Fi config. |
| **Mechanical Key 2** | Switch to GND | **GPIO 1** | Active LOW (`INPUT_PULLUP`). Short click: Volume +10%. Long press: Max Volume (100%). |
| **Mechanical Key 3** | Switch to GND | **GPIO 3** | Active LOW (`INPUT_PULLUP`). Short click: Volume -10%. Long press: Mute (0%). |
| **0.96" OLED Display** | SDA | **GPIO 8** | I2C Data (0x3C, 400kHz) |
| **0.96" OLED Display** | SCL | **GPIO 9** | I2C Clock |
| **MAX98357A DAC Amp** | DIN | **GPIO 5** | I2S DOUT |
| **MAX98357A DAC Amp** | BCLK | **GPIO 6** | Shared I2S BCLK |
| **MAX98357A DAC Amp** | LRC | **GPIO 7** | Shared I2S WS / LRC |
| **INMP441 MEMS Mic** | SD | **GPIO 10** | I2S DIN |
| **INMP441 MEMS Mic** | SCK | **GPIO 6** | Shared I2S BCLK |
| **INMP441 MEMS Mic** | WS | **GPIO 7** | Shared I2S WS / LRC |
| **INMP441 MEMS Mic** | L/R | GND | Left channel mono |
| **Mini Piezo Buzzer** | Positive `+` | **GPIO 4** | Digital Out / MCP tool `self.buzzer.beep` |

## Build and Flash

To build using XiaoZhi build tooling:

```bash
# Set up ESP-IDF environment (v6.0.1 or later, v6.1 recommended)
. $IDF_PATH/export.sh

# Build 4MB flash variant (standard for Super Mini)
python scripts/build.py esp32c3-supermini --name esp32c3-supermini

# Flash to device
idf.py -p COMx flash monitor
```
