# FLW Clock Studio 🕰️

An ESP32-S3 firmware and asset pipeline designed to drive four synchronized SSD1325 128x64 OLED displays in portrait orientation. The device functions as a precision network-synchronized digital clock that alternates between displaying the current time and cycling through random four-letter words (FLW).

The firmware runs on an ESP32-S3 development board (specifically adapted from an ESP32-1732S019 / CYD board with the original LCD panel removed), taking advantage of its 16 MB flash memory and robust dual 3.3V power rails. Fonts are compiled directly into C++ `PROGMEM` data arrays via an automated Python pre-build script, eliminating filesystem overhead and guaranteeing sub-millisecond panel synchronization over SPI.

---

## ✨ Key Features

* **Quad Synchronized OLED Array:** Drives 4x SSD1325 128x64 4-bit grayscale displays over a shared 10 MHz SPI bus with dedicated chip-select lines.
* **Portrait Mapping:** Panels are mounted vertically (64 pixels wide by 128 pixels tall). Glyphs are pre-rotated 90° during asset generation, and boot diagnostics map text horizontally across the rotated matrix.
* **Asynchronous Dual-Core Architecture:**
  * **Core 1 (Graphics Engine):** Runs an isolated FreeRTOS task rendering clock digits, blinking colon cursors, and word transitions from a 16 KB RAM staging buffer with zero network interruptions.
  * **Core 0 (Networking & Control):** Manages WiFi auto-reconnect, NTP time synchronization, mDNS discovery (`http://flwclock.local`), watchdog monitoring, and a non-blocking HTTP REST API.
* **Responsive Web Dashboard:** Built-in mobile-friendly web interface allowing real-time font switching, brightness adjustment, automated hourly rotation configuration, custom word broadcasting, and live display previews for both Clock and FLW modes.
* **High-Contrast Hardware Configuration:** SSD1325 internal charge pump, pre-charge timing, oscillator frequency, and VCOMH levels are configured for maximum output without panel ghosting.
* **Automated Smart Night Mode:** Dims panels automatically during scheduled night hours (default 23:00 to 07:00, active only after valid NTP synchronization).
* **Persistent Preferences:** All user settings (active fonts, font pools, rotation toggle, night mode preferences, brightness limits, inverted mode) are saved across power cycles in ESP32 NVS memory.

---

## 🛠 Hardware & Wiring

The setup utilizes connector **P1** and power header **P2** on the ESP32-1732S019 (Cheap Yellow Display) board.

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
```
*(Note: There is no SPIFFS/LittleFS partition here. All 8MB is allocated to the `app` partition).*

### 2. PlatformIO Environment

Ensure your `platformio.ini` is configured for the ESP32-S3 target and points to your new partition file:

```ini
[platformio]
build_cache_dir = .pio/cache

[env:esp32-s3]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
upload_speed = 460800

board_upload.flash_size = 16MB
board_build.partitions = partitions_16mb.csv

lib_ldf_mode = chain
extra_scripts = pre:generate_assets.py

lib_deps =
    lexus2k/lcdgfx@^1.1.5

monitor_dtr = 0
monitor_rts = 0
```

### 3. WiFi Credentials

Create a `src/secrets.h` file containing your local network credentials:

```cpp
#ifndef SECRETS_H
#define SECRETS_H

const char* WIFI_SSID = "Your_WiFi_SSID";
const char* WIFI_PASS = "Your_WiFi_Password";

#endif
```

---

## 🎨 Asset Generation Pipeline

The `generate_assets.py` script automatically scans the `assets/` directory for TrueType/OpenType fonts (`.ttf`, `.otf`) and word lists (`flw_uk.txt` or `words.txt`).

```text
project_root/
├── assets/
│   ├── flw_uk.txt          # Plain text file with 4-letter words (1 per line)
│   ├── HelveticaBold.ttf   # TrueType or OpenType font files
│   └── RetroFuture.otf
├── include/
│   └── generated_assets.h  # Automatically generated C++ PROGMEM header
├── src/
│   ├── main.cpp
│   └── secrets.h
└── generate_assets.py
```

### How the Pipeline Works

1. **Character Filtering:** Renders 36 standard glyphs per font (`0`–`9` and `A`–`Z`).
2. **Dynamic Scaling:** Scales each character to fit a 62x126 pixel bounding box inside a 64x128 portrait canvas using Lanczos anti-aliasing.
3. **Hardware Rotation & Packing:** Rotates each glyph 90° clockwise to match the physical orientation of the OLED screens and packs the grayscale data into 4-bit nibbles (2 pixels per byte, 4096 bytes per character).
4. **Header Generation:** Outputs a single static `include/generated_assets.h` containing glyph arrays, font name lookups, and the word list stored entirely in `PROGMEM`. It skips writes if no assets changed, preventing unnecessary recompilations.

**To run the pipeline manually:**
```bash
python generate_assets.py
```

---

## 🌐 Web API Reference

The onboard web server exposes a REST API for home automation integration and dashboard control.

| Endpoint | Method | Parameters | Description |
| :--- | :--- | :--- | :--- |
| `/` | `GET` | None | Serves the single-page HTML/CSS/JS control dashboard. |
| `/api/get` | `GET` | None | Returns JSON containing current font indices, brightness states, pools, and font name lists. |
| `/api/set` | `GET` | `font_clock`, `font_flw`, `rotate_fonts`, `time_mask`, `flw_mask`, `invert`, `nightmode`, `contrast_normal`, `contrast_night` | Updates operational parameters and commits them to NVS. |
| `/api/trigger` | `GET` | None | Immediately switches display to Word mode using the selected FLW font. |
| `/api/trigger_clock` | `GET` | None | Immediately switches display to Clock mode and refreshes digits with the active time font. |
| `/api/custom` | `GET` | `w=WORD` | Broadcasts an arbitrary 4-letter uppercase word across panels 0–3. |

---

## 🔌 Boot Sequence

On power-up, the firmware initializes display hardware and attempts a WiFi connection. Once established:

1. **Display 0** renders the WiFi link status and local IP address (formatted horizontally across the 64-pixel width).
2. **Display 1** renders the mDNS hostname status (`flwclock.local`).
3. The boot status screen holds for 4 seconds, after which Core 1 takes over the display pipeline and enters standard timekeeping operation.