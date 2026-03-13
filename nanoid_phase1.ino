#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"
#include <Wire.h>
#include <FS.h>
#include <SD_MMC.h>
#include <math.h>
#include "TouchDrvCSTXXX.hpp"
#include "SensorQMI8658.hpp"
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "nanoid_audio.h"
#include "nanoid_mic.h"

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

// ─── WIFI & NTP ───────────────────────────────────────────────────────────────
WebServer webServer(80);
bool wifiConnected = false;
bool timesynced    = false;

#define NTP_SERVER "pool.ntp.org"
#define TZ_STRING  "MST7MDT,M3.2.0,M11.1.0"

// ─── LOCATION & WEATHER ───────────────────────────────────────────────────────
float locationLat      = 0.0;
float locationLon      = 0.0;
bool  locationLoaded   = false;

float weatherTempF         = -999.0;
int   weatherCode          = -1;
unsigned long lastWeatherFetch = 0;
const unsigned long WEATHER_INTERVAL = 2UL * 60 * 60 * 1000;

#define TEMP_HOT  80.0
#define TEMP_COLD 60.0

#define HOUR_MORNING_START 5
#define HOUR_MORNING_END   11
#define HOUR_NIGHT_START   21

// ─── SPRITE FILENAMES ─────────────────────────────────────────────────────────
#define SPR_NORMAL             0
#define SPR_BLINK              1
#define SPR_HAPPY              2
#define SPR_SAD                3
#define SPR_MAD                4
#define SPR_SCARED             5
#define SPR_DISGUST            6
#define SPR_CONFUSED           7
#define SPR_MORNING            8
#define SPR_MORNING_BLINK      9
#define SPR_NIGHT              10
#define SPR_NIGHT_BLINK        11
#define SPR_RAIN               12
#define SPR_RAIN_BLINK         13
#define SPR_RAIN_MORNING       14
#define SPR_RAIN_MORNING_BLINK 15
#define SPR_RAIN_NIGHT         16
#define SPR_RAIN_NIGHT_BLINK   17
#define SPR_SNOW               18
#define SPR_SNOW_BLINK         19
#define SPR_SNOW_MORNING       20
#define SPR_SNOW_MORNING_BLINK 21
#define SPR_SNOW_NIGHT         22
#define SPR_SNOW_NIGHT_BLINK   23
#define SPR_COLD               24
#define SPR_COLD_BLINK         25
#define SPR_COLD_MORNING       26
#define SPR_COLD_MORNING_BLINK 27
#define SPR_COLD_NIGHT         28
#define SPR_COLD_NIGHT_BLINK   29
#define SPR_HOT                30
#define SPR_HOT_BLINK          31
#define SPR_HOT_MORNING        32
#define SPR_HOT_MORNING_BLINK  33
#define SPR_HOT_NIGHT          34
#define SPR_HOT_NIGHT_BLINK    35
#define SPR_COUNT              36

const char* spriteFiles[SPR_COUNT] = {
  "/normal.bmp",           // 0
  "/normal_blink.bmp",     // 1
  "/happy.bmp",            // 2
  "/sad.bmp",              // 3
  "/mad.bmp",              // 4
  "/scared.bmp",           // 5
  "/disgust.bmp",          // 6
  "/confused.bmp",         // 7
  "/morning.bmp",          // 8
  "/morning_blink.bmp",    // 9
  "/night.bmp",            // 10
  "/night_blink.bmp",      // 11
  "/rain.bmp",             // 12
  "/rain_blink.bmp",       // 13
  "/rainmorning.bmp",      // 14
  "/rainmorning_blink.bmp",// 15
  "/rainnight.bmp",        // 16
  "/rainnight_blink.bmp",  // 17
  "/snow.bmp",             // 18
  "/snow_blink.bmp",       // 19
  "/snowmorning.bmp",      // 20
  "/snowmorning_blink.bmp",// 21
  "/snownight.bmp",        // 22
  "/snownight_blink.bmp",  // 23
  "/cold.bmp",             // 24
  "/cold_blink.bmp",       // 25
  "/coldmorning.bmp",      // 26
  "/coldmorning_blink.bmp",// 27
  "/coldnight.bmp",        // 28
  "/coldnight_blink.bmp",  // 29
  "/hot.bmp",              // 30
  "/hot_blink.bmp",        // 31
  "/hotmorning.bmp",       // 32
  "/hotmorning_blink.bmp", // 33
  "/hotnight.bmp",         // 34
  "/hotnight_blink.bmp",   // 35
};

// ─── FRAME BUFFER ─────────────────────────────────────────────────────────────
uint16_t* frameBuffer    = nullptr;
int cachedSpriteIndex    = -1;
int cachedSpriteW        = 0;
int cachedSpriteH        = 0;

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
  FACE_LISTEN_PREP,  // codec reinit happening, waiting for MCLK settle
  FACE_LISTEN,
  FACE_THINKING,
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

