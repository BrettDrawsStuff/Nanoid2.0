#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"
#include <Wire.h>
#include <FS.h>
#include <SD_MMC.h>

// ─── DISPLAY SETUP ────────────────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *gfx = new Arduino_CO5300(
  bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

// ─── STATE MACHINE ────────────────────────────────────────────────────────────
enum Mode {
  FACE_MODE,
  TEXT_MODE
};

Mode currentMode = FACE_MODE;

// ─── TIMING ───────────────────────────────────────────────────────────────────
unsigned long lastBlinkTime  = 0;
unsigned long blinkDuration  = 0;
unsigned long textModeStart  = 0;
bool isBlinking              = false;
bool needsRedraw             = true;  // triggers first draw on boot

const unsigned long BLINK_INTERVAL     = 3000;
const unsigned long BLINK_LENGTH       = 150;
const unsigned long TEXT_MODE_DURATION = 4000;

// ─── SCREEN CENTER ────────────────────────────────────────────────────────────
const int SCREEN_CX = 233;
const int SCREEN_CY = 233;

// ─── TOUCH ────────────────────────────────────────────────────────────────────
bool isTouched() {
  return digitalRead(TP_INT) == LOW;
}

// ─── BMP LOADER ───────────────────────────────────────────────────────────────
void drawBMP(const char *filename, int16_t x, int16_t y) {
  File file = SD_MMC.open(filename);
  if (!file) {
    Serial.print("Failed to open: ");
    Serial.println(filename);
    return;
  }

  if (file.read() != 'B' || file.read() != 'M') {
    Serial.println("Not a BMP file");
    file.close();
    return;
  }

  file.seek(10);
  uint32_t pixelOffset = 0;
  file.read((uint8_t*)&pixelOffset, 4);

  file.seek(18);
  int32_t imgWidth = 0, imgHeight = 0;
  file.read((uint8_t*)&imgWidth, 4);
  file.read((uint8_t*)&imgHeight, 4);
  if (imgHeight < 0) imgHeight = -imgHeight;

  Serial.print("BMP: ");
  Serial.print(imgWidth);
  Serial.print("x");
  Serial.println(imgHeight);

  unsigned long startTime = millis();

  uint32_t rowSize = ((imgWidth * 3 + 3) / 4) * 4;

  // Allocate full image buffer in PSRAM
  uint16_t *imgBuffer = (uint16_t*)ps_malloc(imgWidth * imgHeight * 2);
  if (!imgBuffer) {
    Serial.println("Failed to allocate image buffer!");
    file.close();
    return;
  }

  uint8_t rowBuffer[rowSize];

  // Read sequentially (BMP is stored bottom-up)
  file.seek(pixelOffset);
  for (int row = 0; row < imgHeight; row++) {
    file.read(rowBuffer, rowSize);
    int destRow = imgHeight - 1 - row;
    for (int col = 0; col < imgWidth; col++) {
      uint8_t b = rowBuffer[col * 3];
      uint8_t g = rowBuffer[col * 3 + 1];
      uint8_t r = rowBuffer[col * 3 + 2];
      imgBuffer[destRow * imgWidth + col] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }
  }
  file.close();

  // Draw entire image in one call
  gfx->draw16bitRGBBitmap(x, y, imgBuffer, imgWidth, imgHeight);
  free(imgBuffer);

  unsigned long endTime = millis();
  Serial.print("BMP drawn in ");
  Serial.print(endTime - startTime);
  Serial.println("ms");
}

// ─── DRAW FUNCTIONS ───────────────────────────────────────────────────────────
void drawFace() {
  gfx->fillScreen(RGB565_BLACK);
  drawBMP("/normal.bmp", 0, 0);
}

void drawTextMode() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(4, 4, 1);
  gfx->setCursor(SCREEN_CX - 72, SCREEN_CY - 20);
  gfx->println("HELLO.");
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

  // needsRedraw = true will trigger first draw in loop
}

// ─── MAIN LOOP ────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── TOUCH INPUT ──
  if (currentMode == FACE_MODE && isTouched()) {
    currentMode = TEXT_MODE;
    textModeStart = now;
    drawTextMode();
    return;
  }

  // ── TEXT MODE TIMEOUT ──
  if (currentMode == TEXT_MODE && (now - textModeStart >= TEXT_MODE_DURATION)) {
    currentMode = FACE_MODE;
    needsRedraw = true;
    lastBlinkTime = now;
    return;
  }

  // ── INITIAL / POST-TEXT REDRAW ──
  if (currentMode == FACE_MODE && needsRedraw) {
    drawFace();
    needsRedraw = false;
    lastBlinkTime = now;
    return;
  }

  // ── BLINK LOGIC ──
  if (currentMode == FACE_MODE && !needsRedraw) {
    if (!isBlinking && (now - lastBlinkTime >= BLINK_INTERVAL)) {
      isBlinking = true;
      blinkDuration = now;
    }

    if (isBlinking && (now - blinkDuration >= BLINK_LENGTH)) {
      isBlinking = false;
      lastBlinkTime = now;
    }
  }
}
