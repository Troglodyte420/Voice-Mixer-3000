#include <Wire.h>
#include <SD.h>
#include "Display.h"
#include "CardputerKeyboard.h"
#include "ES8311Audio.h"
#include "WavRecorder.h"
#include "VoiceEffects.h"
#include "VM3000_background.h"   // ← RGB565 background bitmap
#include "VM3000_sprites.h"      // ← RGB565 button sprites (27x18)
#include "VM3000_slider.h"       // ← RGB565 slider CLC + knob
#include "VM3000_effects.h"      // ← RGB565 effect sprites (59x48)
#include "VM3000_knobs.h"        // ← RGB565 DRC/ALC knob sprites (34x28)
#include <driver/i2s.h>

// ============================================================
// Global objects
// ============================================================

static LGFX lcd;
CardputerKeyboard keyboard;
ES8311Audio audio;
WavRecorder recorder;
VoiceEffects effects;

// ============================================================
// DigiVolume - digital playback gain
// ============================================================

float digiVolume = 1.0f;
const float DIGI_VOL_MIN  = 1.0f;
const float DIGI_VOL_MAX  = 10.0f;
const float DIGI_VOL_STEP = 0.5f;

// Soft limiter — prevents speaker distortion at high volume.
//
// Below THRESH (26000): signal passes through unchanged.
// Above THRESH: smoothly compressed using x/(1+x) curve that
// asymptotically approaches CEIL (32000) but never reaches it.
//
// This works because WE control the signal BEFORE it hits the ES8311 DAC.
// (Unlike recording where the ES8311 clips internally before I2S output.)
//
// The curve is stateless — pure math per sample, nothing to configure,
// nothing that can "not work." Same result every time for the same input.
//
//  Input     → Output
//  ±26000    → ±26000  (unchanged)
//  ±32767    → ±28400  (gentle reduction)
//  ±65534    → ±29333  (2x digiVol)
//  ±262136   → ±31500  (8x digiVol)
//  ±infinity → ±32000  (hard ceiling, never reached)
//
static inline int16_t softLimit(float v) {
    const float THRESH = 26000.0f;
    const float KNEE   = 6000.0f;  // ceiling (32000) minus threshold

    float a = fabsf(v);
    if (a <= THRESH) {
        return (int16_t)v;
    }

    float excess = (a - THRESH) / THRESH;
    float compressed = THRESH + KNEE * (excess / (1.0f + excess));

    return (v > 0) ? (int16_t)compressed : (int16_t)(-compressed);
}

// ============================================================
// Streaming playback stop flag
// ============================================================

volatile bool fxStopRequested = false;
bool          fxPlaying       = false;

// ============================================================
// Color palette  (kept for future overlay use)
// ============================================================

#define COL_BG         0x0000
#define COL_HEADER     0x001F
#define COL_REC        0xF800
#define COL_PLAY       0x07E0
#define COL_IDLE       0x7BEF
#define COL_TEXT       0xFFFF
#define COL_DIM        0x8410
#define COL_YELLOW     0xFFE0
#define COL_CYAN       0x07FF
#define COL_ORANGE     0xFD20
#define COL_PURPLE     0xF81F
#define COL_ALIEN      0x07E0
#define COL_RADIO      0xFD20
#define COL_CHIPMUNK   0xFFC0
#define COL_VILLAIN    0x780F
#define COL_CAVE       0xC618

// ============================================================
// UI state
// ============================================================

enum UIState { UI_IDLE, UI_RECORDING, UI_PLAYING };

UIState       uiState            = UI_IDLE;
UIState       lastButtonState    = (UIState)-1;  // force draw on first call
int           lastSliderVolume   = -1;
int           lastSliderDigi     = -1;
int           lastSliderMic      = -1;
int           lastSliderPitch    = -1;
int           lastSliderFreq1    = -1;
int           lastSliderFreq2    = -1;
int           lastSliderDelay    = -1;
int           lastEffectSprite   = -2;
int           lastDrcMode        = -1;
int           lastAlcMode        = -1;
unsigned long lastDisplayUpdate  = 0;
const unsigned long DISPLAY_UPDATE_MS = 100;
bool          needsFullRedraw    = true;
float         lastDisplayedSeconds = -1;

// ---- Recording click suppression ----
// Start: wait for FN release, then delay before arming the mic.
// Stop:  triggered on FN press, last N ms trimmed from the file.
bool          recStartPending      = false;
unsigned long recStartAt           = 0;
unsigned long recCooldownUntil     = 0;   // ignore FN release until this time
const unsigned long REC_START_DELAY_MS  = 80;
const unsigned long REC_COOLDOWN_MS     = 500; // block restart for 500ms after stop
const uint32_t      REC_TRIM_END_MS     = 80;

// ---- Info screen ----
bool          infoScreenActive   = false;
int           infoPage           = 0;
static const int INFO_PAGES      = 3;

// ---- Battery monitor ----
// G10 has 100k/100k voltage divider → ADC sees vbat/2
// ESP32-S3 ADC1, 12-bit, 3.3V ref, attenuation 11dB for full 0-3.3V range
#define BAT_ADC_PIN        10          // GPIO10 = ADC1 channel
#define BAT_VREF_MV        3300        // ADC full-scale reference mV
#define BAT_ADC_MAX        4095        // 12-bit
#define BAT_DIVIDER        2.0f        // 100k+100k divider ratio
#define BAT_FULL_MV        4200        // 100% — Li-ion fully charged
#define BAT_EMPTY_MV       3300        // 0%  — cutoff
#define BAT_SAMPLES        8           // oversample to reduce noise
#define BAT_UPDATE_MS      5000        // read every 5 seconds
#define BAT_X              190         // left edge of battery display
#define BAT_Y              128         // top of battery display
#define BAT_BG             0x5A48      // background colour at that region

unsigned long lastBatUpdate   = 0;
int           lastBatMv       = -1;    // last displayed value (mV)
bool          lastBatCharging = false;
// Charging detection: compare two readings 5s apart — if voltage rising, charging
int           prevBatMv       = -1;
uint8_t       screenBrightness   = 128;   // 0-255, persisted in NVS
static const uint8_t BRIGHT_STEP = 16;

// ============================================================
// Default settings — applied once at startup, never persisted.
// ============================================================

void initSettings() {
    audio.setVolume(100);
    audio.setMicGain(2);   // 2 × 3dB = 6dB
    digiVolume = 1.0f;
    effects.setMixFreq1(0);
    effects.setMixFreq2(0);
    effects.setDelayMs(0);
    audio.setAlcMode(ES8311Audio::ALC_OFF);
    audio.setDrcMode(ES8311Audio::DRC_OFF);
    screenBrightness = 128;
    lcd.setBrightness(screenBrightness);
    effects.setPitch(0);
    effects.setEffect(VoiceEffects::EFFECT_NONE);

    Serial.println("Settings initialised to defaults");
}

