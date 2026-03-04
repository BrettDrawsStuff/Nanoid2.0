#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"
#include <Wire.h>
#include <FS.h>
#include <SD_MMC.h>
#include <math.h>

// ─── DISPLAY SETUP ────────────────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *gfx = new Arduino_CO5300(
  bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

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

// Each sprite stored as RGB565 in PSRAM
struct Sprite {
  uint16_t* rgb;
  int width;
  int height;
};

Sprite sprites[SPRITE_COUNT];

// ─── STATE MACHINE ────────────────────────────────────────────────────────────
enum Mode {
  FACE_MODE,
  TEXT_MODE
};

Mode currentMode  = FACE_MODE;
int currentSprite = SPR_NORMAL;

// ─── TIMING ───────────────────────────────────────────────────────────────────
unsigned long lastBlinkTime  = 0;
unsigned long blinkStartTime = 0;
unsigned long textModeStart  = 0;
unsigned long lastFloatDraw  = 0;
int lastFloatOffset          = 0;

bool isBlinking = false;

const unsigned long BLINK_INTERVAL     = 3000;
const unsigned long BLINK_LENGTH       = 150;
const unsigned long TEXT_MODE_DURATION = 4000;
const unsigned long FLOAT_INTERVAL     = 30; // 20fps to start, tune later

// ─── FLOAT ANIMATION ──────────────────────────────────────────────────────────
const float FLOAT_AMPLITUDE = 20.0;
const float FLOAT_SPEED     = 0.00065;

int getFloatOffset() {
  return (int)(sin(millis() * FLOAT_SPEED) * FLOAT_AMPLITUDE);
}

// ─── SCREEN ───────────────────────────────────────────────────────────────────
const int SCREEN_CX = 233;
const int SCREEN_CY = 233;

// ─── TOUCH ────────────────────────────────────────────────────────────────────
bool isTouched() {
  return digitalRead(TP_INT) == LOW;
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
// Draws the sprite at floatOffset
// When float position changes, fills the exposed strip with black
// Draws in chunks of ROWS_PER_CHUNK for better display throughput
#define ROWS_PER_CHUNK 8

void drawSprite(int index, int floatOffset, int prevOffset) {
  if (!sprites[index].rgb) return;
  Sprite& s = sprites[index];

  // Erase strip exposed by movement
  if (floatOffset != prevOffset) {
    if (floatOffset > prevOffset) {
      gfx->fillRect(0, prevOffset, s.width, floatOffset - prevOffset, RGB565_BLACK);
    } else {
      gfx->fillRect(0, s.height + floatOffset, s.width, prevOffset - floatOffset, RGB565_BLACK);
    }
  }

  // Draw in chunks of ROWS_PER_CHUNK rows at a time
  for (int startRow = 0; startRow < s.height; startRow += ROWS_PER_CHUNK) {
    int endRow = min(startRow + ROWS_PER_CHUNK, s.height);
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

// ─── TEXT MODE ────────────────────────────────────────────────────────────────
void drawTextMode(const char* message) {
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(4, 4, 1);
  int msgLen = strlen(message);
  int textX  = SCREEN_CX - (msgLen * 12);
  int textY  = SCREEN_CY - 20;
  gfx->setCursor(textX, textY);
  gfx->println(message);
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

  // Touch init
  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, HIGH);
  pinMode(TP_INT, INPUT_PULLUP);

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

  // ── TEXT MODE TIMEOUT ──
  if (currentMode == TEXT_MODE && (now - textModeStart >= TEXT_MODE_DURATION)) {
    currentMode = FACE_MODE;
    gfx->fillScreen(RGB565_BLACK);
    lastBlinkTime = now;
    return;
  }

  // ── FACE MODE ──
  if (currentMode == FACE_MODE) {

    // ── BLINK ──
    if (!isBlinking && (now - lastBlinkTime >= BLINK_INTERVAL)) {
      isBlinking     = true;
      blinkStartTime = now;
      drawSprite(SPR_BLINK, lastFloatOffset, lastFloatOffset);
    }

    if (isBlinking && (now - blinkStartTime >= BLINK_LENGTH)) {
      isBlinking    = false;
      lastBlinkTime = now;
    }

    // ── FLOAT ANIMATION ──
    if (!isBlinking && (now - lastFloatDraw >= FLOAT_INTERVAL)) {
      int newOffset   = getFloatOffset();
      drawSprite(currentSprite, newOffset, lastFloatOffset);
      lastFloatOffset = newOffset;
      lastFloatDraw   = now;
    }
  }
}