const unsigned long ZZZ_INTERVAL     = 60000;
const unsigned long ZZZ_DURATION     = 4000;
const uint8_t       SLEEP_BRIGHTNESS = 75;
const uint8_t       AWAKE_BRIGHTNESS = 150;

const unsigned long GLITCH_DURATION   = 2000;
const unsigned long GLITCH_FRAME_RATE = 16;
const unsigned long TERMINAL_HOLD     = 2000;

// Float animation
const unsigned long FLOAT_INTERVAL = 19;
const float FLOAT_AMPLITUDE        = 7.0;
const float FLOAT_SPEED            = 0.0006;

// Jump animation
#define JUMP_FRAME_COUNT 11
#define JUMP_FRAME_MS    42
int jumpFrameIndex          = 0;
unsigned long lastJumpFrame = 0;
unsigned long touchDownTime = 0;
bool touchHeld              = false;

// Walk animation
#define WALK_FRAME_COUNT 8
#define WALK_FRAME_MS    42
int walkFrameIndex          = 0;
unsigned long lastWalkFrame = 0;

// Wave animation
const int waveSequence[]     = {1,2,3,4,3,2,3,4,3,2,3,4,3,2,1};
#define WAVE_SEQUENCE_LENGTH 15
#define WAVE_FRAME_MS        42
int waveFrameIndex           = 0;
unsigned long lastWaveFrame  = 0;
unsigned long nextWaveTime   = 0;

// PWR button (GPIO 0) — short press = walk toggle, long press = listen
#define PWR_BUTTON_PIN 0
bool pwrButtonPrev              = HIGH;
unsigned long pwrDebounce       = 0;
const unsigned long PWR_DEBOUNCE_MS = 50;

// Long press tracking
unsigned long pwrHoldStart       = 0;
bool          pwrHoldArmed       = false;
bool          pwrLongFired       = false;
const unsigned long LONG_PRESS_MS = 2500;
unsigned long micStartTime        = 0; // when mic actually began recording

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

// ─── drawBMP ──────────────────────────────────────────────────────────────────
void drawBMP(int spriteIndex, int floatOffset, int prevOffset, bool forceReload = false) {
  if (!frameBuffer) return;
  if (spriteIndex < 0 || spriteIndex >= SPR_COUNT) return;

  if (forceReload || spriteIndex != cachedSpriteIndex) {
    const char* filename = spriteFiles[spriteIndex];
    File file = SD_MMC.open(filename);
    if (!file) {
      Serial.print("Missing: "); Serial.println(filename);
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
    cachedSpriteIndex = spriteIndex;
    cachedSpriteW     = imgWidth;
    cachedSpriteH     = imgHeight;
  }

  if (floatOffset != prevOffset) {
    if (floatOffset > prevOffset) {
      gfx->fillRect(0, prevOffset, cachedSpriteW, floatOffset - prevOffset, BG_COLOR);
    } else {
      gfx->fillRect(0, cachedSpriteH + floatOffset, cachedSpriteW, prevOffset - floatOffset, BG_COLOR);
    }
  }
  gfx->draw16bitRGBBitmap(0, floatOffset, frameBuffer, cachedSpriteW, cachedSpriteH);
}

// ─── drawAnimFrame ────────────────────────────────────────────────────────────
void drawAnimFrame(const char* filename, int drawX, int drawY) {
  if (!frameBuffer) return;
  File file = SD_MMC.open(filename);
  if (!file) { Serial.print("Missing: "); Serial.println(filename); return; }
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
  cachedSpriteIndex = -1;
  if (drawX < 0) drawX = (SCREEN_W - imgWidth) / 2;
  if (drawY < 0) drawY = SCREEN_H - imgHeight - 20;
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
  drawTextScreen(message, SCREEN_CX - (msgLen * 12), SCREEN_CY - 20);
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
  gfx->setCursor(SCREEN_CX - (6 * 12), SCREEN_CY - 20);
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
    int y = 0;
    while (y < SCREEN_H) {
      int h = random(4, 28), shift = random(-50, 50);
      uint16_t c = barColors[random(15)];
      if (shift >= 0) {
        gfx->fillRect(shift, y, SCREEN_W - shift, h, c);
        gfx->fillRect(0, y, shift, h, barColors[random(15)]);
      } else {
        gfx->fillRect(0, y, SCREEN_W + shift, h, c);
        gfx->fillRect(SCREEN_W + shift, y, -shift, h, barColors[random(15)]);
      }
      y += h + random(0, 4);
    }
    for (int i = 0; i < 20; i++) gfx->fillRect(0, random(SCREEN_H), SCREEN_W, random(1, 3), random(0xFFFF));
    for (int i = 0; i < 5;  i++) gfx->fillRect(random(SCREEN_W), 0, random(2, 8), SCREEN_H, random(0xFFFF));
    for (int i = 0; i < 200; i++) gfx->drawPixel(random(SCREEN_W), random(SCREEN_H), random(0xFFFF));
  } else {
    if (terminalCharIndex == 0) gfx->fillScreen(0x0000);
    const char chars[] = "0123456789ABCDEF><@#!%^&*?/\\|~=+-_[]{}();:.,LOAD EXEC RUN ERROR MEMORY OVERFLOW ACCESS";
    int charsLen = strlen(chars), charW = 12, charH = 16;
    int cols = SCREEN_W / charW, rows = SCREEN_H / charH;
    int charsPerFrame = max(2, (cols * rows) / 150);
    for (int i = 0; i < charsPerFrame && terminalCharIndex < cols * rows; i++) {
      int row = terminalCharIndex / cols, col = terminalCharIndex % cols;
      char c = chars[random(charsLen)];
      int r = random(10);
      uint16_t color = (r == 0) ? 0xFFFF : (r <= 2) ? 0x0320 : 0x07E0;
      gfx->setTextColor(color);
      gfx->setTextSize(2, 2, 0);
      gfx->setCursor(col * charW, row * charH);
      gfx->print(c);
      if (random(8) == 0)
        gfx->fillRect(col * charW + random(-10, 10), row * charH, random(2, 8), 1, random(0xFFFF));
      terminalCharIndex++;
    }
  }
}

