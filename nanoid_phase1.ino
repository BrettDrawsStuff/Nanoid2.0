#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"
#include <Wire.h>
#include <FS.h>
#include <SD_MMC.h>
#include <math.h>
#include "TouchDrvCSTXXX.hpp"
#include "SensorQMI8658.hpp"

// ─── DISPLAY SETUP ────────────────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *gfx = new Arduino_CO5300(
  bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

// ─── TOUCH SETUP ──────────────────────────────────────────────────────────────
TouchDrvCST92xx touch;
int16_t touchX[5], touchY[5];
volatile bool touchInterrupt = false;

// ─── IMU SETUP ────────────────────────────────────────────────────────────────
SensorQMI8658 qmi;
IMUdata acc;

const float SHAKE_THRESHOLD        = 1.5;
const unsigned long SHAKE_COOLDOWN = 8000;
unsigned long lastShakeTime        = 0;

// ─── BACKGROUND COLOR ─────────────────────────────────────────────────────────
#define BG_COLOR 0x31A8

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
  FACE_SAD,
  FACE_SLEEP,
  TEXT_SLEEP,
  FACE_GLITCH,
  FACE_JUMP,
  FACE_WALK,
  FACE_WAVE,
};

Mode currentMode  = FACE_NORMAL;
int currentSprite = SPR_NORMAL;

enum Reaction {
  REACTION_NONE,
  REACTION_HAPPY,
  REACTION_MAD,
  REACTION_DISGUST,
  REACTION_SCARED
};

Reaction activeReaction = REACTION_NONE;

// ─── TIMING ───────────────────────────────────────────────────────────────────
unsigned long lastBlinkTime    = 0;
unsigned long blinkStartTime   = 0;
unsigned long stateStartTime   = 0;
unsigned long lastFloatDraw    = 0;
unsigned long lastActivityTime = 0;
unsigned long lastZzzTime      = 0;
unsigned long glitchStartTime  = 0;
unsigned long lastGlitchFrame  = 0;
int glitchType                 = 0;

int terminalCharIndex          = 0;
unsigned long lastTerminalChar = 0;
bool terminalDone              = false;
unsigned long terminalDoneTime = 0;
bool terminalGlitching         = false;
const unsigned long TERMINAL_CHAR_SPEED = 18;
unsigned long nextDisgustTime  = 0;
unsigned long nextGlitchTime   = 0;
int lastFloatOffset            = 0;

bool isBlinking  = false;
bool inSleepMode = false;

const unsigned long BLINK_INTERVAL = 3000;
const unsigned long BLINK_LENGTH   = 150;

const unsigned long REACTION_FACE_DURATION = 2000;
const unsigned long REACTION_TEXT_DURATION = 4000;
const unsigned long REACTION_HOLD_DURATION = 10000;

const unsigned long SCARED_FACE_1_DURATION = 2000;
const unsigned long SCARED_TEXT_DURATION   = 4000;
const unsigned long SCARED_FACE_2_DURATION = 4000;
const unsigned long SCARED_SAD_DURATION    = 3000;

enum ScaredPhase {
  SCARED_PHASE_FACE1,
  SCARED_PHASE_TEXT,
  SCARED_PHASE_FACE2,
  SCARED_PHASE_SAD
};
ScaredPhase scaredPhase = SCARED_PHASE_FACE1;

const unsigned long SAD_TIMEOUT   = 5UL  * 60 * 1000;
const unsigned long SLEEP_TIMEOUT = 15UL * 60 * 1000;

const unsigned long ZZZ_INTERVAL    = 60000;
const unsigned long ZZZ_DURATION    = 4000;
const uint8_t       SLEEP_BRIGHTNESS = 75;
const uint8_t       AWAKE_BRIGHTNESS = 150;

const unsigned long GLITCH_DURATION      = 2000;
const unsigned long GLITCH_FRAME_RATE    = 16;
const unsigned long TERMINAL_HOLD        = 2000;
const unsigned long TERMINAL_GLITCH_TIME = 1000;

// Float animation
const unsigned long FLOAT_INTERVAL = 19;
const float FLOAT_AMPLITUDE        = 7.0;
const float FLOAT_SPEED            = 0.0006;

// Jump animation
#define JUMP_FRAME_COUNT   21
#define JUMP_FRAME_MS      42   // 42ms ≈ 24fps
const unsigned long HOLD_MIN = 2000;
const unsigned long HOLD_MAX = 5000;