// ============================================================
// ---- GUI: background + overlay sprite ----
//
// ARCHITECTURE OVERVIEW
// ---------------------
//  drawBackground()   — blits the full 240×135 RGB565 bitmap to the
//                        screen in one DMA push. Called once at startup
//                        and whenever the scene needs a full repaint.
//
//  LGFX_Sprite overlay — an off-screen canvas the same size as any
//                        animated region (e.g. a VU meter column or a
//                        knob needle). To animate:
//
//    1. Call overlay.createSprite(w, h)  (once, in setup or on demand)
//    2. Each frame:
//         overlay.fillSprite(TFT_TRANSPARENT);  // clear to transparent
//         overlay.pushImage(0,0, bmpW,bmpH, myFrameBitmap);  // draw frame
//         overlay.pushSprite(&lcd, x, y, TFT_TRANSPARENT);   // blit
//       Where TFT_TRANSPARENT is the chroma-key colour (e.g. 0x0020)
//       or just push without transparency if the sprite covers the area.
//
//    3. Because the background is a static PROGMEM image you must
//       redraw the dirty rectangle of the background before pushing
//       the new sprite frame, OR use a sprite with transparency that
//       sits on top without needing to erase:
//
//         lcd.pushImageDMA(x, y, w, h, VM3000_background + y*240 + x);
//         // then push new sprite frame on top
//
//  See drawBackground() below as the foundation.
// ============================================================

// Global overlay sprite — create/resize in setup() as needed.
// One sprite instance is reused for all animated regions to save RAM.
// (Each createSprite() call frees the previous allocation.)
LGFX_Sprite guiOverlay(&lcd);

// ---- Sprite positions ----
#define SPRITE_PLAY_X  206
#define SPRITE_PLAY_Y  105
#define SPRITE_REC_X   170
#define SPRITE_REC_Y   105

// Push a 27x18 sprite directly from PROGMEM — no sprite RAM needed.
static inline void pushSprite27x18(const uint16_t* bmp, int16_t x, int16_t y) {
    lcd.pushImage(x, y, VM3000_SPRITE_W, VM3000_SPRITE_H, bmp);
}

// Draw both button indicators to match current UI state.
void drawButtons() {
    if (uiState == lastButtonState) return;   // nothing changed — skip redraw
    lastButtonState = uiState;

    if (uiState == UI_PLAYING) {
        pushSprite27x18(VM3000_PlayON,  SPRITE_PLAY_X, SPRITE_PLAY_Y);
        pushSprite27x18(VM3000_RecOFF,  SPRITE_REC_X,  SPRITE_REC_Y);
    } else if (uiState == UI_RECORDING) {
        pushSprite27x18(VM3000_PlayOFF, SPRITE_PLAY_X, SPRITE_PLAY_Y);
        pushSprite27x18(VM3000_RecON,   SPRITE_REC_X,  SPRITE_REC_Y);
    } else {  // UI_IDLE
        pushSprite27x18(VM3000_PlayOFF, SPRITE_PLAY_X, SPRITE_PLAY_Y);
        pushSprite27x18(VM3000_RecOFF,  SPRITE_REC_X,  SPRITE_REC_Y);
    }
}

// Blit the full-screen background image.
// Reads directly from PROGMEM via pushImage — no RAM copy needed.
// Draw the volume slider.
// SliderCLC is painted first to clear the previous knob position,
// then the knob is placed at the y coordinate mapped from volume 0-100.
// Only redraws when volume has actually changed.
void drawSlider() {
    int vol = (int)audio.getVolume();          // 0-100
    if (vol == lastSliderVolume) return;        // nothing changed — skip
    lastSliderVolume = vol;

    // 1. Repaint the CLC column (erases old knob position)
    lcd.pushImage(SLIDER_CLC_X, SLIDER_CLC_Y,
                  VM3000_CLC_W, VM3000_CLC_H,
                  vol_CLC, true);              // true = big-endian swap

    // 2. Map volume (0-100) → knob Y (SLIDER_KNOB_Y_MIN .. SLIDER_KNOB_Y_MAX)
    //    vol=0  → y=110,  vol=100 → y=20
    int knobY = SLIDER_KNOB_Y_MIN -
                (int)((float)(SLIDER_KNOB_Y_MIN - SLIDER_KNOB_Y_MAX) *
                      (float)vol / 100.0f);

    lcd.pushImage(SLIDER_KNOB_X, knobY,
                  VM3000_KNOB_W, VM3000_KNOB_H,
                  VM3000_SliderKnob, true);     // true = big-endian swap
}

// Draw the digital volume slider.
// Same logic as drawSlider() but mapped to digiVolume (1.0 – 10.0).
void drawSliderDigi() {
    // Represent as integer steps (digiVolume * 2) to avoid float comparison.
    int dv = (int)(digiVolume * 2.0f);
    if (dv == lastSliderDigi) return;   // nothing changed — skip
    lastSliderDigi = dv;

    // 1. Repaint the CLC column to erase the old knob position
    lcd.pushImage(SLIDER_DIGI_CLC_X, SLIDER_DIGI_CLC_Y,
                  VM3000_CLC_W, VM3000_CLC_H,
                  digi_vol_CLC, true);

    // 2. Map digiVolume (DIGI_VOL_MIN..DIGI_VOL_MAX) → knob Y (110..20)
    float norm = (digiVolume - DIGI_VOL_MIN) / (DIGI_VOL_MAX - DIGI_VOL_MIN);
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    int knobY = SLIDER_DIGI_KNOB_Y_MIN -
                (int)((float)(SLIDER_DIGI_KNOB_Y_MIN - SLIDER_DIGI_KNOB_Y_MAX) * norm);

    lcd.pushImage(SLIDER_DIGI_KNOB_X, knobY,
                  VM3000_KNOB_W, VM3000_KNOB_H,
                  VM3000_SliderKnob, true);
}

// Mic gain slider  (0-10 steps, 0-30 dB)
void drawSliderMic() {
    int val = (int)audio.getMicGain();   // 0-10
    if (val == lastSliderMic) return;
    lastSliderMic = val;
    lcd.pushImage(SLIDER_MIC_CLC_X, SLIDER_MIC_CLC_Y,
                  VM3000_MIC_CLC_W, VM3000_CLC_H, mic_gain_CLC, true);
    int knobY = SLIDER_MIC_KNOB_Y_MIN -
                (int)((float)(SLIDER_MIC_KNOB_Y_MIN - SLIDER_MIC_KNOB_Y_MAX) *
                      (float)val / 10.0f);
    lcd.pushImage(SLIDER_MIC_KNOB_X, knobY,
                  VM3000_KNOB_W, VM3000_KNOB_H, VM3000_SliderKnob, true);
}

// Pitch slider  (PITCH_MIN..-1 = below centre, 0 = centre, 1..PITCH_MAX = above)
void drawSliderPitch() {
    int val = effects.getPitch();   // PITCH_MIN(-6) .. PITCH_MAX(10)
    if (val == lastSliderPitch) return;
    lastSliderPitch = val;
    lcd.pushImage(SLIDER_PITCH_CLC_X, SLIDER_PITCH_CLC_Y,
                  VM3000_CLC_W, VM3000_CLC_H, pitch_CLC, true);
    float norm = (float)(val - VoiceEffects::PITCH_MIN) /
                 (float)(VoiceEffects::PITCH_MAX - VoiceEffects::PITCH_MIN);
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    int knobY = SLIDER_PITCH_KNOB_Y_MIN -
                (int)((float)(SLIDER_PITCH_KNOB_Y_MIN - SLIDER_PITCH_KNOB_Y_MAX) * norm);
    lcd.pushImage(SLIDER_PITCH_KNOB_X, knobY,
                  VM3000_KNOB_W, VM3000_KNOB_H, VM3000_SliderKnob, true);
}