// ─── RANDOM EVENT SCHEDULING ──────────────────────────────────────────────────
void scheduleNextWave() {
  nextWaveTime = millis() + (10UL * 60 * 1000) + random(35UL * 60 * 1000);
  Serial.printf("Next wave in %lum\n", (nextWaveTime - millis()) / 60000);
}
void scheduleNextDisgust() {
  nextDisgustTime = millis() + (5UL * 60 * 1000) + random(25UL * 60 * 1000);
  Serial.printf("Next disgust in %lum\n", (nextDisgustTime - millis()) / 60000);
}
void scheduleNextGlitch() {
  nextGlitchTime = millis() + (2UL * 60 * 1000) + random(13UL * 60 * 1000);
  Serial.printf("Next glitch in %lum\n", (nextGlitchTime - millis()) / 60000);
}

// ─── TIME HELPERS ─────────────────────────────────────────────────────────────
bool getTimeInfo(struct tm* ti) {
  if (!timesynced) return false;
  return getLocalTime(ti);
}

int getCurrentHour() {
  struct tm ti;
  if (!getTimeInfo(&ti)) return -1;
  return ti.tm_hour;
}

// ─── CONTEXT SPRITE ───────────────────────────────────────────────────────────
int getContextSprite(bool wantBlink) {
  int hour       = getCurrentHour();
  bool isMorning = (hour >= 0) && (hour >= HOUR_MORNING_START && hour < HOUR_MORNING_END);
  bool isNight   = (hour >= 0) && (hour >= HOUR_NIGHT_START   || hour < HOUR_MORNING_START);

  bool hasWeather = (weatherTempF > -999.0);
  bool isHot  = hasWeather && (weatherTempF >= TEMP_HOT);
  bool isCold = hasWeather && (weatherTempF <= TEMP_COLD);
  bool isRain = hasWeather && (
    (weatherCode >= 51 && weatherCode <= 67) ||
    (weatherCode >= 80 && weatherCode <= 82) ||
    (weatherCode >= 95 && weatherCode <= 99));
  bool isSnow = hasWeather && (weatherCode >= 71 && weatherCode <= 77);

  if (isHot) {
    if (isMorning) return wantBlink ? SPR_HOT_MORNING_BLINK  : SPR_HOT_MORNING;
    if (isNight)   return wantBlink ? SPR_HOT_NIGHT_BLINK    : SPR_HOT_NIGHT;
    return wantBlink ? SPR_HOT_BLINK : SPR_HOT;
  }
  if (isCold) {
    if (isMorning) return wantBlink ? SPR_COLD_MORNING_BLINK : SPR_COLD_MORNING;
    if (isNight)   return wantBlink ? SPR_COLD_NIGHT_BLINK   : SPR_COLD_NIGHT;
    return wantBlink ? SPR_COLD_BLINK : SPR_COLD;
  }
  if (isSnow) {
    if (isMorning) return wantBlink ? SPR_SNOW_MORNING_BLINK : SPR_SNOW_MORNING;
    if (isNight)   return wantBlink ? SPR_SNOW_NIGHT_BLINK   : SPR_SNOW_NIGHT;
    return wantBlink ? SPR_SNOW_BLINK : SPR_SNOW;
  }
  if (isRain) {
    if (isMorning) return wantBlink ? SPR_RAIN_MORNING_BLINK : SPR_RAIN_MORNING;
    if (isNight)   return wantBlink ? SPR_RAIN_NIGHT_BLINK   : SPR_RAIN_NIGHT;
    return wantBlink ? SPR_RAIN_BLINK : SPR_RAIN;
  }
  if (isMorning) return wantBlink ? SPR_MORNING_BLINK : SPR_MORNING;
  if (isNight)   return wantBlink ? SPR_NIGHT_BLINK   : SPR_NIGHT;
  return wantBlink ? SPR_BLINK : SPR_NORMAL;
}