int jumpFrameIndex          = 0;
unsigned long lastJumpFrame = 0;
unsigned long touchDownTime = 0;
bool touchHeld              = false;

// Walk animation
#define WALK_FRAME_COUNT  8
#define WALK_FRAME_MS     42   // 24fps
int walkFrameIndex          = 0;
unsigned long lastWalkFrame = 0;

// Wave animation (random idle, every 10-45 min)
const int waveSequence[]        = {1,2,3,4,3,2,3,4,3,2,3,4,3,2,1};
#define WAVE_SEQUENCE_LENGTH    15
#define WAVE_FRAME_MS           42   // 24fps
int waveFrameIndex              = 0;
unsigned long lastWaveFrame     = 0;
unsigned long nextWaveTime      = 0;

// PWR button (GPIO 0)
#define PWR_BUTTON_PIN 0
bool pwrButtonPrev          = HIGH;
unsigned long pwrDebounce   = 0;
const unsigned long PWR_DEBOUNCE_MS = 50;
// One frame at a time — decode into here, blast to display, reuse
uint16_t* frameBuffer = nullptr;

// ─── TAP DETECTION ────────────────────────────────────────────────────────────
int tapCount                 = 0;
unsigned long tapWindowStart = 0;
unsigned long lastTapTime    = 0;

const unsigned long TAP_WINDOW = 800;
const unsigned long TAP_GAP    = 100;

// ─── SCREEN ───────────────────────────────────────────────────────────────────
const int SCREEN_W  = 466;
const int SCREEN_H  = 466;
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
    Serial.print("Failed to open: "); Serial.println(filename);
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
    Serial.print("PSRAM alloc failed: "); Serial.println(filename);
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
  Serial.print("Loaded: "); Serial.println(filename);
  return true;
}

// ─── DRAW SPRITE ──────────────────────────────────────────────────────────────
#define ROWS_PER_CHUNK 8

void drawSprite(int index, int floatOffset, int prevOffset) {
  if (!sprites[index].rgb) return;
  Sprite& s = sprites[index];
  if (floatOffset != prevOffset) {
    if (floatOffset > prevOffset) {
      gfx->fillRect(0, prevOffset, s.width, floatOffset - prevOffset, BG_COLOR);
    } else {
      gfx->fillRect(0, s.height + floatOffset, s.width, prevOffset - floatOffset, BG_COLOR);
    }
  }
  for (int startRow = 0; startRow < s.height; startRow += ROWS_PER_CHUNK) {
    int endRow      = min(startRow + ROWS_PER_CHUNK, s.height);
    int chunkHeight = endRow - startRow;
    gfx->draw16bitRGBBitmap(
      0, startRow + floatOffset,
      &s.rgb[startRow * s.width],
      s.width, chunkHeight
    );
  }
}

// ─── DRAW JUMP FRAME (streamed from SD into PSRAM buffer) ────────────────────
void drawJumpFrame(int frameIndex) {
  if (!frameBuffer) return;

  char filename[16];
  snprintf(filename, sizeof(filename), "/jump%d.bmp", frameIndex + 1);

  File file = SD_MMC.open(filename);
  if (!file) {
    Serial.print("Jump frame missing: "); Serial.println(filename);
    return;
  }
  if (file.read() != 'B' || file.read() != 'M') { file.close(); return; }
  file.seek(10);
  uint32_t pixelOffset = 0;
  file.read((uint8_t*)&pixelOffset, 4);
  file.seek(18);
  int32_t imgWidth = 0, imgHeight = 0;
  file.read((uint8_t*)&imgWidth, 4);
  file.read((uint8_t*)&imgHeight, 4);
  bool flipped = imgHeight > 0;
  if (imgHeight < 0) imgHeight = -imgHeight;

  uint32_t rowSize = ((imgWidth * 3 + 3) / 4) * 4;
  uint8_t rowBuf[rowSize];
  file.seek(pixelOffset);

  unsigned long t = millis();

  for (int row = 0; row < imgHeight; row++) {
    file.read(rowBuf, rowSize);
    int destRow = flipped ? (imgHeight - 1 - row) : row;
    uint16_t* dst = frameBuffer + destRow * imgWidth;
    for (int col = 0; col < imgWidth; col++) {
      uint8_t b = rowBuf[col * 3];
      uint8_t g = rowBuf[col * 3 + 1];
      uint8_t r = rowBuf[col * 3 + 2];
      dst[col] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }
  }
  file.close();

  // Center horizontally, sit toward bottom to match character in normal.bmp
  int drawX = (SCREEN_W - imgWidth) / 2;
  int drawY = SCREEN_H - imgHeight - 20;

  gfx->draw16bitRGBBitmap(drawX, drawY, frameBuffer, imgWidth, imgHeight);
  Serial.print("Frame "); Serial.print(frameIndex + 1);
  Serial.print(" "); Serial.print(millis() - t); Serial.println("ms");
}

