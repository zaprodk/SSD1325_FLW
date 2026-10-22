#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include "time.h"
#include "secrets.h"
#include "lcdgfx.h"
#include "generated_assets.h"
#include "soc/soc.h"             
#include "soc/rtc_cntl_reg.h"

// 16KB RAM buffer holding all 4 screen panel glyphs simultaneously
uint8_t panelBuffers[4][4096];

#define WDT_TIMEOUT_SECONDS 10

// Shared SPI Pins (CYD P1 Header)
#define OLED_RST_PIN   4   
#define OLED_DC_PIN    5   
#define OLED_CLK_PIN   6  
#define OLED_MOSI_PIN  7  

// Dedicated CS Pins (CYD P1 Header)
#define OLED_CS0_PIN   15
#define OLED_CS1_PIN   16
#define OLED_CS2_PIN   17
#define OLED_CS3_PIN   18

const char* ntpServer = "192.168.1.1";
const long  gmtOffset_sec = 3600;       
const int   daylightOffset_sec = 3600;  

DisplaySSD1325_128x64_SPI display0(-1, {-1, OLED_CS0_PIN, OLED_DC_PIN, 10000000, OLED_CLK_PIN, OLED_MOSI_PIN});
DisplaySSD1325_128x64_SPI display1(-1, {-1, OLED_CS1_PIN, OLED_DC_PIN, 10000000, OLED_CLK_PIN, OLED_MOSI_PIN});
DisplaySSD1325_128x64_SPI display2(-1, {-1, OLED_CS2_PIN, OLED_DC_PIN, 10000000, OLED_CLK_PIN, OLED_MOSI_PIN});
DisplaySSD1325_128x64_SPI display3(-1, {-1, OLED_CS3_PIN, OLED_DC_PIN, 10000000, OLED_CLK_PIN, OLED_MOSI_PIN});

DisplaySSD1325_128x64_SPI* displays[4] = { &display0, &display1, &display2, &display3 };
const uint8_t csPins[4] = { OLED_CS0_PIN, OLED_CS1_PIN, OLED_CS2_PIN, OLED_CS3_PIN };

// --- Max Brightness Hardware Drive Parameters ---
const uint8_t FIXED_PHASE1      = 0x08; // Phase 1 Discharge Clocks
const uint8_t FIXED_PRECHARGE_V = 0x1F; // Max Pre-charge Voltage (0x1F = 1.00 x VCC)
const uint8_t FIXED_CONTRAST    = 255;  // Max Daytime Contrast (255 / 0xFF)
const uint8_t FIXED_PHASE2      = 15;   // Max Phase 2 Pre-charge Clocks (15 / 0x0F)
const uint8_t FIXED_VCOMH       = 31;   // Max VCOMH Deselect (31 / 0x1F)
const uint8_t FIXED_PRECHARGE2  = 15;   // Max 2nd Pre-charge Period (15 / 0x0F)
const uint8_t FIXED_OSC_FREQ    = 0x0F; // Max Oscillator Frequency
const uint8_t FIXED_CLK_DIV     = 0x00; // Divide by 1 for max refresh rate
const uint8_t FIXED_GAMMA       = 0;    // Linear Gamma Preset

// --- Configurable Brightness State ---
volatile uint8_t cfg_contrast_normal = 255; // Full Brightness Default
volatile uint8_t cfg_contrast_night  = 0;   // Night Brightness

// --- Configurable State ---
volatile uint8_t cfg_font_clock = 0;    
volatile uint8_t cfg_font_flw   = 0;    
volatile bool    cfg_invert     = false;
volatile uint8_t cfg_nightmode  = 1;   
volatile bool    settingsChanged = false;

Preferences prefs;

// --- Hourly Font Rotation Configuration ---
volatile bool     cfg_rotate_fonts    = true;   
volatile uint32_t cfg_time_fonts_mask = 0xFFFFFFFF; 
volatile uint32_t cfg_flw_fonts_mask  = 0xFFFFFFFF; 