// Freq 1 slider  (0-200 Hz)
void drawSliderFreq1() {
    int val = (int)effects.getMixFreq1();   // 0-200
    if (val == lastSliderFreq1) return;
    lastSliderFreq1 = val;
    lcd.pushImage(SLIDER_FREQ1_CLC_X, SLIDER_FREQ1_CLC_Y,
                  VM3000_CLC_W, VM3000_CLC_H, freq1_CLC, true);
    int knobY = SLIDER_FREQ1_KNOB_Y_MIN -
                (int)((float)(SLIDER_FREQ1_KNOB_Y_MIN - SLIDER_FREQ1_KNOB_Y_MAX) *
                      (float)val / 200.0f);
    lcd.pushImage(SLIDER_FREQ1_KNOB_X, knobY,
                  VM3000_KNOB_W, VM3000_KNOB_H, VM3000_SliderKnob, true);
}

// Freq 2 slider  (0-15 Hz)
void drawSliderFreq2() {
    int val = (int)effects.getMixFreq2();   // 0-15
    if (val == lastSliderFreq2) return;
    lastSliderFreq2 = val;
    lcd.pushImage(SLIDER_FREQ2_CLC_X, SLIDER_FREQ2_CLC_Y,
                  VM3000_FREQ2_CLC_W, VM3000_CLC_H, freq2_CLC, true);
    int knobY = SLIDER_FREQ2_KNOB_Y_MIN -
                (int)((float)(SLIDER_FREQ2_KNOB_Y_MIN - SLIDER_FREQ2_KNOB_Y_MAX) *
                      (float)val / 15.0f);
    lcd.pushImage(SLIDER_FREQ2_KNOB_X, knobY,
                  VM3000_KNOB_W, VM3000_KNOB_H, VM3000_SliderKnob, true);
}

// Delay slider  (0-1000 ms)
void drawSliderDelay() {
    int val = (int)effects.getDelayMs();   // 0-1000
    if (val == lastSliderDelay) return;
    lastSliderDelay = val;
    lcd.pushImage(SLIDER_DELAY_CLC_X, SLIDER_DELAY_CLC_Y,
                  VM3000_CLC_W, VM3000_CLC_H, del_CLC, true);
    int knobY = SLIDER_DELAY_KNOB_Y_MIN -
                (int)((float)(SLIDER_DELAY_KNOB_Y_MIN - SLIDER_DELAY_KNOB_Y_MAX) *
                      (float)val / 1000.0f);
    lcd.pushImage(SLIDER_DELAY_KNOB_X, knobY,
                  VM3000_KNOB_W, VM3000_KNOB_H, VM3000_SliderKnob, true);
}

// Draw the active voice effect sprite.
// ECLC is painted first to clear the previous sprite, then the effect
// image is pushed on top. EFFECT_NONE leaves the area showing only ECLC.
// ============================================================
// Micro 3x5 pixel font — digits 0-9, dot, minus, slash
// Each glyph is 3 cols x 5 rows, packed as 5 bytes (one per row,
// bits 2-0 = col left-to-right, 1=lit).
// ============================================================

// ============================================================
// 3×5 pixel font — digits + uppercase letters needed for grid labels
// Each glyph: 5 bytes, one per row, bits 2-0 = cols left→right
// ============================================================

static const uint8_t FONT3x5[][5] PROGMEM = {
    // --- digits 0-9 (index 0-9) ---
    { 0b111, 0b101, 0b101, 0b101, 0b111 }, // 0
    { 0b010, 0b110, 0b010, 0b010, 0b111 }, // 1
    { 0b111, 0b001, 0b111, 0b100, 0b111 }, // 2
    { 0b111, 0b001, 0b111, 0b001, 0b111 }, // 3
    { 0b101, 0b101, 0b111, 0b001, 0b001 }, // 4
    { 0b111, 0b100, 0b111, 0b001, 0b111 }, // 5
    { 0b111, 0b100, 0b111, 0b101, 0b111 }, // 6
    { 0b111, 0b001, 0b001, 0b001, 0b001 }, // 7
    { 0b111, 0b101, 0b111, 0b101, 0b111 }, // 8
    { 0b111, 0b101, 0b111, 0b001, 0b111 }, // 9
    // --- symbols (index 10-12) ---
    { 0b000, 0b000, 0b000, 0b000, 0b010 }, // 10 '.'
    { 0b000, 0b000, 0b111, 0b000, 0b000 }, // 11 '-'
    { 0b001, 0b001, 0b010, 0b100, 0b100 }, // 12 '/'
    // --- uppercase letters A-Z (index 13-38) ---
    { 0b010, 0b101, 0b111, 0b101, 0b101 }, // 13 A
    { 0b110, 0b101, 0b110, 0b101, 0b110 }, // 14 B
    { 0b011, 0b100, 0b100, 0b100, 0b011 }, // 15 C
    { 0b110, 0b101, 0b101, 0b101, 0b110 }, // 16 D
    { 0b111, 0b100, 0b110, 0b100, 0b111 }, // 17 E
    { 0b111, 0b100, 0b110, 0b100, 0b100 }, // 18 F
    { 0b011, 0b100, 0b111, 0b101, 0b011 }, // 19 G
    { 0b101, 0b101, 0b111, 0b101, 0b101 }, // 20 H
    { 0b111, 0b010, 0b010, 0b010, 0b111 }, // 21 I
    { 0b001, 0b001, 0b001, 0b101, 0b111 }, // 22 J  (flipped — fits 3px)
    { 0b101, 0b110, 0b100, 0b110, 0b101 }, // 23 K
    { 0b100, 0b100, 0b100, 0b100, 0b111 }, // 24 L
    { 0b101, 0b111, 0b111, 0b101, 0b101 }, // 25 M
    { 0b101, 0b111, 0b111, 0b111, 0b101 }, // 26 N
    { 0b010, 0b101, 0b101, 0b101, 0b010 }, // 27 O
    { 0b110, 0b101, 0b110, 0b100, 0b100 }, // 28 P
    { 0b010, 0b101, 0b101, 0b111, 0b011 }, // 29 Q
    { 0b110, 0b101, 0b110, 0b101, 0b101 }, // 30 R
    { 0b111, 0b100, 0b111, 0b001, 0b111 }, // 31 S (same as 5)
    { 0b111, 0b010, 0b010, 0b010, 0b010 }, // 32 T
    { 0b101, 0b101, 0b101, 0b101, 0b111 }, // 33 U
    { 0b101, 0b101, 0b101, 0b010, 0b010 }, // 34 V
    { 0b101, 0b101, 0b111, 0b111, 0b101 }, // 35 W
    { 0b101, 0b101, 0b010, 0b101, 0b101 }, // 36 X
    { 0b101, 0b101, 0b010, 0b010, 0b010 }, // 37 Y
    { 0b111, 0b001, 0b010, 0b100, 0b111 }, // 38 Z
    // --- colon (index 39) ---
    { 0b000, 0b010, 0b000, 0b010, 0b000 }, // 39 ':'
    { 0b101, 0b001, 0b010, 0b100, 0b101 }, // 40 '%'
};