// ─── DRAW WALK FRAME (streamed from SD into PSRAM buffer) ────────────────────
void drawWalkFrame(int frameIndex) {
  if (!frameBuffer) return;

  char filename[16];
  snprintf(filename, sizeof(filename), "/walk%d.bmp", frameIndex + 1);

  File file = SD_MMC.open(filename);
  if (!file) {
    Serial.print("Walk frame missing: "); Serial.println(filename);
    return;
  }
  if (file.read() != 'B' || file.read() != 'M') { file.close(); return; }
  file.seek(10);
  uint32_t pixelOffset = 0;
  file.read((uint8_t*)&pixelOffset, 4);
  file.seek(18);
  int32_t imgWidth = 0, imgHeight = 0;
  file.read((uint8_t*)&imgWidth, 4);
  file.read((uint8_t*)&imgHeight, 4);
  bool flipped = imgHeight > 0;
  if (imgHeight < 0) imgHeight = -imgHeight;

  uint32_t rowSize = ((imgWidth * 3 + 3) / 4) * 4;
  uint8_t rowBuf[rowSize];
  file.seek(pixelOffset);

  for (int row = 0; row < imgHeight; row++) {
    file.read(rowBuf, rowSize);
    int destRow = flipped ? (imgHeight - 1 - row) : row;
    uint16_t* dst = frameBuffer + destRow * imgWidth;
    for (int col = 0; col < imgWidth; col++) {
      uint8_t b = rowBuf[col * 3];
      uint8_t g = rowBuf[col * 3 + 1];
      uint8_t r = rowBuf[col * 3 + 2];
      dst[col] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }
  }
  file.close();

  int drawX = (SCREEN_W - imgWidth) / 2;
  int drawY = SCREEN_H - imgHeight - 20;

  gfx->draw16bitRGBBitmap(drawX, drawY, frameBuffer, imgWidth, imgHeight);
}

// ─── DRAW WAVE FRAME (streamed from SD into PSRAM buffer) ────────────────────
void drawWaveFrame(int fileIndex) {
  if (!frameBuffer) return;

  char filename[16];
  snprintf(filename, sizeof(filename), "/wave%d.bmp", fileIndex);

  File file = SD_MMC.open(filename);
  if (!file) {
    Serial.print("Wave frame missing: "); Serial.println(filename);
    return;
  }
  if (file.read() != 'B' || file.read() != 'M') { file.close(); return; }
  file.seek(10);
  uint32_t pixelOffset = 0;
  file.read((uint8_t*)&pixelOffset, 4);
  file.seek(18);
  int32_t imgWidth = 0, imgHeight = 0;
  file.read((uint8_t*)&imgWidth, 4);
  file.read((uint8_t*)&imgHeight, 4);
  bool flipped = imgHeight > 0;
  if (imgHeight < 0) imgHeight = -imgHeight;

  uint32_t rowSize = ((imgWidth * 3 + 3) / 4) * 4;
  uint8_t rowBuf[rowSize];
  file.seek(pixelOffset);

  for (int row = 0; row < imgHeight; row++) {
    file.read(rowBuf, rowSize);
    int destRow = flipped ? (imgHeight - 1 - row) : row;
    uint16_t* dst = frameBuffer + destRow * imgWidth;
    for (int col = 0; col < imgWidth; col++) {
      uint8_t b = rowBuf[col * 3];
      uint8_t g = rowBuf[col * 3 + 1];
      uint8_t r = rowBuf[col * 3 + 2];
      dst[col] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }
  }
  file.close();

  int drawX = (SCREEN_W - imgWidth) / 2;
  int drawY = SCREEN_H - imgHeight - 20;
  gfx->draw16bitRGBBitmap(drawX, drawY, frameBuffer, imgWidth, imgHeight);
}