uint8_t getRandomFontFromMask(uint32_t mask, uint8_t currentFont) {
    uint8_t validIndices[FONT_COUNT];
    uint8_t count = 0;
    
    for (uint8_t i = 0; i < FONT_COUNT; i++) {
        if (mask & (1UL << i)) {
            validIndices[count++] = i;
        }
    }
    
    if (count == 0) return 0;
    if (count == 1) return validIndices[0];

    uint8_t chosen;
    do {
        chosen = validIndices[esp_random() % count];
    } while (count > 1 && chosen == currentFont);

    return chosen;
}

const uint8_t GAMMA_PRESETS[3][15] = {
    { 2,  4,  6,  8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30 }, 
    { 0,  1,  2,  3,  5,  8, 12, 16, 20, 24, 27, 29, 30, 31, 31 }, 
    { 5,  8, 11, 14, 16, 18, 20, 22, 24, 25, 26, 27, 28, 29, 30 }  
};

WebServer server(80);

inline uint8_t getCharIndex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
    return 0;
}

const uint8_t* getAssetForChar(char c, uint8_t fontIdx) {
    if (fontIdx >= FONT_COUNT) fontIdx = 0;
    uint8_t charIdx = getCharIndex(c);
    return (const uint8_t*) pgm_read_ptr(&(FONT_GLYPHS[fontIdx][charIdx]));
}

// Pre-fetches all 4 characters into RAM staging buffer
void loadWordToBuffers(const char* word4, uint8_t fontIdx) {
    for (int i = 0; i < 4; i++) {
        const uint8_t* ptr = getAssetForChar(word4[i], fontIdx);
        memcpy_P(panelBuffers[i], ptr, 4096);
    }
}

// Flushes RAM buffers to the OLED panels over SPI
void renderBatchToDisplays() {
    for (int i = 0; i < 4; i++) {
        displays[i]->drawBitmap4(0, 0, 128, 64, panelBuffers[i]);
    }
}

// ==============================================================================
// HORIZONTAL TEXT HELPER FOR 90° CLOCKWISE ROTATED DISPLAY (64 WIDE x 128 TALL)
// ==============================================================================
void drawStrHorizontalOnRotatedDisplay(DisplaySSD1325_128x64_SPI* d, int xv, int yv, const char* str, uint8_t color = 15) {
    d->setColor(color);
    while (*str) {
        char c = *str;
        if (c >= 32 && c <= 127) {
            const uint8_t* glyph = &ssd1306xled_font6x8[4 + (c - 32) * 6];
            for (int col = 0; col < 6; col++) {
                uint8_t line = pgm_read_byte(&glyph[col]);
                for (int row = 0; row < 8; row++) {
                    if (line & (1 << row)) {
                        int px = xv + col;
                        int py = yv + row;
                        if (px >= 0 && px < 64 && py >= 0 && py < 128) {
                            d->putPixel(py, 63 - px);
                        }
                    }
                }
            }
        }
        xv += 7; // 6px font width + 1px spacing
        str++;
    }
}

enum DisplayMode { MODE_CLOCK, MODE_FLW };
volatile DisplayMode currentMode = MODE_CLOCK;
volatile uint32_t modeStartTime = 0;
char currentWord[5] = "TIME";
volatile bool forceRedraw = true;

void selectRandomWord() {
    uint16_t count = FLW_WORD_COUNT;
    static uint16_t lastIdx = 0xFFFF;
    if (count > 1) {
        uint16_t idx;
        do { idx = esp_random() % count; } while (idx == lastIdx);
        lastIdx = idx;
        memcpy_P(currentWord, FLW_WORDS[idx], 5);
        currentWord[4] = '\0';
    } else if (count == 1) {
        memcpy_P(currentWord, FLW_WORDS[0], 5);
        currentWord[4] = '\0';
    }
}