// ─── REACTION TRIGGER ─────────────────────────────────────────────────────────
void triggerReaction(Reaction r) {
  activeReaction = r;
  currentMode    = FACE_REACTION;
  stateStartTime = millis();
  isBlinking     = false;
  if (r == REACTION_HAPPY)        { currentSprite = SPR_HAPPY;   nanoid_audio_play("/snd/happy.wav");   Serial.println("Reaction: HAPPY"); }
  else if (r == REACTION_MAD)     { currentSprite = SPR_MAD;     nanoid_audio_play("/snd/mad.wav");     Serial.println("Reaction: MAD"); }
  else if (r == REACTION_DISGUST) { currentSprite = SPR_DISGUST; nanoid_audio_play("/snd/disgust.wav"); Serial.println("Reaction: DISGUST"); }
  else if (r == REACTION_SCARED)  { currentSprite = SPR_SCARED;  scaredPhase = SCARED_PHASE_FACE1; nanoid_audio_play("/snd/scared.wav"); Serial.println("Reaction: SCARED"); }
  drawBMP(currentSprite, 0, 0, true);
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
  drawBMP(getContextSprite(false), 0, 0, true);
  Serial.println("Returned to normal");
}

// ─── ENTER SAD MODE ───────────────────────────────────────────────────────────
void enterSadMode() {
  currentMode     = FACE_SAD;
  currentSprite   = SPR_SAD;
  stateStartTime  = millis();
  isBlinking      = false;
  lastFloatOffset = 0;
  drawBMP(SPR_SAD, 0, 0, true);
  nanoid_audio_play("/snd/sad.wav");
  Serial.println("Entered sad mode");
}

// ─── ENTER SLEEP MODE ─────────────────────────────────────────────────────────
void enterSleepMode() {
  currentMode     = FACE_SLEEP;
  inSleepMode     = true;
  lastZzzTime     = millis();
  lastFloatOffset = 0;
  gfx->setBrightness(SLEEP_BRIGHTNESS);
  drawBMP(SPR_BLINK, 0, 0, true);
  nanoid_audio_play("/snd/sleep.wav");
  Serial.println("Entered sleep mode");
}

// ─── FLOAT OFFSET ─────────────────────────────────────────────────────────────
int getFloatOffset() {
  return (int)(sin(millis() * FLOAT_SPEED) * FLOAT_AMPLITUDE);
}

// ─── WIFI ─────────────────────────────────────────────────────────────────────
bool loadWifiConfig(char* ssid, char* password, int maxLen) {
  File f = SD_MMC.open("/wifi.cfg");
  if (!f) { Serial.println("wifi.cfg not found — skipping WiFi"); return false; }
  String s = f.readStringUntil('\n'); s.trim();
  String p = f.readStringUntil('\n'); p.trim();
  f.close();
  if (s.length() == 0 || p.length() == 0) { Serial.println("wifi.cfg malformed"); return false; }
  s.toCharArray(ssid, maxLen);
  p.toCharArray(password, maxLen);
  return true;
}

void initWifi() {
  char ssid[64] = {0}, password[64] = {0};
  if (!loadWifiConfig(ssid, password, 64)) return;
  Serial.print("Connecting to WiFi: "); Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) delay(200);
  if (WiFi.status() != WL_CONNECTED) { Serial.println("WiFi timed out"); WiFi.disconnect(true); return; }
  wifiConnected = true;
  Serial.print("WiFi connected. IP: "); Serial.println(WiFi.localIP());
  configTzTime(TZ_STRING, NTP_SERVER);
  Serial.print("Syncing NTP");
  unsigned long ntpStart = millis();
  struct tm ti;
  while (!getLocalTime(&ti) && millis() - ntpStart < 8000) { Serial.print("."); delay(500); }
  if (getLocalTime(&ti)) {
    timesynced = true;
    Serial.printf("\nTime synced: %02d:%02d:%02d\n", ti.tm_hour, ti.tm_min, ti.tm_sec);
  } else {
    Serial.println("\nNTP sync failed");
  }
}

// ─── LOCATION ─────────────────────────────────────────────────────────────────
void loadLocationConfig() {
  File f = SD_MMC.open("/location.cfg");
  if (!f) { Serial.println("location.cfg not found — weather disabled"); return; }
  String lat = f.readStringUntil('\n'); lat.trim();
  String lon = f.readStringUntil('\n'); lon.trim();
  f.close();
  if (lat.length() == 0 || lon.length() == 0) { Serial.println("location.cfg malformed"); return; }
  locationLat    = lat.toFloat();
  locationLon    = lon.toFloat();
  locationLoaded = true;
  Serial.printf("Location: %.4f, %.4f\n", locationLat, locationLon);
}