// ─── TEXT SCREENS ─────────────────────────────────────────────────────────────
void drawTextScreen(const char* message, int x, int y) {
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(4, 4, 1);
  gfx->setCursor(x, y);
  gfx->println(message);
}

void drawTextCentered(const char* message) {
  int msgLen = strlen(message);
  int x = SCREEN_CX - (msgLen * 12);
  int y = SCREEN_CY - 20;
  drawTextScreen(message, x, y);
}

void drawUghText() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(4, 4, 1);
  gfx->setCursor(280, 340);
  gfx->println("ugh.");
}

void drawAhhhhText() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(4, 4, 1);
  int x = SCREEN_CX - (6 * 12);
  int y = SCREEN_CY - 20;
  gfx->setCursor(x, y);
  gfx->println("AHHHH!");
}

// ─── GLITCH FRAME ─────────────────────────────────────────────────────────────
void drawGlitchFrame() {
  if (glitchType == 0) {
    uint16_t barColors[] = {
      0xF800, 0xFC00, 0xFFE0, 0x07E0, 0x07FF,
      0x001F, 0xF81F, 0xFFFF, 0xF8F0, 0xBBF7,
      0xFD20, 0x04FF, 0xAFE5, 0xFF80, 0x801F
    };
    int numColors = 15;
    int y = 0;
    while (y < SCREEN_H) {
      int h     = random(4, 28);
      int shift = random(-50, 50);
      uint16_t c = barColors[random(numColors)];
      if (shift >= 0) {
        gfx->fillRect(shift, y, SCREEN_W - shift, h, c);
        gfx->fillRect(0, y, shift, h, barColors[random(numColors)]);
      } else {
        gfx->fillRect(0, y, SCREEN_W + shift, h, c);
        gfx->fillRect(SCREEN_W + shift, y, -shift, h, barColors[random(numColors)]);
      }
      y += h + random(0, 4);
    }
    for (int i = 0; i < 20; i++)
      gfx->fillRect(0, random(SCREEN_H), SCREEN_W, random(1, 3), random(0, 65535));
    for (int i = 0; i < 5; i++)
      gfx->fillRect(random(SCREEN_W), 0, random(2, 8), SCREEN_H, random(0, 65535));
    for (int i = 0; i < 200; i++)
      gfx->drawPixel(random(SCREEN_W), random(SCREEN_H), random(0, 65535));
  } else {
    if (terminalCharIndex == 0) gfx->fillScreen(0x0000);
    const char chars[] = "0123456789ABCDEF><@#!%^&*?/\\|~=+-_[]{}();:.,LOAD EXEC RUN ERROR MEMORY OVERFLOW ACCESS";
    int charsLen = strlen(chars);
    int charW = 12, charH = 16;
    int cols  = SCREEN_W / charW;
    int rows  = SCREEN_H / charH;
    int total = cols * rows;
    int charsPerFrame = max(2, total / 150);
    for (int i = 0; i < charsPerFrame && terminalCharIndex < total; i++) {
      int row = terminalCharIndex / cols;
      int col = terminalCharIndex % cols;
      char c  = chars[random(charsLen)];
      uint16_t color;
      int r = random(10);
      if (r == 0)      color = 0xFFFF;
      else if (r <= 2) color = 0x0320;
      else             color = 0x07E0;
      gfx->setTextColor(color);
      gfx->setTextSize(2, 2, 0);
      gfx->setCursor(col * charW, row * charH);
      gfx->print(c);
      if (random(8) == 0)
        gfx->fillRect(col * charW + random(-10, 10), row * charH, random(2, 8), 1, random(0, 65535));
      terminalCharIndex++;
    }
  }
}

// ─── RANDOM EVENT SCHEDULING ──────────────────────────────────────────────────
void scheduleNextWave() {
  // Fire wave randomly between 10 and 45 minutes from now
  nextWaveTime = millis() + (10UL * 60 * 1000) + random(35UL * 60 * 1000);
  Serial.print("Next wave in ");
  Serial.print((nextWaveTime - millis()) / 60000);
  Serial.println(" minutes");
}

void scheduleNextDisgust() {
  nextDisgustTime = millis() + (5UL * 60 * 1000) + random(25UL * 60 * 1000);
  Serial.print("Next disgust in ");
  Serial.print((nextDisgustTime - millis()) / 60000);
  Serial.println(" minutes");
}