bool isNightModeActive() {
    if (cfg_nightmode == 2) return true;
    if (cfg_nightmode == 1) {
        time_t nowSecs; time(&nowSecs);
        struct tm timeinfo; localtime_r(&nowSecs, &timeinfo);
        if (timeinfo.tm_year > 70) {
            if (timeinfo.tm_hour >= 23 || timeinfo.tm_hour < 7) {
                return true;
            }
        }
    }
    return false;
}

void configurePanel(DisplaySSD1325_128x64_SPI* d) {
    bool inNightMode = isNightModeActive();

    uint8_t activeContrast = inNightMode ? cfg_contrast_night : cfg_contrast_normal;
    uint8_t activePhase2 = map(activeContrast, 0, 255, 4, FIXED_PHASE2);

    uint8_t clock_byte = ((FIXED_OSC_FREQ & 0x0F) << 4) | (FIXED_CLK_DIV & 0x0F);
    uint8_t phase_byte = ((activePhase2 & 0x0F) << 4) | (FIXED_PHASE1 & 0x0F);
    uint8_t precharge2_byte = 0x80 | (FIXED_PRECHARGE2 & 0x0F);

    d->getInterface().commandStart();
    d->getInterface().send(0xB3); d->getInterface().send(clock_byte);
    d->getInterface().send(0xB1); d->getInterface().send(phase_byte);
    d->getInterface().send(0xBB); d->getInterface().send(FIXED_PRECHARGE_V);
    d->getInterface().send(0xBE); d->getInterface().send(FIXED_VCOMH & 0x1F);

    d->getInterface().send(0xB8);
    for (int i = 0; i < 15; i++) {
        d->getInterface().send(GAMMA_PRESETS[FIXED_GAMMA][i]);
    }

    d->getInterface().send(0xA0); d->getInterface().send(0x50);
    d->getInterface().send(0xA1); d->getInterface().send(0x00);
    d->getInterface().send(0xA8); d->getInterface().send(0x3F);
    d->getInterface().send(0xA2); d->getInterface().send(0x4C);
    d->getInterface().send(0xAD); d->getInterface().send(0x02);

    d->getInterface().send(cfg_invert ? 0xA7 : 0xA4);
    d->getInterface().send(0x81); d->getInterface().send(activeContrast);
    d->getInterface().send(0x82); d->getInterface().send(precharge2_byte);

    d->getInterface().send(0xAF);
    d->getInterface().stop();
}

void applySettingsToAllPanels() {
    for (int i = 0; i < 4; i++) {
        configurePanel(displays[i]);
    }
}

void drawDotCursor(DisplaySSD1325_128x64_SPI* d, int cx = 64, int cy = 58) {
    static const uint8_t dot8x8[8][8] = {
        { 0x00, 0x03, 0x05, 0x05, 0x05, 0x05, 0x03, 0x00 },
        { 0x03, 0x05, 0x08, 0x0B, 0x0B, 0x08, 0x05, 0x03 },
        { 0x05, 0x08, 0x0E, 0x0F, 0x0F, 0x0E, 0x08, 0x05 },
        { 0x05, 0x0B, 0x0F, 0x0F, 0x0F, 0x0F, 0x0B, 0x05 },
        { 0x05, 0x0B, 0x0F, 0x0F, 0x0F, 0x0F, 0x0B, 0x05 },
        { 0x05, 0x08, 0x0E, 0x0F, 0x0F, 0x0E, 0x08, 0x05 },
        { 0x03, 0x05, 0x08, 0x0B, 0x0B, 0x08, 0x05, 0x03 },
        { 0x00, 0x03, 0x05, 0x05, 0x05, 0x05, 0x03, 0x00 }
    };

    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            uint8_t val = dot8x8[row][col];
            if (val > 0) {
                d->setColor(val);
                d->putPixel(cx - 3 + col, cy - 3 + row);
            }
        }
    }
}