// Draw a single 3x5 glyph at pixel (px, py).
static void drawGlyph(int px, int py, uint8_t glyphIdx, uint16_t colour) {
    for (int row = 0; row < 5; row++) {
        uint8_t bits = pgm_read_byte(&FONT3x5[glyphIdx][row]);
        for (int col = 0; col < 3; col++) {
            if (bits & (0x04 >> col)) {
                lcd.drawPixel(px + col, py + row, colour);
            }
        }
    }
}

// Draw a string at (px, py). Chars are 3px wide + 1px gap.
// Supported: '0'-'9', 'A'-'Z', '.', '-', '/', ':'
static void drawTinyString(int px, int py, const char* str, uint16_t colour) {
    int x = px;
    while (*str) {
        uint8_t idx = 255;
        char c = *str++;
        if      (c >= '0' && c <= '9') idx = c - '0';
        else if (c >= 'A' && c <= 'Z') idx = c - 'A' + 13;
        else if (c == '.')             idx = 10;
        else if (c == '-')             idx = 11;
        else if (c == '/')             idx = 12;
        else if (c == ':')             idx = 39;
        else if (c == '%')             idx = 40;
        if (idx != 255) {
            drawGlyph(x, py, idx, colour);
            x += 4;   // 3px glyph + 1px gap
        } else {
            x += 4;   // space
        }
    }
}

// ============================================================
// 3×3 grid values panel (59×48 px, black BG)
//
// Layout (cols 20|20|19 px, rows 16 px each):
//   [VOL  ] [DIG  ] [MIC  ]
//   [PIT  ] [FR1  ] [FR2  ]
//   [DLY  ] [DRC  ] [ALC  ]
//
// Each cell: label row (top, dim cyan) + value row (bottom, white)
// ============================================================

// Returns first letter of DRC mode name
static char drcInitial() {
    switch (audio.getDrcMode()) {
        case ES8311Audio::DRC_MED:   return 'M';
        case ES8311Audio::DRC_HARD:  return 'H';
        case ES8311Audio::DRC_CRUSH: return 'C';
        case ES8311Audio::DRC_NUKE:  return 'N';
        default:                     return 'O';  // Off
    }
}

// Returns first letter of ALC mode name
static char alcInitial() {
    switch (audio.getAlcMode()) {
        case ES8311Audio::ALC_LOW:  return 'L';
        case ES8311Audio::ALC_MID:  return 'M';
        case ES8311Audio::ALC_HIGH: return 'H';
        default:                    return 'O';  // Off
    }
}

void drawValuesPanel() {
    // Colours
    const uint16_t WHITE  = 0xFFFF;
    const uint16_t LABEL  = 0x4A69;   // dim blue-grey for label row

    // Grid origin
    const int GX = EFFECT_SPRITE_X;   // 171
    const int GY = EFFECT_SPRITE_Y;   // 10
    const int COL_W[3] = { 20, 20, 19 };
    const int ROW_H    = 16;

    // Cell x origins
    int cx[3];
    cx[0] = GX;
    cx[1] = GX + COL_W[0];
    cx[2] = GX + COL_W[0] + COL_W[1];

    // Cell y origins
    int ry[3];
    ry[0] = GY;
    ry[1] = GY + ROW_H;
    ry[2] = GY + ROW_H * 2;

    // Helper: draw one cell — label at top, value below
    // lx,ly = top-left of cell; lbl = 3-char label; val = value string
    auto drawCell = [&](int lx, int ly, const char* lbl, const char* val) {
        int lbl_x = lx + 2;
        int val_x = lx + 2;
        int lbl_y = ly + 1;          // 1px padding top
        int val_y = ly + 8;          // label(5px) + 2px gap
        drawTinyString(lbl_x, lbl_y, lbl, LABEL);
        drawTinyString(val_x, val_y, val, WHITE);
    };

    char buf[8];

    // Row 0
    snprintf(buf, sizeof(buf), "%d", (int)audio.getVolume());
    drawCell(cx[0], ry[0], "VOL", buf);

    int dv = (int)(digiVolume * 10.0f + 0.5f);
    snprintf(buf, sizeof(buf), "%d.%d", dv / 10, dv % 10);
    drawCell(cx[1], ry[0], "DIG", buf);

    snprintf(buf, sizeof(buf), "%d", (int)audio.getMicGain() * 3);
    drawCell(cx[2], ry[0], "MIC", buf);

    // Row 1
    int pitch = effects.getPitch();
    if (pitch < 0) snprintf(buf, sizeof(buf), "-%d", -pitch);
    else           snprintf(buf, sizeof(buf), "%d",   pitch);
    drawCell(cx[0], ry[1], "PIT", buf);

    snprintf(buf, sizeof(buf), "%d", (int)effects.getMixFreq1());
    drawCell(cx[1], ry[1], "FR1", buf);

    snprintf(buf, sizeof(buf), "%d", (int)effects.getMixFreq2());
    drawCell(cx[2], ry[1], "FR2", buf);

    // Row 2
    snprintf(buf, sizeof(buf), "%d", (int)effects.getDelayMs());
    drawCell(cx[0], ry[2], "DLY", buf);

    buf[0] = drcInitial(); buf[1] = '\0';
    drawCell(cx[1], ry[2], "DRC", buf);

    buf[0] = alcInitial(); buf[1] = '\0';
    drawCell(cx[2], ry[2], "ALC", buf);
}

void drawEffectSprite() {
    int val = (int)effects.getEffect();
    if (val == lastEffectSprite) return;
    lastEffectSprite = val;

    // Always clear first with ECLC
    lcd.pushImage(EFFECT_SPRITE_X, EFFECT_SPRITE_Y,
                  VM3000_EFFECT_W, VM3000_EFFECT_H,
                  VM3000_EffectCLC, true);

    if (val == (int)VoiceEffects::EFFECT_NONE) {
        // No effect active — draw values panel instead
        drawValuesPanel();
        return;
    }

    const uint16_t* bmp = nullptr;
    switch ((VoiceEffects::VoiceEffect)val) {
        case VoiceEffects::EFFECT_ROBOT:    bmp = VM3000_EffectRobot;    break;
        case VoiceEffects::EFFECT_GHOST:    bmp = VM3000_EffectGhost;    break;
        case VoiceEffects::EFFECT_ALIEN:    bmp = VM3000_EffectAlien;    break;
        case VoiceEffects::EFFECT_RADIO:    bmp = VM3000_EffectRadio;    break;
        case VoiceEffects::EFFECT_CHIPMUNK: bmp = VM3000_EffectChipmunk; break;
        case VoiceEffects::EFFECT_VILLAIN:  bmp = VM3000_EffectVillain;  break;
        case VoiceEffects::EFFECT_CAVE:     bmp = VM3000_EffectCave;     break;
        default: break;
    }
    if (bmp) {
        lcd.pushImage(EFFECT_SPRITE_X, EFFECT_SPRITE_Y,
                      VM3000_EFFECT_W, VM3000_EFFECT_H,
                      bmp, true);
    }
}

// DRC knob — 5 states: OFF=Knob1, SOFT=Knob2, MED=Knob3, HARD=Knob4, CRUSH=Knob5
// Refresh the values panel in-place when a value changes and EFFECT_NONE is active.
// Repaints ECLC first to clear old digits, then redraws all 7 rows.
void refreshValuesPanel() {
    if (effects.getEffect() != VoiceEffects::EFFECT_NONE) return;
    lcd.pushImage(EFFECT_SPRITE_X, EFFECT_SPRITE_Y,
                  VM3000_EFFECT_W, VM3000_EFFECT_H,
                  VM3000_EffectCLC, true);
    drawValuesPanel();
}