void scheduleNextGlitch() {
  nextGlitchTime = millis() + (2UL * 60 * 1000) + random(13UL * 60 * 1000);
  Serial.print("Next glitch in ");
  Serial.print((nextGlitchTime - millis()) / 60000);
  Serial.println(" minutes");
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
  } else if (r == REACTION_DISGUST) {
    currentSprite = SPR_DISGUST;
    Serial.println("Reaction: DISGUST");
  } else if (r == REACTION_SCARED) {
    currentSprite = SPR_SCARED;
    scaredPhase   = SCARED_PHASE_FACE1;
    Serial.println("Reaction: SCARED");
  }
  drawSprite(currentSprite, 0, 0);
  lastFloatOffset = 0;
}

// ─── RETURN TO NORMAL ─────────────────────────────────────────────────────────
void returnToNormal() {
  activeReaction   = REACTION_NONE;
  currentMode      = FACE_NORMAL;
  currentSprite    = SPR_NORMAL;
  isBlinking       = false;
  inSleepMode      = false;
  lastBlinkTime    = millis();
  lastFloatDraw    = millis();
  lastActivityTime = millis();
  lastFloatOffset  = 0;
  gfx->setBrightness(AWAKE_BRIGHTNESS);
  drawSprite(SPR_NORMAL, 0, 0);
  Serial.println("Returned to normal");
}

// ─── ENTER SAD MODE ───────────────────────────────────────────────────────────
void enterSadMode() {
  currentMode     = FACE_SAD;
  currentSprite   = SPR_SAD;
  stateStartTime  = millis();
  isBlinking      = false;
  lastFloatOffset = 0;
  drawSprite(SPR_SAD, 0, 0);
  Serial.println("Entered sad mode");
}

// ─── ENTER SLEEP MODE ─────────────────────────────────────────────────────────
void enterSleepMode() {
  currentMode     = FACE_SLEEP;
  inSleepMode     = true;
  lastZzzTime     = millis();
  lastFloatOffset = 0;
  gfx->setBrightness(SLEEP_BRIGHTNESS);
  drawSprite(SPR_BLINK, 0, 0);
  Serial.println("Entered sleep mode");
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
  Serial.print("Free PSRAM after sprites: ");
  Serial.print(ESP.getFreePsram() / 1024);
  Serial.println("KB");
}

// ─── SETUP ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Wire.begin(IIC_SDA, IIC_SCL);
  pinMode(PWR_BUTTON_PIN, INPUT_PULLUP);

  touch.setPins(TP_RESET, TP_INT);
  if (!touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL)) {
    Serial.println("Touch init failed!");
  } else {
    Serial.println("Touch initialized.");
  }
  attachInterrupt(TP_INT, onTouchInterrupt, FALLING);

  if (!qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    Serial.println("IMU init failed!");
  } else {
    Serial.println("IMU initialized.");
    qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_125Hz, SensorQMI8658::LPF_MODE_0);
    qmi.enableAccelerometer();
  }

  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("SD card mount failed!");
  } else {
    Serial.println("SD card mounted.");
  }

  if (!gfx->begin()) {
    Serial.println("Display init failed!");
  }
  gfx->setBrightness(AWAKE_BRIGHTNESS);

  preloadSprites();

  unsigned long now = millis();
  lastBlinkTime     = now;
  lastFloatDraw     = now;
  lastActivityTime  = now;

  scheduleNextDisgust();
  scheduleNextGlitch();
  scheduleNextWave();

  // Allocate reusable animation frame buffer in PSRAM
  frameBuffer = (uint16_t*)ps_malloc(466 * 466 * 2);
  if (!frameBuffer) {
    Serial.println("Frame buffer alloc failed!");
  } else {
    Serial.println("Frame buffer ready.");
  }

  drawSprite(SPR_NORMAL, 0, 0);
}

