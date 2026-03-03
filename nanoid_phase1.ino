#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"
#include <Wire.h>

// ─── DISPLAY SETUP ────────────────────────────────────────────────────────────
// This is identical to hello_world - it tells Arduino exactly what display hardware we have
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *gfx = new Arduino_CO5300(
  bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

// ─── STATE MACHINE ────────────────────────────────────────────────────────────
// The Nanoid is always in one of these two modes
enum Mode {
  FACE_MODE,
  TEXT_MODE
};

Mode currentMode = FACE_MODE; // start in face mode

// ─── TIMING ───────────────────────────────────────────────────────────────────
// We use millis() instead of delay() so the screen never freezes
unsigned long lastBlinkTime   = 0;  // when the nanoid last blinked
unsigned long blinkDuration   = 0;  // how long the blink lasts
unsigned long textModeStart   = 0;  // when we entered text mode
bool isBlinking               = false;

// Blink every 3-5 seconds, blink lasts 150ms
const unsigned long BLINK_INTERVAL = 3000;
const unsigned long BLINK_LENGTH   = 150;

// Text mode lasts 2 seconds before returning to face
const unsigned long TEXT_MODE_DURATION = 4000;

// ─── SCREEN CENTER ────────────────────────────────────────────────────────────
const int SCREEN_CX = 233; // center X of the 466x466 screen
const int SCREEN_CY = 233; // center Y

// ─── TOUCH SETUP ──────────────────────────────────────────────────────────────
bool isTouched() {
  return digitalRead(TP_INT) == LOW;
}

// ─── DRAW FUNCTIONS ───────────────────────────────────────────────────────────

void drawFace(bool eyesOpen) {
  // Clear screen to black void
  gfx->fillScreen(RGB565_BLACK);

  // Draw the Nanoid head - white circle, floating small in the void
  // 80px radius = 160px diameter, about 34% of the 466px screen
  gfx->fillCircle(SCREEN_CX, SCREEN_CY, 80, RGB565_WHITE);

  // Draw a slightly smaller circle on top in a light grey
  // This gives the head some dimension and makes eyes readable
  gfx->fillCircle(SCREEN_CX, SCREEN_CY, 76, 0xD69A); // light grey

  if (eyesOpen) {
    // Eyes open - two filled dark circles
    // Left eye
    gfx->fillCircle(SCREEN_CX - 25, SCREEN_CY - 10, 12, RGB565_BLACK);
    // Right eye
    gfx->fillCircle(SCREEN_CX + 25, SCREEN_CY - 10, 12, RGB565_BLACK);

    // Small white highlight dots in each eye - brings them to life
    gfx->fillCircle(SCREEN_CX - 21, SCREEN_CY - 14, 4, RGB565_WHITE);
    gfx->fillCircle(SCREEN_CX + 29, SCREEN_CY - 14, 4, RGB565_WHITE);
  } else {
    // Eyes closed - just two small horizontal lines (blink)
    gfx->fillRect(SCREEN_CX - 37, SCREEN_CY - 12, 24, 4, RGB565_BLACK);
    gfx->fillRect(SCREEN_CX + 13, SCREEN_CY - 12, 24, 4, RGB565_BLACK);
  }

  // Simple neutral mouth - small dark oval
  gfx->fillCircle(SCREEN_CX, SCREEN_CY + 30, 18, RGB565_BLACK);
  gfx->fillCircle(SCREEN_CX, SCREEN_CY + 24, 18, 0xD69A); // cover top = flat bottom mouth
}

void drawTextMode() {
  // Face disappears, screen goes full black, cryptic message appears
  gfx->fillScreen(RGB565_BLACK);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(4, 4, 1);

  // Center the text manually - each char is ~12px wide at size 2
  // "HELLO." is 6 chars = ~72px, so start at center - 36
  gfx->setCursor(SCREEN_CX - 72, SCREEN_CY - 20);
  gfx->println("HELLO.");
}

// ─── SETUP ────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);

  Wire.begin(IIC_SDA, IIC_SCL);

// Initialize touch chip
pinMode(TP_RESET, OUTPUT);
digitalWrite(TP_RESET, HIGH);
pinMode(TP_INT, INPUT_PULLUP);

  if (!gfx->begin()) {
    Serial.println("Display init failed!");
  }

  gfx->setBrightness(200); // brighter than hello world default
  gfx->fillScreen(RGB565_BLACK);

  // Draw the face immediately on boot
  drawFace(true);

  lastBlinkTime = millis();
}

// ─── MAIN LOOP ────────────────────────────────────────────────────────────────

void loop() {
  unsigned long now = millis(); // current time in milliseconds

  // ── TOUCH INPUT ──
  if (currentMode == FACE_MODE && isTouched()) {
    currentMode = TEXT_MODE;
    textModeStart = now;
    drawTextMode();
    return; // skip the rest of the loop this frame
  }

  // ── TEXT MODE TIMEOUT ──
  // After 2 seconds in text mode, snap back to face
  if (currentMode == TEXT_MODE && (now - textModeStart >= TEXT_MODE_DURATION)) {
    currentMode = FACE_MODE;
    drawFace(true);
    lastBlinkTime = now;
    return;
  }

  // ── BLINK LOGIC ──
  // Only blink when in face mode
  if (currentMode == FACE_MODE) {

    if (!isBlinking && (now - lastBlinkTime >= BLINK_INTERVAL)) {
      // Time to blink - close the eyes
      isBlinking = true;
      blinkDuration = now;
      drawFace(false);
    }

    if (isBlinking && (now - blinkDuration >= BLINK_LENGTH)) {
      // Blink over - open the eyes
      isBlinking = false;
      lastBlinkTime = now;
      drawFace(true);
    }
  }
}