void drawDrcKnob() {
    int val = (int)audio.getDrcMode();
    if (val == lastDrcMode) return;
    lastDrcMode = val;

    const uint16_t* bmp = VM3000_Knob1;   // default = OFF
    switch ((ES8311Audio::DrcMode)val) {
        case ES8311Audio::DRC_MED:   bmp = VM3000_Knob2; break;
        case ES8311Audio::DRC_HARD:  bmp = VM3000_Knob3; break;
        case ES8311Audio::DRC_CRUSH: bmp = VM3000_Knob4; break;
        case ES8311Audio::DRC_NUKE:  bmp = VM3000_Knob5; break;
        default: break;   // DRC_OFF → Knob1
    }
    lcd.pushImage(DRC_KNOB_X, DRC_KNOB_Y,
                  VM3000_KNOB_BMP_W, VM3000_KNOB_BMP_H,
                  bmp, true);
}

// ALC knob — 4 states: OFF=Knob1, LOW=Knob2, MID=Knob3, HIGH=Knob4
void drawAlcKnob() {
    int val = (int)audio.getAlcMode();
    if (val == lastAlcMode) return;
    lastAlcMode = val;

    const uint16_t* bmp = VM3000_Knob1;   // default = OFF
    switch ((ES8311Audio::AlcMode)val) {
        case ES8311Audio::ALC_LOW:  bmp = VM3000_Knob2; break;
        case ES8311Audio::ALC_MID:  bmp = VM3000_Knob3; break;
        case ES8311Audio::ALC_HIGH: bmp = VM3000_Knob4; break;
        default: break;   // ALC_OFF → Knob1
    }
    lcd.pushImage(ALC_KNOB_X, ALC_KNOB_Y,
                  VM3000_KNOB_BMP_W, VM3000_KNOB_BMP_H,
                  bmp, true);
}

// ============================================================
// Info screen
// BG: #AD9A7D → 0xACCF   Text: #362C2E → 0x3165
// Two pages, navigated with A (prev) / Z (next), closed with
// DEL, OK, FN, or any of the trigger keys.
// ============================================================

#define INFO_BG   0xACCF
#define INFO_TEXT 0x0000
#define INFO_HEAD 0x0000   // black for page header

// Content — two pages, max 14 rows each (Font0 size1 = 8px, 1px gap = 9px/line)
// Page 1: transport + sliders
static const char* const INFO_PAGE0[] PROGMEM = {
    " VOICE MIXER 3000   1/3",
    "-----------------------",
    "FN        Record / Stop",
    "OK        Play / Stop",
    "B / V     Bright up/down",
    "",
    "q/1  Vol down/up......0 => 100%",
    "w/2  DigiVol down/up......1 => 10x",
    "e/3  MicGain down/up......0 => 30dB",
    "r/4  Pitch down/up......-6 => 10",
    "t/5  Freq1 down/up......0 => 200Hz",
    "y/6  Freq2 down/up......0 => 15Hz",
    "u/7  Delay down/up......0 => 1000ms",
};
static const int INFO_PAGE0_LINES = 13;

// Page 2: effects, DRC, ALC states
static const char* const INFO_PAGE1[] PROGMEM = {
    " VOICE MIXER 3000   2/3",
    "-----------------------",
    "0   Cycle voice effect",
    "-   Cycle DRC",
    "+   Cycle ALC",
    "",
    "Effects (key 0):",
    " None / Robot / Ghost / Alien / Radio",
    " Chipmunk / Villain / Cave",
    "",
    "DRC: Off / Med / Hard / Crush / NUKE",
    "",
    "ALC: Off / Low / Mid / High",
};
static const int INFO_PAGE1_LINES = 14;

// Page 3: DRC and ALC explained
static const char* const INFO_PAGE2[] PROGMEM = {
    " VOICE MIXER 3000   3/3",
    "-----------------------",
    "DRC - Dynamic Range",
    "Compression (playback):",
    " Reduces loud peaks so",
    " output stays consistent.",
    " Med=gentle  Crush=hard",
    " NUKE=brick limit at max",
    "",
    "ALC - Auto Level Control",
    "(recording mic input):",
    " Auto-adjusts mic gain",
    " to avoid clipping.",
    " Low=slow  High=fast",
    "",
};
static const int INFO_PAGE2_LINES = 15;

// Bold text helper: draw string twice — second pass 1px right.
// Works with any font, no extra memory needed.
static void drawBoldString(const char* str, int x, int y) {
    lcd.drawString(str, x,   y);
    lcd.drawString(str, x+1, y);
}

void drawInfoScreen() {
    lcd.fillScreen(INFO_BG);
    lcd.setFont(&fonts::Font0);
    lcd.setTextSize(1);

    const char* const* lines;
    int count;
    switch (infoPage) {
        case 1:  lines = INFO_PAGE1; count = INFO_PAGE1_LINES; break;
        case 2:  lines = INFO_PAGE2; count = INFO_PAGE2_LINES; break;
        default: lines = INFO_PAGE0; count = INFO_PAGE0_LINES; break;
    }

    // Line height: Font0 = 8px tall, 1px gap
    const int LINE_H = 9;
    // Footer hint drawn at bottom
    const int FOOTER_Y = 135 - 8 - 1;

    for (int i = 0; i < count; i++) {
        const char* line = (const char*)pgm_read_ptr(&lines[i]);
        int y = 2 + i * LINE_H;
        if (y + 8 > FOOTER_Y) break;

        if (i < 2) {
            // Header: black, bold
            lcd.setTextColor(INFO_HEAD, INFO_BG);
            drawBoldString(line, 2, y);
        } else if (*line == '\0') {
            // Empty line — skip drawing
        } else {
            lcd.setTextColor(INFO_TEXT, INFO_BG);
            drawBoldString(line, 2, y);
        }
    }

    // Fixed footer navigation hint
    lcd.setTextColor(INFO_HEAD, INFO_BG);
    char navBuf[40];
    snprintf(navBuf, sizeof(navBuf),
             "[<] prev  [>] next  [DEL/ESC] close");
    drawBoldString(navBuf, 2, FOOTER_Y);
}

void openInfoScreen() {
    infoScreenActive = true;
    infoPage = 0;
    drawInfoScreen();
}

void closeInfoScreen() {
    infoScreenActive = false;
    needsFullRedraw = true;
}

// ---- Battery monitor ------------------------------------------------

// Read battery voltage in mV using oversampled ADC.
// GPIO10 feeds ADC1 through 100k/100k divider, so vbat = vadc * 2.
int readBatteryMv() {
    analogSetAttenuation(ADC_11db);   // 0-3.3V range
    long sum = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        sum += analogReadMilliVolts(BAT_ADC_PIN);
        delayMicroseconds(200);
    }
    int vadc = (int)(sum / BAT_SAMPLES);
    return (int)(vadc * BAT_DIVIDER);
}

