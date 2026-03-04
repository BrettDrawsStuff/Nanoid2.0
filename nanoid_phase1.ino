#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"
#include <Wire.h>
#include <FS.h>
#include <SD_MMC.h>
#include <math.h>
#include "TouchDrvCSTXXX.hpp"

// ─── DISPLAY SETUP ────────────────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *gfx = new Arduino_CO5300(
  bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

// ─── TOUCH SETUP ──────────────────────────────────────────────────────────────
TouchDrvCST92xx touch;
int16_t touchX[5], touchY[5];
volatile bool touchInterrupt = false;

// ─── SPRITE DEFINITIONS ───────────────────────────────────────────────────────
#define SPRITE_COUNT 8

const char* spriteFiles[SPRITE_COUNT] = {
  "/normal.bmp",
  "/normal_blink.bmp",
  "/happy.bmp",
  "/sad.bmp",
  "/mad.bmp",
  "/scared.bmp",
  "/disgust.bmp",
  "/confused.bmp"
};

#define SPR_NORMAL    0
#define SPR_BLINK     1
#define SPR_HAPPY     2
#define SPR_SAD       3
#define SPR_MAD       4
#define SPR_SCARED    5
#define SPR_DISGUST   6
#define SPR_CONFUSED  7

struct Sprite {
  uint16_t* rgb;
  int width;
  int height;
};

Sprite sprites[SPRITE_COUNT];

// ─── STATE MACHINE ────────────────────────────────────────────────────────────
enum Mode {
  FACE_NORMAL,
  FACE_REACTION,
  TEXT_REACTION,
  FACE_HOLD,
};

Mode currentMode  = FACE_NORMAL;
int currentSprite = SPR_NORMAL;

enum Reaction {
  REACTION_NONE,
  REACTION_HAPPY,
  REACTION_MAD
};

Reaction activeReaction = REACTION_NONE;

// ─── TIMING ───────────────────────────────────────────────────────────────────
unsigned long lastBlinkTime  = 0;
unsigned long blinkStartTime = 0;
unsigned long stateStartTime = 0;
unsigned long lastFloatDraw  = 0;
int lastFloatOffset          = 0;

bool isBlinking = false;

const unsigned long BLINK_INTERVAL         = 3000;
const unsigned long BLINK_LENGTH           = 150;
const unsigned long REACTION_FACE_DURATION = 2000;
const unsigned long REACTION_TEXT_DURATION = 4000;
const unsigned long REACTION_HOLD_DURATION = 10000;
const unsigned long FLOAT_INTERVAL         = 30;
const float FLOAT_AMPLITUDE                = 20.0;
const float FLOAT_SPEED                    = 0.00065;

// ─── TAP DETECTION ────────────────────────────────────────────────────────────
int tapCount                 = 0;
unsigned long tapWindowStart = 0;
unsigned long lastTapTime    = 0;

const unsigned long TAP_WINDOW = 800;
const unsigned long TAP_GAP    = 100; // min ms between taps

// ─── SCREEN ───────────────────────────────────────────────────────────────────
const int SCREEN_CX = 233;
const int SCREEN_CY = 233;

// ─── TOUCH INTERRUPT ──────────────────────────────────────────────────────────
void IRAM_ATTR onTouchInterrupt() {
  touchInterrupt = true;
}