// ─── WEATHER ──────────────────────────────────────────────────────────────────
void fetchWeather() {
  if (!wifiConnected || !locationLoaded) return;

  WiFiClientSecure client;
  client.setInsecure();
  const char* host = "api.open-meteo.com";
  char path[200];
  snprintf(path, sizeof(path),
    "/v1/forecast?latitude=%.4f&longitude=%.4f&current_weather=true&temperature_unit=fahrenheit",
    locationLat, locationLon);

  if (!client.connect(host, 443)) { Serial.println("Weather: connect failed"); return; }
  client.printf("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);

  unsigned long timeout = millis();
  while (client.available() == 0 && millis() - timeout < 8000) delay(10);
  if (!client.available()) { Serial.println("Weather: timed out"); client.stop(); return; }

  String full = "";
  while (client.available()) full += (char)client.read();
  client.stop();

  int jsonStart = full.indexOf('{');
  if (jsonStart < 0) { Serial.println("Weather: no JSON in response"); return; }
  String body = full.substring(jsonStart);

  int cwIdx = body.indexOf("\"current_weather\":");
  if (cwIdx < 0) {
    Serial.println("Weather: current_weather block not found");
    Serial.println(body.substring(0, 200));
    return;
  }
  String cw = body.substring(cwIdx);

  int tIdx = cw.indexOf("\"temperature\":");
  int cIdx = cw.indexOf("\"weathercode\":");
  if (tIdx < 0 || cIdx < 0) {
    Serial.println("Weather: parse failed");
    Serial.println(cw.substring(0, 200));
    return;
  }

  weatherTempF     = cw.substring(tIdx + 14).toFloat();
  weatherCode      = cw.substring(cIdx + 14).toInt();
  lastWeatherFetch = millis();
  Serial.printf("Weather: %.1f°F  code:%d\n", weatherTempF, weatherCode);
}

// ─── WEB SERVER ───────────────────────────────────────────────────────────────
void handleRoot() {
  struct tm ti;
  bool hasTime = getLocalTime(&ti);
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Nanoid</title>";
  html += "<style>body{background:#1a1a2e;color:#e0e0e0;font-family:monospace;padding:2em;}";
  html += "h1{color:#7fff7f;}span.dim{color:#888;}.box{border:1px solid #444;padding:1em;margin:1em 0;border-radius:6px;}</style>";
  html += "</head><body><h1>// NANOID v2.0</h1><div class='box'>";
  if (hasTime) {
    html += "<p>&#128336; <b>" + String(ti.tm_hour) + ":" + (ti.tm_min < 10 ? "0" : "") + String(ti.tm_min);
    html += "</b> <span class='dim'>Mountain Time</span></p>";
  } else {
    html += "<p><span class='dim'>time not synced</span></p>";
  }
  if (weatherTempF > -999.0) {
    html += "<p>&#127777; <b>" + String((int)weatherTempF) + "°F</b>";
    html += " <span class='dim'>code:" + String(weatherCode) + "</span></p>";
  }
  html += "<p>&#128246; IP: " + WiFi.localIP().toString() + "</p>";
  html += "</div><div class='box'><p><span class='dim'>more controls coming soon...</span></p></div>";
  html += "</body></html>";
  webServer.send(200, "text/html", html);
}

void initWebServer() {
  if (!wifiConnected) return;
  webServer.on("/", handleRoot);
  webServer.begin();
  Serial.println("Web server started");
}

// ─── AUDIO SCHEDULING ─────────────────────────────────────────────────────────
unsigned long nextIdleSound = 0;
void scheduleNextIdleSound() {
  nextIdleSound = millis() + (3UL * 60 * 1000) + random(7UL * 60 * 1000);
}

// ─── SETUP ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Wire.begin(IIC_SDA, IIC_SCL);
  pinMode(PWR_BUTTON_PIN, INPUT_PULLUP);

  touch.setPins(TP_RESET, TP_INT);
  if (!touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL)) Serial.println("Touch init failed!");
  else Serial.println("Touch initialized.");
  attachInterrupt(TP_INT, onTouchInterrupt, FALLING);

  if (!qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) Serial.println("IMU init failed!");
  else {
    Serial.println("IMU initialized.");
    qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_125Hz, SensorQMI8658::LPF_MODE_0);
    qmi.enableAccelerometer();
  }

  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  if (!SD_MMC.begin("/sdcard", true)) Serial.println("SD mount failed!");
  else Serial.println("SD mounted.");

  nanoid_audio_init();
  nanoid_mic_init();
  nanoid_mic_scan_i2c();
  initWifi();
  initWebServer();
  loadLocationConfig();
  fetchWeather();

  if (!gfx->begin()) Serial.println("Display init failed!");
  gfx->setBrightness(AWAKE_BRIGHTNESS);

  frameBuffer = (uint16_t*)ps_malloc(SCREEN_W * SCREEN_H * 2);
  if (!frameBuffer) Serial.println("frameBuffer alloc failed!");
  else {
    Serial.print("frameBuffer ready. Free PSRAM: ");
    Serial.print(ESP.getFreePsram() / 1024);
    Serial.println("KB");
  }

  unsigned long now = millis();
  lastBlinkTime     = now;
  lastFloatDraw     = now;
  lastActivityTime  = now;

  scheduleNextDisgust();
  scheduleNextGlitch();
  scheduleNextWave();

  scheduleNextIdleSound();
  nanoid_audio_play("/snd/boot.wav");

  drawBMP(getContextSprite(false), 0, 0, true);
}