// Draw the battery voltage + charging status at the bottom-right corner.
// Uses the 3x5 micro font already in the sketch.
// Clears its own region with BAT_BG before drawing.
void drawBattery(int mv, bool charging) {
    // Erase region: 74px wide × 7px tall (fits 5px font + 1px top/bottom)
    lcd.fillRect(BAT_X, BAT_Y, 74, 7, BAT_BG);

    // Format: "CHG 4.18V" or "    4.18V" (spaces keep width consistent)
    char buf[12];
    int  volts   = mv / 1000;
    int  millis_ = (mv % 1000) / 10;   // two decimal places

    if (charging) {
        snprintf(buf, sizeof(buf), "CHG %d.%02dV", volts, millis_);
    } else {
        // Show percent instead of prefix
        int pct = (mv - BAT_EMPTY_MV) * 100 / (BAT_FULL_MV - BAT_EMPTY_MV);
        if (pct > 100) pct = 100;
        if (pct <   0) pct = 0;
        snprintf(buf, sizeof(buf), "%3d%% %d.%02dV", pct, volts, millis_);
    }

    // Colour: green if charging, yellow if >50%, orange if >20%, red if low
    uint16_t col;
    if (charging) {
        col = 0x07E0;   // green
    } else {
        int pct = (mv - BAT_EMPTY_MV) * 100 / (BAT_FULL_MV - BAT_EMPTY_MV);
        if      (pct > 50) col = 0xFFFF;   // white
        else if (pct > 20) col = 0xFFE0;   // yellow
        else               col = 0xF800;   // red
    }

    drawTinyString(BAT_X, BAT_Y + 1, buf, col);
}

void drawBackground() {
    lastButtonState  = (UIState)-1;
    lastSliderVolume = -1;
    lastSliderDigi   = -1;
    lastSliderMic    = -1;
    lastSliderPitch  = -1;
    lastSliderFreq1  = -1;
    lastSliderFreq2  = -1;
    lastSliderDelay  = -1;
    lastEffectSprite = -2;
    lastDrcMode      = -1;
    lastAlcMode      = -1;
    lastBatMv        = -1;   // force battery redraw after background blit
    lcd.pushImage(0, 0, VM3000_BG_W, VM3000_BG_H, VM3000_background);
    drawButtons();
    drawSlider();
    drawSliderDigi();
    drawSliderMic();
    drawSliderPitch();
    drawSliderFreq1();
    drawSliderFreq2();
    drawSliderDelay();
    drawEffectSprite();
    drawDrcKnob();
    drawAlcKnob();
    // Draw last known battery state immediately (loop will refresh it)
    if (prevBatMv > 0) drawBattery(prevBatMv, lastBatCharging);
}

// Convenience: redraw a rectangular patch of the background
// (use this before pushing a new animated sprite frame over it).
void redrawBgRect(int16_t x, int16_t y, int16_t w, int16_t h) {
    // Clamp to screen
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > VM3000_BG_W) w = VM3000_BG_W - x;
    if (y + h > VM3000_BG_H) h = VM3000_BG_H - y;
    if (w <= 0 || h <= 0) return;

    // Push only the rows that overlap with [y .. y+h)
    for (int16_t row = y; row < y + h; row++) {
        lcd.pushImage(x, row,
                      w, 1,
                      VM3000_background + row * VM3000_BG_W + x);
    }
}

// ============================================================
// Legacy display helpers — kept intact, currently not called.
// Re-wire these into drawBackground() overlay logic as needed.
// ============================================================

void drawHeader() {
    lcd.fillRect(0, 0, 240, 20, COL_HEADER);
    lcd.setTextColor(COL_TEXT, COL_HEADER);
    lcd.setTextSize(1);
    lcd.setFont(&fonts::Font2);
    lcd.drawCenterString("Voice Memo", 120, 2);
}

uint16_t effectColor() {
    switch (effects.getEffect()) {
        case VoiceEffects::EFFECT_ROBOT:      return COL_CYAN;
        case VoiceEffects::EFFECT_GHOST:      return COL_PURPLE;
        case VoiceEffects::EFFECT_ALIEN:      return COL_ALIEN;
        case VoiceEffects::EFFECT_RADIO:      return COL_RADIO;
        case VoiceEffects::EFFECT_CHIPMUNK:   return COL_CHIPMUNK;
        case VoiceEffects::EFFECT_VILLAIN:    return COL_VILLAIN;
        case VoiceEffects::EFFECT_CAVE:       return COL_CAVE;
        default:                              return COL_DIM;
    }
}

void drawStatusBar() {
    // ... (legacy — not currently drawn)
}

void drawControls() {
    // ... (legacy — not currently drawn)
}

void drawFullUI() {
    drawBackground();
    needsFullRedraw      = false;
    lastDisplayedSeconds = -1;
}

void updateTimerOnly() {
    // Buttons are drawn only on state change via drawBackground() → drawButtons().
    // Nothing else needs periodic refresh in background-only mode yet.
}

void showEffectStatus(const char* msg) {
    // No-op in background-only mode.
    // TODO: push a translucent status sprite near the display area on the bg.
    (void)msg;
}

// ============================================================
// I2S pins (match ES8311Audio)
// ============================================================

static const int PIN_I2S_SCLK = 41;
static const int PIN_I2S_LRCK = 43;
static const int PIN_I2S_DOUT = 42;

// ============================================================
// playWithEffects - STREAMING version
// ============================================================