// ─── BMP LOADER ───────────────────────────────────────────────────────────────
bool loadBMP(const char* filename, Sprite& sprite) {
  File file = SD_MMC.open(filename);
  if (!file) {
    Serial.print("Failed to open: ");
    Serial.println(filename);
    return false;
  }

  if (file.read() != 'B' || file.read() != 'M') {
    Serial.println("Not a BMP file");
    file.close();
    return false;
  }

  file.seek(10);
  uint32_t pixelOffset = 0;
  file.read((uint8_t*)&pixelOffset, 4);

  file.seek(18);
  int32_t imgWidth = 0, imgHeight = 0;
  file.read((uint8_t*)&imgWidth, 4);
  file.read((uint8_t*)&imgHeight, 4);
  bool flipped = imgHeight > 0;
  if (imgHeight < 0) imgHeight = -imgHeight;

  sprite.width  = imgWidth;
  sprite.height = imgHeight;

  sprite.rgb = (uint16_t*)ps_malloc(imgWidth * imgHeight * 2);
  if (!sprite.rgb) {
    Serial.print("PSRAM allocation failed for: ");
    Serial.println(filename);
    file.close();
    return false;
  }

  uint32_t rowSize = ((imgWidth * 3 + 3) / 4) * 4;
  uint8_t rowBuffer[rowSize];

  file.seek(pixelOffset);
  for (int row = 0; row < imgHeight; row++) {
    file.read(rowBuffer, rowSize);
    int destRow = flipped ? (imgHeight - 1 - row) : row;
    for (int col = 0; col < imgWidth; col++) {
      uint8_t b = rowBuffer[col * 3];
      uint8_t g = rowBuffer[col * 3 + 1];
      uint8_t r = rowBuffer[col * 3 + 2];
      sprite.rgb[destRow * imgWidth + col] =
        ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }
  }

  file.close();
  Serial.print("Loaded: ");
  Serial.println(filename);
  return true;
}

// ─── DRAW SPRITE ──────────────────────────────────────────────────────────────
#define ROWS_PER_CHUNK 8

void drawSprite(int index, int floatOffset, int prevOffset) {
  if (!sprites[index].rgb) return;
  Sprite& s = sprites[index];

  if (floatOffset != prevOffset) {
    if (floatOffset > prevOffset) {
      gfx->fillRect(0, prevOffset, s.width, floatOffset - prevOffset, RGB565_BLACK);
    } else {
      gfx->fillRect(0, s.height + floatOffset, s.width, prevOffset - floatOffset, RGB565_BLACK);
    }
  }

  for (int startRow = 0; startRow < s.height; startRow += ROWS_PER_CHUNK) {
    int endRow      = min(startRow + ROWS_PER_CHUNK, s.height);
    int chunkHeight = endRow - startRow;
    gfx->draw16bitRGBBitmap(
      0,
      startRow + floatOffset,
      &s.rgb[startRow * s.width],
      s.width,
      chunkHeight
    );
  }
}

// ─── TEXT SCREEN ──────────────────────────────────────────────────────────────
void drawTextScreen(const char* message) {
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(4, 4, 1);
  int msgLen = strlen(message);
  int textX  = SCREEN_CX - (msgLen * 12);
  int textY  = SCREEN_CY - 20;
  gfx->setCursor(textX, textY);
  gfx->println(message);
}

// ─── REACTION TRIGGER ─────────────────────────────────────────────────────────
void triggerReaction(Reaction r) {
  activeReaction = r;
  currentMode    = FACE_REACTION;
  stateStartTime = millis();
  isBlinking     = false;

  if (r == REACTION_HAPPY) {
    currentSprite = SPR_HAPPY;
    Serial.println("Reaction: HAPPY");
  } else if (r == REACTION_MAD) {
    currentSprite = SPR_MAD;
    Serial.println("Reaction: MAD");
  }

  gfx->fillScreen(RGB565_BLACK);
  drawSprite(currentSprite, 0, 0);
  lastFloatOffset = 0;
}

// ─── RETURN TO NORMAL ─────────────────────────────────────────────────────────
void returnToNormal() {
  activeReaction  = REACTION_NONE;
  currentMode     = FACE_NORMAL;
  currentSprite   = SPR_NORMAL;
  isBlinking      = false;
  lastBlinkTime   = millis();
  lastFloatDraw   = millis();
  lastFloatOffset = 0;
  gfx->fillScreen(RGB565_BLACK);
  drawSprite(SPR_NORMAL, 0, 0);
  Serial.println("Returned to normal");
}

// ─── FLOAT OFFSET ─────────────────────────────────────────────────────────────
int getFloatOffset() {
  return (int)(sin(millis() * FLOAT_SPEED) * FLOAT_AMPLITUDE);
}

// ─── PRELOAD ALL SPRITES ──────────────────────────────────────────────────────
void preloadSprites() {
  Serial.println("Preloading sprites...");
  unsigned long start = millis();
  for (int i = 0; i < SPRITE_COUNT; i++) {
    loadBMP(spriteFiles[i], sprites[i]);
  }
  Serial.print("All sprites loaded in ");
  Serial.print(millis() - start);
  Serial.println("ms");
}