// ─── MAIN LOOP ────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  nanoid_audio_loop();
  nanoid_mic_loop();
  if (wifiConnected) webServer.handleClient();

  if (wifiConnected && locationLoaded &&
      (lastWeatherFetch == 0 || millis() - lastWeatherFetch >= WEATHER_INTERVAL)) {
    fetchWeather();
  }

  // ── PWR BUTTON — short press = walk toggle, long press (2.5s) = listen ──
  bool pwrButton = digitalRead(PWR_BUTTON_PIN);

  if (pwrButton == LOW && pwrButtonPrev == HIGH && (now - pwrDebounce > PWR_DEBOUNCE_MS)) {
    pwrDebounce  = now;
    pwrHoldStart = now;
    pwrHoldArmed = true;
    pwrLongFired = false;
  }

  if (pwrHoldArmed && pwrButton == LOW && !pwrLongFired) {
    if (now - pwrHoldStart >= LONG_PRESS_MS) {
      pwrLongFired = true;
      pwrHoldArmed = false;
      if (currentMode == FACE_NORMAL || currentMode == FACE_WALK) {
        if (currentMode == FACE_WALK) returnToNormal();
        currentMode      = FACE_LISTEN_PREP;
        stateStartTime   = now;
        lastActivityTime = now;
        drawBMP(SPR_CONFUSED, 0, 0, true);
        lastFloatOffset = 0;
        Serial.println("Listen prep started.");
      }
    }
  }

  if (pwrButton == HIGH && pwrButtonPrev == LOW) {
    if (pwrHoldArmed && !pwrLongFired) {
      // Short press — walk toggle
      if (currentMode == FACE_NORMAL) {
        currentMode      = FACE_WALK;
        walkFrameIndex   = 0;
        lastWalkFrame    = now;
        lastActivityTime = now;
        nanoid_audio_play("/snd/walk.wav");
        Serial.println("Walk ON");
      } else if (currentMode == FACE_WALK) {
        returnToNormal();
        Serial.println("Walk OFF");
      }
    }
    if (currentMode == FACE_LISTEN && (millis() - micStartTime >= 1500)) {
      Serial.printf("Stopping mic. micStartTime=%lu now=%lu elapsed=%lu\n", micStartTime, millis(), millis()-micStartTime);
      // Released during listen (minimum 1.5s recorded) — stop and process
      nanoid_mic_stop();
      currentMode    = FACE_THINKING;
      stateStartTime = now;
      nanoid_audio_play("/snd/thinking.wav");
      drawBMP(SPR_CONFUSED, 0, 0, true);
      lastFloatOffset = 0;
      Serial.println("Thinking...");
    }
    pwrHoldArmed = false;
    pwrLongFired = false;
  }

  pwrButtonPrev = pwrButton;

  // ── TOUCH ──
  if (touchInterrupt) {
    touchInterrupt = false;
    uint8_t touched = touch.getPoint(touchX, touchY, touch.getSupportTouchPoint());
    if (touched > 0) {
      lastActivityTime = now;
      if (currentMode == FACE_SLEEP || currentMode == TEXT_SLEEP || currentMode == FACE_SAD) {
        nanoid_audio_play("/snd/wake.wav");
        returnToNormal();
        tapCount  = 0;
        touchHeld = false;
        return;
      }
      if (currentMode == FACE_NORMAL) {
        if (!touchHeld) { touchHeld = true; touchDownTime = now; }
        if (now - lastTapTime > TAP_GAP) {
          tapCount++;
          lastTapTime    = now;
          tapWindowStart = now;
        }
      }
    } else {
      if (touchHeld && currentMode == FACE_NORMAL) {
        Serial.print("Released. tapCount: "); Serial.println(tapCount);
        if (tapCount >= 10 && tapCount <= 60) {
          currentMode    = FACE_JUMP;
          jumpFrameIndex = 0;
          lastJumpFrame  = now;
          tapCount       = 0;
          touchHeld      = false;
          nanoid_audio_play("/snd/jump.wav");
          Serial.println("Jump triggered!");
          return;
        }
      }
      touchHeld = false;
    }
  }

  if (tapCount > 0 && currentMode == FACE_NORMAL && (now - tapWindowStart > TAP_WINDOW)) {
    if (tapCount == 2)      triggerReaction(REACTION_HAPPY);
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
    unsigned long inactive = millis() - lastActivityTime;
    if (inactive >= SLEEP_TIMEOUT) { enterSleepMode(); return; }
    if (inactive >= SAD_TIMEOUT && currentMode == FACE_NORMAL) { enterSadMode(); return; }
  }

  if (currentMode == FACE_NORMAL) {
    if (now >= nextIdleSound) {
      scheduleNextIdleSound();
      const char* idleSounds[] = {"/snd/idle1.wav", "/snd/idle2.wav", "/snd/idle3.wav"};
      nanoid_audio_play(idleSounds[random(3)]);
    }
    if (now >= nextDisgustTime) { scheduleNextDisgust(); triggerReaction(REACTION_DISGUST); return; }
    if (now >= nextGlitchTime) {
      scheduleNextGlitch();
      glitchType = random(2); terminalCharIndex = 0; terminalDone = false; terminalGlitching = false;
      currentMode = FACE_GLITCH; glitchStartTime = now; lastGlitchFrame = now;
      nanoid_audio_play("/snd/glitch.wav");
      Serial.print("Glitch! "); Serial.println(glitchType == 0 ? "rainbow" : "terminal");
      return;
    }
    if (now >= nextWaveTime) {
      scheduleNextWave();
      currentMode = FACE_WAVE; waveFrameIndex = 0; lastWaveFrame = now;
      nanoid_audio_play("/snd/wave.wav");
      Serial.println("Wave triggered!");
      return;
    }
  }

  if (currentMode == FACE_LISTEN_PREP) {
    // On first tick of prep: reconfigure ES7210 codec while MCLK is live
    if (now - stateStartTime < 10) {
      nanoid_mic_prep();
    }
    // After 400ms settle, open I2S and start recording
    if (now - stateStartTime >= 400) {
      nanoid_mic_start();
      micStartTime = millis();
      currentMode  = FACE_LISTEN;
      Serial.printf("Mic started. micStartTime=%lu\n", micStartTime);
      Serial.println("Listen mode triggered.");
    }
    return;
  }

  if (currentMode == FACE_LISTEN) {
    // Float on confused face while holding button and recording
    if (now - lastFloatDraw >= FLOAT_INTERVAL) {
      int newOffset = getFloatOffset();
      drawBMP(SPR_CONFUSED, newOffset, lastFloatOffset);
      lastFloatOffset = newOffset;
      lastFloatDraw   = now;
    }
    return;
  }

  if (currentMode == FACE_THINKING) {
    // Wait for mic processing to finish, then react
    if (!nanoid_mic_busy()) {
      const char* transcript = nanoid_mic_last_transcript();
      if (strlen(transcript) > 0) {
        Serial.print("Transcript: "); Serial.println(transcript);
        // TODO: send to LLM — next phase
        triggerReaction(REACTION_HAPPY);
      } else {
        returnToNormal();
      }
    }
    return;
  }

  if (currentMode == FACE_GLITCH) {
    if (glitchType == 0) {
      if (now - glitchStartTime >= GLITCH_DURATION) {
        drawBMP(getContextSprite(false), lastFloatOffset, lastFloatOffset, true);
        currentMode = FACE_NORMAL;
        Serial.println("Glitch ended");
      } else if (now - lastGlitchFrame >= GLITCH_FRAME_RATE) {
        drawGlitchFrame(); lastGlitchFrame = now;
      }
    } else {
      if (!terminalDone) {
        if (now - lastGlitchFrame >= GLITCH_FRAME_RATE) {
          drawGlitchFrame(); lastGlitchFrame = now;
          if (terminalCharIndex >= (SCREEN_W / 12) * (SCREEN_H / 16)) {
            terminalDone = true; terminalDoneTime = now;
          }
        }
      } else {
        if (now - terminalDoneTime >= TERMINAL_HOLD) {
          drawBMP(getContextSprite(false), lastFloatOffset, lastFloatOffset, true);
          currentMode = FACE_NORMAL;
          Serial.println("Glitch ended");
        }
      }
    }
    return;
  }

  if (currentMode == FACE_JUMP) {
    if (now - lastJumpFrame >= JUMP_FRAME_MS) {
      char filename[16];
      snprintf(filename, sizeof(filename), "/jump%d.bmp", jumpFrameIndex + 1);
      drawAnimFrame(filename, -1, -1);
      lastJumpFrame = now;
      jumpFrameIndex++;
      if (jumpFrameIndex >= JUMP_FRAME_COUNT) { returnToNormal(); Serial.println("Jump complete"); }
    }
    return;
  }

  if (currentMode == FACE_WALK) {
    lastActivityTime = now;
    if (now - lastWalkFrame >= WALK_FRAME_MS) {
      char filename[16];
      snprintf(filename, sizeof(filename), "/walk%d.bmp", walkFrameIndex + 1);
      drawAnimFrame(filename, -1, -1);
      lastWalkFrame  = now;
      walkFrameIndex = (walkFrameIndex + 1) % WALK_FRAME_COUNT;
    }
    return;
  }

  if (currentMode == FACE_WAVE) {
    if (now - lastWaveFrame >= WAVE_FRAME_MS) {
      char filename[16];
      snprintf(filename, sizeof(filename), "/wave%d.bmp", waveSequence[waveFrameIndex]);
      drawAnimFrame(filename, -1, -1);
      lastWaveFrame = now;
      waveFrameIndex++;
      if (waveFrameIndex >= WAVE_SEQUENCE_LENGTH) { returnToNormal(); Serial.println("Wave complete"); }
    }
    return;
  }

  if (currentMode == FACE_SAD) return;

  if (currentMode == FACE_SLEEP) {
    if (now - lastZzzTime >= ZZZ_INTERVAL) {
      currentMode = TEXT_SLEEP; stateStartTime = now;
      drawTextCentered("zzz..."); Serial.println("zzz...");
    }
    return;
  }

  if (currentMode == TEXT_SLEEP) {
    if (now - stateStartTime >= ZZZ_DURATION) {
      currentMode = FACE_SLEEP; lastZzzTime = now;
      drawBMP(SPR_BLINK, 0, 0, true);
    }
    return;
  }

  if (currentMode == FACE_REACTION) {
    if (activeReaction == REACTION_SCARED) {
      if (scaredPhase == SCARED_PHASE_FACE1 && now - stateStartTime >= SCARED_FACE_1_DURATION) {
        scaredPhase = SCARED_PHASE_TEXT; stateStartTime = now; drawAhhhhText();
      } else if (scaredPhase == SCARED_PHASE_TEXT && now - stateStartTime >= SCARED_TEXT_DURATION) {
        scaredPhase = SCARED_PHASE_FACE2; stateStartTime = now; drawBMP(SPR_SCARED, 0, 0, true);
      } else if (scaredPhase == SCARED_PHASE_FACE2 && now - stateStartTime >= SCARED_FACE_2_DURATION) {
        scaredPhase = SCARED_PHASE_SAD; stateStartTime = now;
        currentSprite = SPR_SAD; drawBMP(SPR_SAD, 0, 0, true);
      } else if (scaredPhase == SCARED_PHASE_SAD && now - stateStartTime >= SCARED_SAD_DURATION) {
        returnToNormal();
      }
      return;
    }
    if (now - stateStartTime >= REACTION_FACE_DURATION) {
      currentMode = TEXT_REACTION; stateStartTime = now;
      if (activeReaction == REACTION_HAPPY)        drawTextCentered("hi!");
      else if (activeReaction == REACTION_MAD)     drawTextCentered("hisssss.");
      else if (activeReaction == REACTION_DISGUST) drawUghText();
    }
    return;
  }

  if (currentMode == TEXT_REACTION) {
    if (now - stateStartTime >= REACTION_TEXT_DURATION) {
      currentMode = FACE_HOLD; stateStartTime = now;
      drawBMP(currentSprite, 0, 0, true);
      lastFloatOffset = 0; lastFloatDraw = now;
    }
    return;
  }

  if (currentMode == FACE_HOLD) {
    if (now - stateStartTime >= REACTION_HOLD_DURATION) { returnToNormal(); return; }
    if (now - lastFloatDraw >= FLOAT_INTERVAL) {
      int newOffset = getFloatOffset();
      drawBMP(currentSprite, newOffset, lastFloatOffset);
      lastFloatOffset = newOffset; lastFloatDraw = now;
    }
    return;
  }

  if (currentMode == FACE_NORMAL) {
    if (!isBlinking && (now - lastBlinkTime >= BLINK_INTERVAL)) {
      isBlinking = true; blinkStartTime = now;
      drawBMP(getContextSprite(true), lastFloatOffset, lastFloatOffset, true);
    }
    if (isBlinking && (now - blinkStartTime >= BLINK_LENGTH)) {
      isBlinking = false; lastBlinkTime = now;
    }
    if (!isBlinking && (now - lastFloatDraw >= FLOAT_INTERVAL)) {
      int newOffset = getFloatOffset();
      drawBMP(getContextSprite(false), newOffset, lastFloatOffset);
      lastFloatOffset = newOffset; lastFloatDraw = now;
    }
  }
}