void playWithEffects() {
    if (!effects.isAnyEffectActive() && digiVolume <= 1.01f) {
        recorder.startPlayback();
        return;
    }

    File f = SD.open("/VoiceMemos/current.wav", FILE_READ);
    if (!f) {
        Serial.println("FX: cannot open file");
        recorder.startPlayback();
        return;
    }

    uint8_t hdr[44];
    bool headerOk = (f.size() >= 44) &&
                    (f.read(hdr, 44) == 44) &&
                    (memcmp(hdr,     "RIFF", 4) == 0) &&
                    (memcmp(hdr + 8, "WAVE", 4) == 0) &&
                    (*(uint16_t*)(hdr + 20) == 1)  &&
                    (*(uint16_t*)(hdr + 22) == 1)  &&
                    (*(uint16_t*)(hdr + 34) == 16);

    if (!headerOk) {
        Serial.println("FX: bad WAV header");
        f.close();
        recorder.startPlayback();
        return;
    }

    uint32_t sampleRate = *(uint32_t*)(hdr + 24);
    uint32_t dataSize   = *(uint32_t*)(hdr + 40);
    int32_t  numSamples = (int32_t)(dataSize / 2);

    Serial.printf("FX stream: %d samples, %dHz, effect=%d, pitch=%d, digiVol=%.1f\n",
                  numSamples, sampleRate,
                  (int)effects.getEffect(), effects.getPitch(), digiVolume);

    if (!effects.beginStream(sampleRate)) {
        Serial.println("FX: beginStream failed (alloc?)");
        f.close();
        recorder.startPlayback();
        return;
    }

    audio.enableSpkForStream();

    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = sampleRate;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 256;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;
    cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    cfg.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = I2S_PIN_NO_CHANGE;
    pins.bck_io_num   = PIN_I2S_SCLK;
    pins.ws_io_num    = PIN_I2S_LRCK;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;

    if (i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL) != ESP_OK) {
        Serial.println("FX: I2S install failed");
        effects.endStream();
        f.close();
        recorder.startPlayback();
        return;
    }
    i2s_set_pin(I2S_NUM_1, &pins);
    i2s_zero_dma_buffer(I2S_NUM_1);
    delay(100);

    showEffectStatus("Playing with FX...");

    fxPlaying = true;
    fxStopRequested = false;

    // loop() is blocked during playWithEffects(), so we must update
    // uiState and the Play button manually right now.
    uiState = UI_PLAYING;
    drawButtons();

    const int CHUNK = VoiceEffects::MAX_CHUNK;
    int16_t readBuf[CHUNK];
    int16_t procBuf[CHUNK * 2];
    int16_t stereoBuf[CHUNK * 2 * 2];

    int32_t samplesRemaining = numSamples;
    uint32_t t0 = millis();
    size_t bw;

    while (samplesRemaining > 0 && !fxStopRequested) {
        keyboard.update();
        if (keyboard.wasPressed("ok")) {
            Serial.println("FX: stop requested during playback");
            break;
        }

        int32_t toRead = min((int32_t)CHUNK, samplesRemaining);
        size_t bytesRead = f.read((uint8_t*)readBuf, toRead * 2);
        int32_t got = (int32_t)(bytesRead / 2);
        if (got <= 0) break;

        int32_t outCount = effects.processChunk(readBuf, got,
                                                 procBuf, CHUNK * 2);

        if (outCount > 0) {
            for (int32_t i = 0; i < outCount; i++) {
                int16_t s = softLimit((float)procBuf[i] * digiVolume);
                stereoBuf[i * 2]     = s;
                stereoBuf[i * 2 + 1] = s;
            }
            i2s_write(I2S_NUM_1, stereoBuf, outCount * 4, &bw, portMAX_DELAY);
        }

        samplesRemaining -= got;
    }
    f.close();

    while (effects.hasTail() && !fxStopRequested) {
        keyboard.update();
        if (keyboard.wasPressed("ok")) {
            Serial.println("FX: stop requested during tail");
            break;
        }

        int32_t tailOut = effects.flushTail(procBuf, CHUNK * 2);
        if (tailOut <= 0) break;

        for (int32_t i = 0; i < tailOut; i++) {
            int16_t s = softLimit((float)procBuf[i] * digiVolume);
            stereoBuf[i * 2]     = s;
            stereoBuf[i * 2 + 1] = s;
        }
        i2s_write(I2S_NUM_1, stereoBuf, tailOut * 4, &bw, portMAX_DELAY);
    }

    memset(stereoBuf, 0, sizeof(stereoBuf));
    for (int i = 0; i < 4; i++) {
        i2s_write(I2S_NUM_1, stereoBuf, sizeof(stereoBuf), &bw, 10);
    }
    delay(50);

    effects.endStream();
    i2s_driver_uninstall(I2S_NUM_1);

    fxPlaying = false;
    fxStopRequested = false;

    // Restore IDLE button state immediately (loop() hasn't run yet).
    uiState = UI_IDLE;
    drawButtons();

    Serial.printf("FX stream: done in %dms\n", (int)(millis() - t0));
    needsFullRedraw = true;
}

// ============================================================
// Setup
// ============================================================

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n=== Voice Mixer 3000 ===");

    lcd.init();
    lcd.setRotation(1);
    lcd.setBrightness(screenBrightness);

    Wire.begin(8, 9, 400000);
    delay(50);

    keyboard.begin(&Wire, 0x34);

    audio.begin(&Wire);
    if (!audio.testConnection()) {
        // Minimal error indicator: red tint in top-left corner
        lcd.fillRect(0, 0, 60, 14, COL_REC);
        lcd.setFont(&fonts::Font0);
        lcd.setTextColor(COL_TEXT, COL_REC);
        lcd.drawString("NO CODEC", 2, 3);
        Serial.println("ES8311 not found!");
        while (1) delay(1000);
    }

    if (psramFound()) {
        Serial.printf("PSRAM: %d bytes available\n", ESP.getFreePsram());
    } else {
        Serial.println("No PSRAM detected (not needed for streaming FX)");
    }

    initSettings();

    bool sdOk = recorder.begin(&audio, &effects);
    if (!sdOk) {
        // Small SD error badge — doesn't destroy the background
        lcd.fillRect(0, 0, 60, 14, COL_ORANGE);
        lcd.setFont(&fonts::Font0);
        lcd.setTextColor(COL_TEXT, COL_ORANGE);
        lcd.drawString("NO SD", 2, 3);
        delay(2000);
    }

    // Draw background AFTER initSettings() so the slider knob
    // reflects the correct default values.
    drawBackground();

    needsFullRedraw = false;
    Serial.println("Ready!");
}

// ============================================================
// Loop
// ============================================================

