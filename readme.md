# FLW Clock 🕰️

An ESP32-S3 firmware and asset pipeline designed to drive four synchronized SSD1325 128x64 OLED displays in portrait orientation. The device functions as a precision network-synchronized digital clock that alternates between displaying the current time and cycling through random four-letter words (FLW).

The firmware runs on an ESP32-S3 development board. Fonts are compiled directly into C++ `PROGMEM` data arrays via an automated Python pre-build script, eliminating filesystem overhead and guaranteeing sub-millisecond panel synchronization over SPI.

---

## ✨ Key Features

* **Quad Synchronized OLED Array:** Drives 4x SSD1325 128x64 4-bit grayscale displays over a shared 10 MHz SPI bus with dedicated chip-select lines.
* **Portrait Mapping:** Panels are mounted vertically (64 pixels wide by 128 pixels tall). Glyphs are pre-rotated 90° during asset generation, and boot diagnostics map text horizontally across the rotated matrix.
* **Asynchronous Dual-Core Architecture:**
  * **Core 1 (Graphics Engine):** Runs an isolated FreeRTOS task rendering clock digits, blinking colon cursors, and word transitions from a 16 KB RAM staging buffer with zero network interruptions.
  * **Core 0 (Networking & Control):** Manages WiFi auto-reconnect, NTP time synchronization, mDNS discovery (`http://flwclock.local`), watchdog monitoring, and a non-blocking HTTP REST API.
* **Automatic Daylight Saving Time (DST):** Utilizes POSIX timezone strings for accurate, automatic transitions between Summer and Winter time based on regional rules.
* **Responsive Web Dashboard:** Built-in mobile-friendly web interface allowing real-time timezone selection, font switching, brightness adjustment, automated hourly rotation configuration, custom word broadcasting, and live display previews for both Clock and FLW modes.
* **High-Contrast Hardware Configuration:** SSD1325 internal charge pump, pre-charge timing, oscillator frequency, and VCOMH levels are configured for maximum output without panel ghosting.
* **Automated Smart Night Mode:** Dims panels automatically during scheduled night hours (default 23:00 to 07:00, active only after valid NTP synchronization).
* **Persistent Preferences:** All user settings (timezones, active fonts, font pools, rotation toggle, night mode preferences, brightness limits, inverted mode) are saved across power cycles in ESP32 NVS memory.

---

## 🛠 Hardware & Wiring

### Pin Assignment Table

| Signal | ESP32-S3 GPIO | Header Location | Description |
| :--- | :--- | :--- | :--- |
| **GND** | GND | **P1 - Pin 1** | Common ground rail for all displays |
| **OLED_RST** | IO4 | **P1 - Pin 2** | Shared hardware reset line |
| **OLED_DC** | IO5 | **P1 - Pin 3** | Shared Data / Command select line |
| **OLED_CLK** | IO6 | **P1 - Pin 4** | Shared SPI Clock (SCK / SCL) |
| **OLED_MOSI** | IO7 | **P1 - Pin 5** | Shared SPI Master Out (MOSI / SDA / DIN) |
| **OLED_CS0** | IO15 | **P1 - Pin 6** | Chip Select — Display 0 (Hours 10s / Word Char 1) |
| **OLED_CS1** | IO16 | **P1 - Pin 7** | Chip Select — Display 1 (Hours 1s / Word Char 2) |
| **OLED_CS2** | IO17 | **P1 - Pin 8** | Chip Select — Display 2 (Minutes 10s / Word Char 3) |
| **OLED_CS3** | IO18 | **P1 - Pin 9** | Chip Select — Display 3 (Minutes 1s / Word Char 4) |
| **VCC** | 3.3V | **P2 - Pin 12** | Regulated 3.3V logic and panel power rail |

---

## 🚀 Software Setup

### Prerequisites

* [VS Code](https://code.visualstudio.com/) with the [PlatformIO IDE extension](https://platformio.org/).
* Python 3.8+ (with `Pillow` installed; the generation script will attempt to auto-install it if missing).

### 1. Partition Configuration

Because we are storing pre-rendered images directly in the firmware binary (avoiding slow filesystem lookups), you need an expanded application partition. Create `partitions_16mb.csv` in your project root:

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x5000,
phy_init, data, phy,     0xE000,   0x1000,
factory,  app,  factory, 0x10000,  0x800000,