void loadSavedPreferences() {
    prefs.begin("flwclock", true);
    cfg_font_clock      = prefs.getUChar("font_clk", 0);
    cfg_font_flw        = prefs.getUChar("font_flw", 0);
    cfg_invert          = prefs.getBool("invert", false);
    cfg_nightmode       = prefs.getUChar("nightmode", 1);
    cfg_rotate_fonts    = prefs.getBool("rotate_f", true);
    cfg_time_fonts_mask = prefs.getUInt("time_mask", 0xFFFFFFFF);
    cfg_flw_fonts_mask  = prefs.getUInt("flw_mask", 0xFFFFFFFF);
    cfg_contrast_normal = prefs.getUChar("cntr_norm", 255);
    cfg_contrast_night  = prefs.getUChar("cntr_night", 0);
    prefs.end();

    if (cfg_font_clock >= FONT_COUNT) cfg_font_clock = 0;
    if (cfg_font_flw >= FONT_COUNT)   cfg_font_flw = 0;
}

void savePreferences() {
    prefs.begin("flwclock", false);
    prefs.putUChar("font_clk", cfg_font_clock);
    prefs.putUChar("font_flw", cfg_font_flw);
    prefs.putBool("invert", cfg_invert);
    prefs.putUChar("nightmode", cfg_nightmode);
    prefs.putBool("rotate_f", cfg_rotate_fonts);
    prefs.putUInt("time_mask", cfg_time_fonts_mask);
    prefs.putUInt("flw_mask", cfg_flw_fonts_mask);
    prefs.putUChar("cntr_norm", cfg_contrast_normal);
    prefs.putUChar("cntr_night", cfg_contrast_night);
    prefs.end();
}