void loop() {
    keyboard.update();
    recorder.update();

    // ---- Info screen: check trigger keys first (always active) ----

    bool triggerInfo = keyboard.wasPressed("del")   ||
                       keyboard.wasPressed("space") ||
                       keyboard.wasPressed("ctrl")  ||
                       keyboard.wasPressed("shift") ||
                       keyboard.wasPressed("alt")   ||
                       keyboard.wasPressed("opt")   ||
                       keyboard.wasPressed("~")     ||
                       keyboard.wasPressed("`");

    bool closeInfo   = keyboard.wasPressed("del")   ||
                       keyboard.wasPressed("~")     ||
                       keyboard.wasPressed("`")     ||
                       keyboard.wasPressed("space") ||
                       keyboard.wasPressed("ok");

    if (infoScreenActive) {
        // , = previous page,  / = next page
        if (keyboard.wasPressed(",")) {
            if (infoPage > 0) { infoPage--; drawInfoScreen(); }
        }
        if (keyboard.wasPressed("/")) {
            if (infoPage < INFO_PAGES - 1) { infoPage++; drawInfoScreen(); }
        }
        if (closeInfo) {
            closeInfoScreen();
        }
        delay(5);
        return;   // swallow all other keys while info is open
    }

    if (triggerInfo) {
        openInfoScreen();
        delay(5);
        return;
    }

    // ---- Normal operation ----

    UIState prevState = uiState;

    if      (recorder.isRecording())            uiState = UI_RECORDING;
    else if (recorder.isPlaying() || fxPlaying) uiState = UI_PLAYING;
    else                                         uiState = UI_IDLE;

    if (uiState != prevState) needsFullRedraw = true;

    // ---- Recording ----
    // START: on FN release, arm mic after REC_START_DELAY_MS
    // STOP:  on FN press, trim end and set cooldown to block immediate restart

    if (recorder.isRecording()) {
        if (keyboard.wasPressed("fn")) {
            Serial.println("KEY: Fn — stop recording (trimmed)");
            recorder.stopRecordingTrimmed(REC_TRIM_END_MS);
            recStartPending  = false;
            recCooldownUntil = millis() + REC_COOLDOWN_MS;
        }
    } else {
        if (!recorder.isBusy() && !fxPlaying) {
            // Only respond to release if cooldown has expired
            if (millis() > recCooldownUntil && keyboard.wasReleased("fn")) {
                Serial.println("KEY: Fn released — arming mic");
                recStartPending = true;
                recStartAt      = millis() + REC_START_DELAY_MS;
            }
        }
        if (recStartPending && millis() >= recStartAt) {
            recStartPending = false;
            if (!recorder.isBusy() && !fxPlaying) {
                if (recorder.sdReady()) {
                    Serial.println("Starting recording (delayed start)");
                    recorder.startRecording(16000);
                } else {
                    Serial.println("No SD card!");
                }
            }
        }
    }

    // ---- Playback ----

    if (keyboard.wasPressed("ok")) {
        Serial.println("KEY: OK");
        if (fxPlaying) {
            fxStopRequested = true;
            Serial.println("FX stop requested");
        } else if (recorder.isPlaying()) {
            recorder.stopPlayback();
        } else if (!recorder.isBusy()) {
            if (recorder.hasRecording()) playWithEffects();
            else Serial.println("No recording found");
        }
    }

    // ---- Tone ----

    // ---- Volume (q = down, 1 = up) ----

    if (keyboard.wasPressed("q")) {
        uint8_t v = audio.getVolume();
        audio.setVolume(v >= 10 ? v - 10 : 0);
        drawSlider(); refreshValuesPanel();
    }
    if (keyboard.wasPressed("1")) {
        uint8_t v = audio.getVolume();
        audio.setVolume(v <= 90 ? v + 10 : 100);
        drawSlider(); refreshValuesPanel();
    }

    // ---- DigiVolume (w = down, 2 = up) ----

    if (keyboard.wasPressed("w")) {
        digiVolume -= DIGI_VOL_STEP;
        if (digiVolume < DIGI_VOL_MIN) digiVolume = DIGI_VOL_MIN;
        Serial.printf("DigiVol: %.1fx\n", digiVolume);
        drawSliderDigi(); refreshValuesPanel();
    }
    if (keyboard.wasPressed("2")) {
        digiVolume += DIGI_VOL_STEP;
        if (digiVolume > DIGI_VOL_MAX) digiVolume = DIGI_VOL_MAX;
        Serial.printf("DigiVol: %.1fx\n", digiVolume);
        drawSliderDigi(); refreshValuesPanel();
    }

    // ---- Mic gain (e = down, 3 = up) ----

    if (keyboard.wasPressed("e")) {
        uint8_t g = audio.getMicGain();
        audio.setMicGain(g > 0 ? g - 1 : 0);
        drawSliderMic(); refreshValuesPanel();
    }
    if (keyboard.wasPressed("3")) {
        uint8_t g = audio.getMicGain();
        audio.setMicGain(g < 10 ? g + 1 : 10);
        drawSliderMic(); refreshValuesPanel();
    }

    // ---- Pitch (r = down, 4 = up) ----

    if (keyboard.wasPressed("r")) {
        if (!recorder.isRecording()) {
            effects.pitchDown();
            drawSliderPitch(); refreshValuesPanel();
        }
    }
    if (keyboard.wasPressed("4")) {
        if (!recorder.isRecording()) {
            effects.pitchUp();
            drawSliderPitch(); refreshValuesPanel();
        }
    }

    // ---- Freq 1 (t = down, 5 = up) ----

    if (keyboard.wasPressed("t")) {
        uint16_t f = effects.getMixFreq1();
        effects.setMixFreq1(f >= 10 ? f - 10 : 0);
        Serial.printf("MixF1: %dHz\n", effects.getMixFreq1());
        drawSliderFreq1(); refreshValuesPanel();
    }
    if (keyboard.wasPressed("5")) {
        uint16_t f = effects.getMixFreq1();
        effects.setMixFreq1(f <= 190 ? f + 10 : 200);
        Serial.printf("MixF1: %dHz\n", effects.getMixFreq1());
        drawSliderFreq1(); refreshValuesPanel();
    }

    // ---- Freq 2 (y = down, 6 = up) ----

    if (keyboard.wasPressed("y")) {
        uint16_t f = effects.getMixFreq2();
        effects.setMixFreq2(f >= 1 ? f - 1 : 0);
        Serial.printf("MixF2: %dHz\n", effects.getMixFreq2());
        drawSliderFreq2(); refreshValuesPanel();
    }
    if (keyboard.wasPressed("6")) {
        uint16_t f = effects.getMixFreq2();
        effects.setMixFreq2(f <= 14 ? f + 1 : 15);
        Serial.printf("MixF2: %dHz\n", effects.getMixFreq2());
        drawSliderFreq2(); refreshValuesPanel();
    }

    // ---- Delay (u = down, 7 = up) ----

    if (keyboard.wasPressed("u")) {
        uint16_t d = effects.getDelayMs();
        effects.setDelayMs(d >= 100 ? d - 100 : 0);
        Serial.printf("Delay: %dms\n", effects.getDelayMs());
        drawSliderDelay(); refreshValuesPanel();
    }
    if (keyboard.wasPressed("7")) {
        uint16_t d = effects.getDelayMs();
        effects.setDelayMs(d <= 900 ? d + 100 : 1000);
        Serial.printf("Delay: %dms\n", effects.getDelayMs());
        drawSliderDelay(); refreshValuesPanel();
    }

    // ---- Voice effect cycle (0) ----

    if (keyboard.wasPressed("0")) {
        if (!recorder.isRecording()) {
            effects.cycleEffect();
            Serial.printf("Effect changed to: %d\n", (int)effects.getEffect());
            drawEffectSprite();
        }
    }

    // ---- DRC cycle (-) ----

    if (keyboard.wasPressed("-")) {
        audio.cycleDrc();
        char lbl[8]; audio.getDrcLabel(lbl, sizeof(lbl));
        Serial.printf("DRC: %s\n", lbl);
        drawDrcKnob(); refreshValuesPanel();
    }

    // ---- ALC cycle (+) ----

    if (keyboard.wasPressed("+")) {
        audio.cycleAlc();
        char lbl[8]; audio.getAlcLabel(lbl, sizeof(lbl));
        Serial.printf("ALC: %s\n", lbl);
        drawAlcKnob(); refreshValuesPanel();
    }

    // ---- Brightness (B = up, V = down) ----

    if (keyboard.wasPressed("b")) {
        screenBrightness = (screenBrightness <= 255 - BRIGHT_STEP)
                           ? screenBrightness + BRIGHT_STEP : 255;
        lcd.setBrightness(screenBrightness);
        Serial.printf("Brightness: %d\n", screenBrightness);
    }
    if (keyboard.wasPressed("v")) {
        screenBrightness = (screenBrightness >= BRIGHT_STEP)
                           ? screenBrightness - BRIGHT_STEP : 0;
        lcd.setBrightness(screenBrightness);
        Serial.printf("Brightness: %d\n", screenBrightness);
    }


    // ---- Battery monitor ----

    if (millis() - lastBatUpdate >= BAT_UPDATE_MS || lastBatMv == -1) {
        lastBatUpdate = millis();
        int mv = readBatteryMv();

        // Charging detection: voltage rising by >10mV since last reading
        bool charging = (prevBatMv > 0) && (mv - prevBatMv > 10);
        prevBatMv = mv;

        // Only redraw if value or charging state changed noticeably (>10mV)
        if (abs(mv - lastBatMv) > 10 || charging != lastBatCharging || lastBatMv == -1) {
            lastBatMv       = mv;
            lastBatCharging = charging;
            drawBattery(mv, charging);
        }
    }

    // ---- Display ----

    if (needsFullRedraw) {
        drawBackground();
        needsFullRedraw = false;
        lastDisplayedSeconds = -1;
    } else if (millis() - lastDisplayUpdate >= DISPLAY_UPDATE_MS) {
        updateTimerOnly();
        lastDisplayUpdate = millis();
    }

    delay(5);
}
