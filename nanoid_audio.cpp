// nanoid_audio.cpp
// Isolated from main sketch to prevent Audio.h scope corruption.
// Uses Waveshare's ES8311 driver for proper codec initialization.

#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#include <Audio.h>
#include "es8311.h"

// From pin_config.h
#define I2S_BCK_IO   9
#define I2S_WS_IO   45
#define I2S_DO_IO    8
#define I2S_MCK_IO  42
#define PA          46   // NS4150B amp enable

#define EXAMPLE_SAMPLE_RATE    44100
#define EXAMPLE_MCLK_MULTIPLE  256
#define EXAMPLE_MCLK_FREQ_HZ   (EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE)

// Audio object — must use (false, 3, I2S_NUM_0) per Waveshare example
Audio audio(I2S_NUM_0);
static es8311_handle_t es_handle = nullptr;
static bool audioReady = false;

static void IRAM_ATTR audio_tick(void* arg) {
  audio.loop();
}

void nanoid_audio_init() {
  // 1. Init ES8311 codec via proper driver
  es_handle = es8311_create(I2C_NUM_0, ES8311_ADDRESS_0);
  if (!es_handle) { Serial.println("ES8311 create failed!"); return; }

  const es8311_clock_config_t es_clk = {
    .mclk_inverted      = false,
    .mclk_from_mclk_pin = true,
    .sclk_inverted      = false,
    .mclk_frequency     = EXAMPLE_MCLK_FREQ_HZ,
    .sample_frequency   = EXAMPLE_SAMPLE_RATE
  };
  esp_err_t err = es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  if (err != ESP_OK) { Serial.printf("ES8311 init failed: %d\n", err); return; }

  es8311_voice_volume_set(es_handle, 75, NULL);
  es8311_microphone_config(es_handle, false);

  // Boost analog output stage — REG 0x13 bits control HP driver gain
  // 0x10 = default, try higher gain values
  Wire.beginTransmission(0x18);
  Wire.write(0x13); Wire.write(0x10);  // HP driver on, max analog gain
  Wire.endTransmission();

  Serial.println("ES8311 initialized.");

  // 2. Enable amp
  pinMode(PA, OUTPUT);
  digitalWrite(PA, HIGH);
  delay(50);

  // 3. Configure Audio library
  audio.setPinout(I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO, I2S_MCK_IO);
  audio.setVolume(10);

  // 4. Run audio.loop() on a 20ms timer
  esp_timer_handle_t audio_timer = NULL;
  const esp_timer_create_args_t timer_args = {
    .callback              = &audio_tick,
    .dispatch_method       = ESP_TIMER_TASK,
    .name                  = "audio_tick",
    .skip_unhandled_events = true
  };
  esp_timer_create(&timer_args, &audio_timer);
  esp_timer_start_periodic(audio_timer, 20 * 1000);

  audioReady = true;
  Serial.println("Audio ready.");
}

void nanoid_audio_play(const char* filename) {
  if (!audioReady) return;
  if (!SD_MMC.exists(filename)) {
    Serial.print("Sound missing: "); Serial.println(filename);
    return;
  }
  audio.connecttoFS(SD_MMC, filename);
  Serial.print("Playing: "); Serial.println(filename);
}

void nanoid_audio_loop() {
  // No-op — audio.loop() runs on its own timer now.
}