// ─── SETUP ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Wire.begin(IIC_SDA, IIC_SCL);

  // Touch init using SensorLib driver
  touch.setPins(TP_RESET, TP_INT);
  if (!touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL)) {
    Serial.println("Touch init failed!");
  } else {
    Serial.println("Touch initialized.");
    Serial.print("Touch model: ");
    Serial.println(touch.getModelName());
  }
  attachInterrupt(TP_INT, onTouchInterrupt, FALLING);

  // SD card init
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("SD card mount failed!");
  } else {
    Serial.println("SD card mounted.");
  }

  // Display init
  if (!gfx->begin()) {
    Serial.println("Display init failed!");
  }
  gfx->setBrightness(150);
  gfx->fillScreen(RGB565_BLACK);

  preloadSprites();

  lastBlinkTime = millis();
  lastFloatDraw = millis();

  drawSprite(SPR_NORMAL, 0, 0);
}

// ─── MAIN LOOP ────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── TAP DETECTION via SensorLib ──
  if (currentMode == FACE_NORMAL && touchInterrupt) {
    touchInterrupt = false;
    uint8_t touched = touch.getPoint(touchX, touchY, touch.getSupportTouchPoint());
    if (touched > 0) {
      // Only count if enough time has passed since last tap
      if (now - lastTapTime > TAP_GAP) {
        tapCount++;
        lastTapTime    = now;
        tapWindowStart = now;
        Serial.print("Tap! count:");
        Serial.println(tapCount);
      }
    }
  }

  // Evaluate tap count after window expires
  if (tapCount > 0 && (now - tapWindowStart > TAP_WINDOW)) {
    if (tapCount == 2) {
      triggerReaction(REACTION_HAPPY);
    } else if (tapCount == 3) {
      triggerReaction(REACTION_MAD);
    }
    tapCount = 0;
  }

  // ── REACTION SEQUENCE ──
  if (currentMode == FACE_REACTION) {
    if (now - stateStartTime >= REACTION_FACE_DURATION) {
      currentMode    = TEXT_REACTION;
      stateStartTime = now;
      if (activeReaction == REACTION_HAPPY) {
        drawTextScreen("hi!");
      } else if (activeReaction == REACTION_MAD) {
        drawTextScreen("hisssss.");
      }
    }
  }

  if (currentMode == TEXT_REACTION) {
    if (now - stateStartTime >= REACTION_TEXT_DURATION) {
      currentMode    = FACE_HOLD;
      stateStartTime = now;
      gfx->fillScreen(RGB565_BLACK);
      drawSprite(currentSprite, 0, 0);
      lastFloatOffset = 0;
      lastFloatDraw   = now;
    }
  }

  if (currentMode == FACE_HOLD) {
    if (now - stateStartTime >= REACTION_HOLD_DURATION) {
      returnToNormal();
      return;
    }
    if (now - lastFloatDraw >= FLOAT_INTERVAL) {
      int newOffset   = getFloatOffset();
      drawSprite(currentSprite, newOffset, lastFloatOffset);
      lastFloatOffset = newOffset;
      lastFloatDraw   = now;
    }
  }

  // ── NORMAL IDLE ──
  if (currentMode == FACE_NORMAL) {
    if (!isBlinking && (now - lastBlinkTime >= BLINK_INTERVAL)) {
      isBlinking     = true;
      blinkStartTime = now;
      drawSprite(SPR_BLINK, lastFloatOffset, lastFloatOffset);
    }

    if (isBlinking && (now - blinkStartTime >= BLINK_LENGTH)) {
      isBlinking    = false;
      lastBlinkTime = now;
    }

    if (!isBlinking && (now - lastFloatDraw >= FLOAT_INTERVAL)) {
      int newOffset   = getFloatOffset();
      drawSprite(SPR_NORMAL, newOffset, lastFloatOffset);
      lastFloatOffset = newOffset;
      lastFloatDraw   = now;
    }
  }
}