// ─── MAIN LOOP ────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── PWR BUTTON (GPIO 0) — toggle walk mode ──
  bool pwrButton = digitalRead(PWR_BUTTON_PIN);
  if (pwrButton == LOW && pwrButtonPrev == HIGH && (now - pwrDebounce > PWR_DEBOUNCE_MS)) {
    pwrDebounce   = now;
    if (currentMode == FACE_NORMAL) {
      currentMode    = FACE_WALK;
      walkFrameIndex = 0;
      lastWalkFrame  = now;
      Serial.println("Walk mode ON");
    } else if (currentMode == FACE_WALK) {
      returnToNormal();
      Serial.println("Walk mode OFF");
    }
  }
  pwrButtonPrev = pwrButton;

  if (touchInterrupt) {
    touchInterrupt = false;
    uint8_t touched = touch.getPoint(touchX, touchY, touch.getSupportTouchPoint());
    if (touched > 0) {
      lastActivityTime = now;

      // Wake from sleep or sad
      if (currentMode == FACE_SLEEP || currentMode == TEXT_SLEEP || currentMode == FACE_SAD) {
        returnToNormal();
        tapCount  = 0;
        touchHeld = false;
        return;
      }

      if (currentMode == FACE_NORMAL) {
        if (!touchHeld) {
          touchHeld     = true;
          touchDownTime = now;
        }
        if (now - lastTapTime > TAP_GAP) {
          tapCount++;
          lastTapTime    = now;
          tapWindowStart = now;
        }
      }
    } else {
      // Finger lifted — check for jump range
      if (touchHeld && currentMode == FACE_NORMAL) {
        Serial.print("Released. tapCount: "); Serial.println(tapCount);
        if (tapCount >= 10 && tapCount <= 60) {
          currentMode    = FACE_JUMP;
          jumpFrameIndex = 0;
          lastJumpFrame  = now;
          tapCount       = 0;
          touchHeld      = false;
          Serial.println("Jump triggered!");
          return;
        }
      }
      touchHeld = false;
    }
  }

  if (tapCount > 0 && currentMode == FACE_NORMAL && (now - tapWindowStart > TAP_WINDOW)) {
    if (tapCount == 2) triggerReaction(REACTION_HAPPY);
    else if (tapCount == 3) triggerReaction(REACTION_MAD);
    tapCount = 0;
  }

  if (currentMode == FACE_NORMAL && (now - lastShakeTime > SHAKE_COOLDOWN)) {
    if (qmi.getAccelerometer(acc.x, acc.y, acc.z)) {
      float magnitude = sqrt(acc.x * acc.x + acc.y * acc.y + acc.z * acc.z);
      if (magnitude > SHAKE_THRESHOLD) {
        lastShakeTime    = now;
        lastActivityTime = now;
        triggerReaction(REACTION_SCARED);
        return;
      }
    }
  }

  if (currentMode == FACE_NORMAL || currentMode == FACE_SAD) {
    unsigned long inactive = now - lastActivityTime;
    if (inactive >= SLEEP_TIMEOUT && currentMode != FACE_SLEEP) {
      enterSleepMode(); return;
    } else if (inactive >= SAD_TIMEOUT && currentMode == FACE_NORMAL) {
      enterSadMode(); return;
    }
  }

  if (currentMode == FACE_NORMAL) {
    if (now >= nextDisgustTime) {
      scheduleNextDisgust();
      triggerReaction(REACTION_DISGUST);
      return;
    }
    if (now >= nextGlitchTime) {
      scheduleNextGlitch();
      glitchType        = random(2);
      terminalCharIndex = 0;
      terminalDone      = false;
      terminalGlitching = false;
      currentMode       = FACE_GLITCH;
      glitchStartTime   = now;
      lastGlitchFrame   = now;
      Serial.print("Glitch! type:");
      Serial.println(glitchType == 0 ? "rainbow" : "terminal");
      return;
    }
    if (now >= nextWaveTime) {
      scheduleNextWave();
      currentMode    = FACE_WAVE;
      waveFrameIndex = 0;
      lastWaveFrame  = now;
      Serial.println("Wave triggered!");
      return;
    }
  }

  if (currentMode == FACE_GLITCH) {
    if (glitchType == 0) {
      if (now - glitchStartTime >= GLITCH_DURATION) {
        drawSprite(SPR_NORMAL, lastFloatOffset, lastFloatOffset);
        currentMode = FACE_NORMAL;
        Serial.println("Glitch ended");
      } else if (now - lastGlitchFrame >= GLITCH_FRAME_RATE) {
        drawGlitchFrame();
        lastGlitchFrame = now;
      }
    } else {
      if (!terminalDone) {
        if (now - lastGlitchFrame >= GLITCH_FRAME_RATE) {
          drawGlitchFrame();
          lastGlitchFrame = now;
          int cols = SCREEN_W / 12;
          int rows = SCREEN_H / 16;
          if (terminalCharIndex >= cols * rows) {
            terminalDone     = true;
            terminalDoneTime = now;
            Serial.println("Terminal done, holding...");
          }
        }
      } else {
        if (now - terminalDoneTime >= TERMINAL_HOLD) {
          drawSprite(SPR_NORMAL, lastFloatOffset, lastFloatOffset);
          currentMode = FACE_NORMAL;
          Serial.println("Glitch ended");
        }
      }
    }
    return;
  }

  if (currentMode == FACE_JUMP) {
    if (now - lastJumpFrame >= JUMP_FRAME_MS) {
      drawJumpFrame(jumpFrameIndex);
      lastJumpFrame = now;
      jumpFrameIndex++;
      if (jumpFrameIndex >= JUMP_FRAME_COUNT) {
        returnToNormal();
        Serial.println("Jump complete");
      }
    }
    return;
  }

  if (currentMode == FACE_WALK) {
    if (now - lastWalkFrame >= WALK_FRAME_MS) {
      drawWalkFrame(walkFrameIndex);
      lastWalkFrame  = now;
      walkFrameIndex = (walkFrameIndex + 1) % WALK_FRAME_COUNT; // loop continuously
    }
    return;
  }

  if (currentMode == FACE_WAVE) {
    if (now - lastWaveFrame >= WAVE_FRAME_MS) {
      drawWaveFrame(waveSequence[waveFrameIndex]);
      lastWaveFrame = now;
      waveFrameIndex++;
      if (waveFrameIndex >= WAVE_SEQUENCE_LENGTH) {
        returnToNormal();
        Serial.println("Wave complete");
      }
    }
    return;
  }

  if (currentMode == FACE_SAD) return;

  if (currentMode == FACE_SLEEP) {
    if (now - lastZzzTime >= ZZZ_INTERVAL) {
      currentMode    = TEXT_SLEEP;
      stateStartTime = now;
      drawTextCentered("zzz...");
      Serial.println("zzz...");
    }
    return;
  }

  if (currentMode == TEXT_SLEEP) {
    if (now - stateStartTime >= ZZZ_DURATION) {
      currentMode = FACE_SLEEP;
      lastZzzTime = now;
      drawSprite(SPR_BLINK, 0, 0);
    }
    return;
  }

  if (currentMode == FACE_REACTION) {
    if (activeReaction == REACTION_SCARED) {
      if (scaredPhase == SCARED_PHASE_FACE1 && now - stateStartTime >= SCARED_FACE_1_DURATION) {
        scaredPhase    = SCARED_PHASE_TEXT;
        stateStartTime = now;
        drawAhhhhText();
      } else if (scaredPhase == SCARED_PHASE_TEXT && now - stateStartTime >= SCARED_TEXT_DURATION) {
        scaredPhase    = SCARED_PHASE_FACE2;
        stateStartTime = now;
        drawSprite(SPR_SCARED, 0, 0);
      } else if (scaredPhase == SCARED_PHASE_FACE2 && now - stateStartTime >= SCARED_FACE_2_DURATION) {
        scaredPhase    = SCARED_PHASE_SAD;
        stateStartTime = now;
        currentSprite  = SPR_SAD;
        drawSprite(SPR_SAD, 0, 0);
      } else if (scaredPhase == SCARED_PHASE_SAD && now - stateStartTime >= SCARED_SAD_DURATION) {
        returnToNormal();
      }
      return;
    }
    if (now - stateStartTime >= REACTION_FACE_DURATION) {
      currentMode    = TEXT_REACTION;
      stateStartTime = now;
      if (activeReaction == REACTION_HAPPY)        drawTextCentered("hi!");
      else if (activeReaction == REACTION_MAD)     drawTextCentered("hisssss.");
      else if (activeReaction == REACTION_DISGUST) drawUghText();
    }
    return;
  }

  if (currentMode == TEXT_REACTION) {
    if (now - stateStartTime >= REACTION_TEXT_DURATION) {
      currentMode     = FACE_HOLD;
      stateStartTime  = now;
      drawSprite(currentSprite, 0, 0);
      lastFloatOffset = 0;
      lastFloatDraw   = now;
    }
    return;
  }

  if (currentMode == FACE_HOLD) {
    if (now - stateStartTime >= REACTION_HOLD_DURATION) {
      returnToNormal(); return;
    }
    if (now - lastFloatDraw >= FLOAT_INTERVAL) {
      int newOffset   = getFloatOffset();
      drawSprite(currentSprite, newOffset, lastFloatOffset);
      lastFloatOffset = newOffset;
      lastFloatDraw   = now;
    }
    return;
  }

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