// ==============================================================================
// CORE 1: DEDICATED GRAPHICS ENGINE
// ==============================================================================
void displayTask(void* parameter) {
    static int lastMinute = -1;
    static bool lastCursorState = false;
    static bool lastNightState = false;

    for (;;) {
        uint32_t nowMs = millis();

        bool currentNightState = isNightModeActive();
        if (currentNightState != lastNightState) {
            lastNightState = currentNightState;
            settingsChanged = true;
        }

        if (settingsChanged) {
            settingsChanged = false;
            applySettingsToAllPanels();
            forceRedraw = true;
        }

        // State Machine Transitions
        if (currentMode == MODE_CLOCK && (nowMs - modeStartTime >= 20000)) { 
            currentMode = MODE_FLW;
            modeStartTime = nowMs;
            selectRandomWord();
            loadWordToBuffers(currentWord, cfg_font_flw);
            renderBatchToDisplays();
        } 
        else if (currentMode == MODE_FLW && (nowMs - modeStartTime >= 5000)) { 
            currentMode = MODE_CLOCK;
            modeStartTime = nowMs;
            forceRedraw = true;
        }

        // Render Clock Mode
        if (currentMode == MODE_CLOCK) {
            time_t nowSecs; time(&nowSecs);
            struct tm timeinfo; localtime_r(&nowSecs, &timeinfo);

            static int lastHourRotated = -1;
            if (cfg_rotate_fonts && timeinfo.tm_hour != lastHourRotated && timeinfo.tm_min == 0) {
                lastHourRotated = timeinfo.tm_hour;
                cfg_font_clock = getRandomFontFromMask(cfg_time_fonts_mask, cfg_font_clock);
                cfg_font_flw   = getRandomFontFromMask(cfg_flw_fonts_mask, cfg_font_flw);
                savePreferences();
                forceRedraw = true;
            }

            if (timeinfo.tm_hour == 1 && timeinfo.tm_min == 23 && timeinfo.tm_sec == 0) {
                ESP.restart();
            }

            bool currentCursorState = ((nowMs % 2000) < 1000);
            bool minuteChanged = (timeinfo.tm_min != lastMinute);
            bool cursorChanged = (currentCursorState != lastCursorState);

            if (minuteChanged || forceRedraw) {
                lastMinute = timeinfo.tm_min;
                forceRedraw = false;
                lastCursorState = currentCursorState;

                char clockStr[5];
                snprintf(clockStr, sizeof(clockStr), "%02d%02d", timeinfo.tm_hour, timeinfo.tm_min);

                loadWordToBuffers(clockStr, cfg_font_clock);
                renderBatchToDisplays();

                if (currentCursorState) {
                    drawDotCursor(&display0, 64, 58);
                }
            } 
            else if (cursorChanged) {
                lastCursorState = currentCursorState;

                display0.drawBitmap4(0, 0, 128, 64, panelBuffers[0]);

                if (currentCursorState) {
                    drawDotCursor(&display0, 64, 58);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ==============================================================================
// WEB DASHBOARD & API (CORE 0)
// ==============================================================================
const char HTTP_INDEX[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>FLW Clock Studio</title><style>
        body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: #0f111a; color: #e0e6ed; margin: 0; padding: 20px; }
        .card { background: #1a1d2d; max-width: 520px; margin: 0 auto 20px auto; padding: 20px; border-radius: 12px; box-shadow: 0 4px 20px rgba(0,0,0,0.6); }
        h2, h3 { color: #00e5ff; margin-top: 0; text-align: center; }
        .control-group { margin-bottom: 16px; }
        label { display: flex; justify-content: space-between; font-weight: 600; font-size: 13px; margin-bottom: 6px; }
        span.val { color: #ffca28; font-family: monospace; }
        input[type=range] { width: 100%; accent-color: #00e5ff; }
        select, input[type=text] { width: 100%; background: #0f111a; border: 1px solid #333b55; color: #fff; padding: 10px; border-radius: 6px; box-sizing: border-box; font-size: 14px; }
        .checkbox-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; background: #0f111a; padding: 12px; border-radius: 6px; border: 1px solid #333b55; }
        .checkbox-grid label { font-weight: normal; font-size: 13px; display: flex; align-items: center; justify-content: flex-start; gap: 8px; margin: 0; cursor: pointer; }
        .checkbox-grid input[type=checkbox] { accent-color: #00e5ff; margin: 0; cursor: pointer; }
        .btn-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 10px; }
        button { background: #00e5ff; color: #0f111a; font-weight: bold; border: none; padding: 12px; border-radius: 6px; cursor: pointer; font-size: 14px; }
        button:hover { background: #80d8ff; }
    </style>
</head>
<body>
    <div class="card">
        <h3>Font & Style Controls</h3>
        <div class="control-group">
            <label>Active Time Font:</label>
            <select id="font_clock" onchange="previewTimeFont()"></select>
        </div>
        <div class="control-group">
            <label>Active FLW Font:</label>
            <select id="font_flw" onchange="previewFlwFont()"></select>
        </div>
        <div class="control-group">
            <label>Hourly Font Rotation:</label>
            <select id="rotate_fonts" onchange="updateParam()">
                <option value="0">Disabled (Keep Current Fonts)</option>
                <option value="1">Enabled (Randomize Top of Every Hour)</option>
            </select>
        </div>
        <div class="control-group">
            <label>Allowed Time Fonts Pool:</label>
            <div id="time_pool" class="checkbox-grid"></div>
        </div>
        <div class="control-group">
            <label>Allowed FLW Fonts Pool:</label>
            <div id="flw_pool" class="checkbox-grid"></div>
        </div>
        <div class="control-group">
            <label>Display Invert (0xA4/0xA7):</label>
            <select id="invert" onchange="updateParam()">
                <option value="0">Normal Mode</option>
                <option value="1">Inverted Mode</option>
            </select>
        </div>
    </div>

    <div class="card">
        <h3>Operational & Night Mode</h3>
        <div class="control-group">
            <label>Normal Mode Brightness: <span id="lbl_norm" class="val">255</span></label>
            <input type="range" id="contrast_normal" min="0" max="255" oninput="updateParam()">
        </div>
        <div class="control-group">
            <label>Night Mode Brightness: <span id="lbl_night" class="val">0</span></label>
            <input type="range" id="contrast_night" min="0" max="255" oninput="updateParam()">
        </div>
        <div class="control-group">
            <label>Night Mode Dimming Schedule:</label>
            <select id="nightmode" onchange="updateParam()">
                <option value="0">Disabled (Full Brightness Always)</option>
                <option value="1">Auto Schedule (Night brightness during 23:00 - 07:00)</option>
                <option value="2">Force Night Mode Now</option>
            </select>
        </div>
    </div>

    <div class="card">
        <h3>Live Controls</h3>
        <div class="control-group">
            <label>Broadcast Custom Word:</label>
            <input type="text" id="custom_word" maxlength="4" placeholder="E.G. STAR" style="text-transform: uppercase;">
        </div>
        <div class="btn-grid">
            <button onclick="sendCustomWord()">Send Word</button>
            <button onclick="triggerNextWord()">Next FLW Word</button>
        </div>
    </div>

    <script>
        function loadValues() {
            fetch('/api/get')
                .then(r => r.json())
                .then(d => {
                    let fc = document.getElementById('font_clock');
                    let ff = document.getElementById('font_flw');
                    let tp = document.getElementById('time_pool');
                    let fp = document.getElementById('flw_pool');

                    fc.innerHTML = ''; ff.innerHTML = '';
                    tp.innerHTML = ''; fp.innerHTML = '';

                    d.font_names.forEach((name, idx) => {
                        fc.add(new Option(name, idx));
                        ff.add(new Option(name, idx));

                        let tChecked = (d.time_mask & (1 << idx)) ? 'checked' : '';
                        let fChecked = (d.flw_mask & (1 << idx)) ? 'checked' : '';

                        tp.innerHTML += `<label><input type="checkbox" class="t-mask" value="${idx}" ${tChecked} onchange="previewTimeFont(${idx})">${name}</label>`;
                        fp.innerHTML += `<label><input type="checkbox" class="f-mask" value="${idx}" ${fChecked} onchange="previewFlwFont(${idx})">${name}</label>`;
                    });

                    fc.value = d.font_clock;
                    ff.value = d.font_flw;
                    document.getElementById('rotate_fonts').value = d.rotate_fonts ? 1 : 0;
                    document.getElementById('invert').value = d.invert ? 1 : 0;
                    document.getElementById('nightmode').value = d.nightmode;

                    document.getElementById('contrast_normal').value = d.contrast_normal;
                    document.getElementById('contrast_night').value  = d.contrast_night;
                    refreshLabels();
                });
        }

        function refreshLabels() {
            document.getElementById('lbl_norm').innerText  = document.getElementById('contrast_normal').value;
            document.getElementById('lbl_night').innerText = document.getElementById('contrast_night').value;
        }

        let debounceTimer;
        function updateParam(triggerFlwPreview = false, triggerTimePreview = false) {
            refreshLabels();
            clearTimeout(debounceTimer);
            debounceTimer = setTimeout(() => {
                let timeMask = 0;
                document.querySelectorAll('.t-mask:checked').forEach(cb => timeMask |= (1 << parseInt(cb.value)));

                let flwMask = 0;
                document.querySelectorAll('.f-mask:checked').forEach(cb => flwMask |= (1 << parseInt(cb.value)));

                let p = new URLSearchParams();
                p.append('font_clock', document.getElementById('font_clock').value);
                p.append('font_flw', document.getElementById('font_flw').value);
                p.append('rotate_fonts', document.getElementById('rotate_fonts').value);
                p.append('time_mask', timeMask);
                p.append('flw_mask', flwMask);
                p.append('invert', document.getElementById('invert').value);
                p.append('nightmode', document.getElementById('nightmode').value);
                p.append('contrast_normal', document.getElementById('contrast_normal').value);
                p.append('contrast_night', document.getElementById('contrast_night').value);

                fetch('/api/set?' + p.toString()).then(() => {
                    if (triggerFlwPreview) {
                        fetch('/api/trigger');
                    } else if (triggerTimePreview) {
                        fetch('/api/trigger_clock');
                    }
                });
            }, 50);
        }

        function previewTimeFont(idx) {
            if (idx !== undefined) {
                document.getElementById('font_clock').value = idx;
            }
            updateParam(false, true);
        }

        function previewFlwFont(idx) {
            if (idx !== undefined) {
                document.getElementById('font_flw').value = idx;
            }
            updateParam(true, false);
        }

        function sendCustomWord() {
            let w = document.getElementById('custom_word').value.toUpperCase();
            if(w.length === 4) fetch('/api/custom?w=' + w);
        }

        function triggerNextWord() { fetch('/api/trigger'); }

        window.onload = loadValues;
    </script>
</body>
</html>
)rawliteral";

void handleRoot() { server.send(200, "text/html", HTTP_INDEX); }

void handleGetApi() {
    String json = "{\"font_clock\":" + String(cfg_font_clock) +
                  ",\"font_flw\":" + String(cfg_font_flw) +
                  ",\"rotate_fonts\":" + String(cfg_rotate_fonts ? 1 : 0) +
                  ",\"time_mask\":" + String(cfg_time_fonts_mask) +
                  ",\"flw_mask\":" + String(cfg_flw_fonts_mask) +
                  ",\"invert\":" + String(cfg_invert ? 1 : 0) +
                  ",\"nightmode\":" + String(cfg_nightmode) +
                  ",\"contrast_normal\":" + String(cfg_contrast_normal) +
                  ",\"contrast_night\":" + String(cfg_contrast_night) +
                  ",\"font_names\":[";
    for (uint8_t i = 0; i < FONT_COUNT; i++) {
        json += "\"" + String(FONT_NAMES[i]) + "\"";
        if (i < FONT_COUNT - 1) json += ",";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

void handleSetApi() {
    if (server.hasArg("font_clock"))      cfg_font_clock      = server.arg("font_clock").toInt();
    if (server.hasArg("font_flw"))        cfg_font_flw        = server.arg("font_flw").toInt();
    if (server.hasArg("rotate_fonts"))    cfg_rotate_fonts    = (server.arg("rotate_fonts").toInt() == 1);
    if (server.hasArg("time_mask"))       cfg_time_fonts_mask = server.arg("time_mask").toInt();
    if (server.hasArg("flw_mask"))        cfg_flw_fonts_mask  = server.arg("flw_mask").toInt();
    if (server.hasArg("invert"))          cfg_invert          = (server.arg("invert").toInt() == 1);
    if (server.hasArg("nightmode"))       cfg_nightmode       = server.arg("nightmode").toInt();
    if (server.hasArg("contrast_normal")) cfg_contrast_normal = server.arg("contrast_normal").toInt();
    if (server.hasArg("contrast_night"))  cfg_contrast_night  = server.arg("contrast_night").toInt();

    savePreferences();
    settingsChanged = true;
    server.send(200, "text/plain", "OK");
}

void handleTriggerApi() {
    currentMode = MODE_FLW;
    modeStartTime = millis();
    selectRandomWord();
    loadWordToBuffers(currentWord, cfg_font_flw);
    renderBatchToDisplays();
    server.send(200, "text/plain", "OK");
}

void handleTriggerClockApi() {
    currentMode = MODE_CLOCK;
    modeStartTime = millis();
    forceRedraw = true;
    server.send(200, "text/plain", "OK");
}

void handleCustomWordApi() {
    if (server.hasArg("w")) {
        String w = server.arg("w");
        w.toUpperCase();
        if (w.length() == 4) {
            currentMode = MODE_FLW;
            modeStartTime = millis();
            snprintf(currentWord, sizeof(currentWord), "%s", w.c_str());
            loadWordToBuffers(currentWord, cfg_font_flw);
            renderBatchToDisplays();
        }
    }
    server.send(200, "text/plain", "OK");
}

void initWatchdog() {
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = WDT_TIMEOUT_SECONDS * 1000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true
    };
    esp_task_wdt_reconfigure(&twdt_config);
    esp_task_wdt_add(NULL);
#else
    esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);
    esp_task_wdt_add(NULL);
#endif
}

void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    Serial.begin(115200);

    initWatchdog();

    for (int i = 0; i < 4; i++) {
        pinMode(csPins[i], OUTPUT);
        digitalWrite(csPins[i], HIGH);
    }

    pinMode(OLED_RST_PIN, OUTPUT);
    digitalWrite(OLED_RST_PIN, LOW);
    delay(20);
    digitalWrite(OLED_RST_PIN, HIGH);
    delay(20);

    loadSavedPreferences();

    for (int i = 0; i < 4; i++) {
        displays[i]->begin();
        configurePanel(displays[i]);
        displays[i]->clear();
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    Serial.print("\nConnecting to WiFi...");
    while (WiFi.status() != WL_CONNECTED) {
        delay(250);
        Serial.print(".");
    }
    Serial.println("\nWiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    if (MDNS.begin("flwclock")) {
        Serial.println("mDNS Active: http://flwclock.local");
    }

    // =========================================================================
    // BOOT DISPLAY: DISPLAY 0 & DISPLAY 1 (HORIZONTALLY ALIGNED ON PORTRAIT PANELS)
    // =========================================================================
    String ip = WiFi.localIP().toString();
    int dot1 = ip.indexOf('.');
    int dot2 = ip.indexOf('.', dot1 + 1);
    String ipPart1 = ip.substring(0, dot2 + 1); // e.g. "192.168."
    String ipPart2 = ip.substring(dot2 + 1);      // e.g. "1.100"

    for (int i = 0; i < 4; i++) {
        displays[i]->clear();
    }

    // Display 0: Wi-Fi Status & IP Address
    drawStrHorizontalOnRotatedDisplay(&display0, 7, 20, "WIFI OK");
    drawStrHorizontalOnRotatedDisplay(&display0, 7, 42, "IP:");
    drawStrHorizontalOnRotatedDisplay(&display0, 4, 58, ipPart1.c_str());
    drawStrHorizontalOnRotatedDisplay(&display0, 4, 72, ipPart2.c_str());

    // Display 1: mDNS Domain Name
    drawStrHorizontalOnRotatedDisplay(&display1, 7, 20, "MDNS OK");
    drawStrHorizontalOnRotatedDisplay(&display1, 7, 42, "HOST:");
    drawStrHorizontalOnRotatedDisplay(&display1, 4, 58, "flwclock");
    drawStrHorizontalOnRotatedDisplay(&display1, 4, 72, ".local");

    delay(4000); // Hold boot text on Panels 0 & 1 for 4 seconds
    // =========================================================================

    server.on("/", handleRoot);
    server.on("/api/get", handleGetApi);
    server.on("/api/set", handleSetApi);
    server.on("/api/trigger", handleTriggerApi);
    server.on("/api/trigger_clock", handleTriggerClockApi);
    server.on("/api/custom", handleCustomWordApi);
    server.on("/favicon.ico", []() { server.send(204); });
    server.onNotFound([]() { server.send(404, "text/plain", "404: Not Found"); });
    server.begin();

    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    modeStartTime = millis();

    xTaskCreatePinnedToCore(
        displayTask,
        "DisplayTask",
        8192,
        NULL,
        2,
        NULL,
        1
    );
}

void loop() {
    esp_task_wdt_reset();
    server.handleClient();
    delay(2);
}