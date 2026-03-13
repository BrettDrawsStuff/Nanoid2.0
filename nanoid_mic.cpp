// nanoid_mic.cpp
// ES7210 mic array init, I2S recording, Google Cloud STT via HTTPS REST.
// I2S channel is created only when recording starts and deleted when done,
// so the shared I2S pins (BCK/WS/MCLK) are free for audio the rest of the time.

#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#include <WiFiClientSecure.h>
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "es7210.h"
#include "nanoid_mic.h"
#include "nanoid_audio.h"

// ── Pins ──────────────────────────────────────────────────────────────────────
#define MIC_BCK_IO   9
#define MIC_WS_IO   45
#define MIC_DIN_IO  10
#define MIC_MCK_IO  42

#define MIC_SAMPLE_RATE  44100   // must match shared MCLK (44100 * 256 = 11.29MHz)
#define MIC_STT_RATE     16000   // Google STT target rate
#define MIC_BITS         16
#define MIC_CHANNELS     1
#define MIC_MAX_SECONDS  8
#define MIC_BUF_SAMPLES  (MIC_SAMPLE_RATE * MIC_MAX_SECONDS * 2) // *2 for stereo capture
#define MIC_BUF_BYTES    (MIC_BUF_SAMPLES * (MIC_BITS / 8))

// ── State ─────────────────────────────────────────────────────────────────────
static es7210_dev_handle_t es7210_handle = nullptr;
static i2s_chan_handle_t   rx_handle     = nullptr;
static int16_t*            recBuffer     = nullptr;
static size_t              recSamples    = 0;
static volatile bool       recording     = false;
static bool                processing    = false;
static char                lastTranscript[512] = {0};
static char                googleApiKey[128]   = {0};
static bool                micReady      = false;
static TaskHandle_t        micTaskHandle = nullptr;

// ── FreeRTOS capture task — runs on core 0, drains I2S continuously ──────────
static void micCaptureTask(void* arg) {
    const size_t READ_BYTES = 512;
    while (true) {
        if (recording && rx_handle && recSamples < MIC_BUF_SAMPLES) {            size_t bytesRead = 0;
            size_t toRead = min((size_t)READ_BYTES,
                                (MIC_BUF_SAMPLES - recSamples) * sizeof(int16_t));
            i2s_channel_read(rx_handle, recBuffer + recSamples, toRead,
                             &bytesRead, pdMS_TO_TICKS(20));
            recSamples += bytesRead / sizeof(int16_t);
            if (recSamples >= MIC_BUF_SAMPLES) recording = false; // buffer full
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

// ── Load Google API key from SD ───────────────────────────────────────────────
static bool loadGoogleKey() {
    File f = SD_MMC.open("/google.cfg");
    if (!f) { Serial.println("google.cfg not found — STT disabled"); return false; }
    String key = f.readStringUntil('\n'); key.trim(); f.close();
    if (key.length() == 0) { Serial.println("google.cfg empty"); return false; }
    key.toCharArray(googleApiKey, sizeof(googleApiKey));
    Serial.println("Google API key loaded.");
    return true;
}

// ── WAV header ────────────────────────────────────────────────────────────────
static void writeWavHeader(uint8_t* buf, uint32_t dataBytes, uint32_t sampleRate,
                            uint16_t channels, uint16_t bitsPerSample) {
    uint32_t byteRate   = sampleRate * channels * bitsPerSample / 8;
    uint16_t blockAlign = channels * bitsPerSample / 8;
    uint32_t chunkSize  = 36 + dataBytes;
    memcpy(buf,    "RIFF", 4); memcpy(buf+4,  &chunkSize,  4);
    memcpy(buf+8,  "WAVE", 4); memcpy(buf+12, "fmt ",      4);
    uint32_t sub1 = 16;        memcpy(buf+16, &sub1,       4);
    uint16_t fmt  = 1;         memcpy(buf+20, &fmt,        2);
    memcpy(buf+22, &channels,      2); memcpy(buf+24, &sampleRate,  4);
    memcpy(buf+28, &byteRate,      4); memcpy(buf+32, &blockAlign,  2);
    memcpy(buf+34, &bitsPerSample, 2); memcpy(buf+36, "data",       4);
    memcpy(buf+40, &dataBytes,     4);
}

// ── Base64 encode ─────────────────────────────────────────────────────────────
static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static String base64Encode(const uint8_t* data, size_t len) {
    String out;
    out.reserve((len / 3 + 1) * 4 + 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = ((uint32_t)data[i] << 16);
        if (i+1 < len) b |= ((uint32_t)data[i+1] << 8);
        if (i+2 < len) b |= data[i+2];
        out += b64chars[(b >> 18) & 0x3F];
        out += b64chars[(b >> 12) & 0x3F];
        out += (i+1 < len) ? b64chars[(b >> 6) & 0x3F] : '=';
        out += (i+2 < len) ? b64chars[b & 0x3F]        : '=';
    }
    return out;
}

// ── Google STT REST call ──────────────────────────────────────────────────────
static String sendToGoogleSTT(const uint8_t* wavData, size_t wavLen) {
    String audioB64 = base64Encode(wavData, wavLen);
    String body = "{\"config\":{\"encoding\":\"LINEAR16\",\"languageCode\":\"en-US\"},\"audio\":{\"content\":\"";
    body += audioB64;
    body += "\"}}";

    WiFiClientSecure client;
    client.setInsecure();
    if (!client.connect("speech.googleapis.com", 443)) {
        Serial.println("STT: connect failed"); return "";
    }
    String path = "/v1/speech:recognize?key=";
    path += googleApiKey;
    client.printf("POST %s HTTP/1.1\r\n", path.c_str());
    client.println("Host: speech.googleapis.com");
    client.println("Content-Type: application/json");
    client.printf("Content-Length: %d\r\n", body.length());
    client.println("Connection: close");
    client.println();
    client.print(body);

    String response = "";
    // Wait for response, then read until connection closes
    unsigned long timeout = millis();
    while (client.connected() && millis() - timeout < 15000) {
        while (client.available()) {
            response += (char)client.read();
            timeout = millis(); // reset timeout while data is flowing
        }
        delay(1);
    }
    client.stop();

    int idx = response.indexOf("\"transcript\":");
    Serial.println("STT response body:"); Serial.println(response.substring(response.indexOf("\r\n\r\n")));
    if (idx < 0) { Serial.println("STT: no transcript"); return ""; }
    int start = response.indexOf('"', idx + 13) + 1;
    int end   = response.indexOf('"', start);
    if (start < 1 || end < 0) return "";
    return response.substring(start, end);
}

// ── Open I2S RX channel — called just before recording ───────────────────────
static bool i2s_rx_open() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, NULL, &rx_handle) != ESP_OK) {
        Serial.println("Mic: I2S channel create failed"); rx_handle = nullptr; return false;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)MIC_MCK_IO,
            .bclk = (gpio_num_t)MIC_BCK_IO,
            .ws   = (gpio_num_t)MIC_WS_IO,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)MIC_DIN_IO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false }
        }
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    if (i2s_channel_init_std_mode(rx_handle, &std_cfg) != ESP_OK) {
        Serial.println("Mic: I2S init failed");
        i2s_del_channel(rx_handle); rx_handle = nullptr; return false;
    }
    return true;
}

// ── Close I2S RX channel — returns shared pins to audio ──────────────────────
static void i2s_rx_close() {
    if (!rx_handle) return;
    i2s_channel_disable(rx_handle);
    i2s_del_channel(rx_handle);
    rx_handle = nullptr;
}

// ── Init — ES7210 I2C + buffer only. No I2S claimed at boot. ─────────────────
void nanoid_mic_init() {
    loadGoogleKey();

    recBuffer = (int16_t*)ps_malloc(MIC_BUF_BYTES);
    if (!recBuffer) { Serial.println("Mic buffer alloc failed!"); return; }

    es7210_i2c_config_t i2c_conf = { .i2c_port = I2C_NUM_0, .i2c_addr = ES7210_ADDRRES_00 };
    if (es7210_new_codec(&i2c_conf, &es7210_handle) != ESP_OK) {
        Serial.println("ES7210 create failed!"); return;
    }
    es7210_codec_config_t codec_conf = {
        .sample_rate_hz = MIC_SAMPLE_RATE,
        .mclk_ratio     = 256,
        .i2s_format     = ES7210_I2S_FMT_I2S,
        .bit_width      = ES7210_I2S_BITS_16B,
        .mic_bias       = ES7210_MIC_BIAS_2V87,
        .mic_gain       = ES7210_MIC_GAIN_30DB,
        .flags          = { .tdm_enable = false }
    };
    esp_err_t cerr = es7210_config_codec(es7210_handle, &codec_conf);
    Serial.printf("ES7210 config: %s\n", cerr == ESP_OK ? "OK" : "FAILED");

    // Force gain registers directly via I2C as a safety net
    // 30dB = value 10 = 0x0A, with PGA enable bit 0x10 => 0x1A
    Wire.beginTransmission(0x40); Wire.write(0x43); Wire.write(0x1A); Wire.endTransmission();
    Wire.beginTransmission(0x40); Wire.write(0x44); Wire.write(0x1A); Wire.endTransmission();
    Wire.beginTransmission(0x40); Wire.write(0x45); Wire.write(0x1A); Wire.endTransmission();
    Wire.beginTransmission(0x40); Wire.write(0x46); Wire.write(0x1A); Wire.endTransmission();
    Serial.println("ES7210 gain forced to 30dB.");

    micReady = true;
    xTaskCreatePinnedToCore(micCaptureTask, "micCapture", 4096, nullptr, 1, &micTaskHandle, 0);
    Serial.println("Mic ready (I2S pins free until recording).");
}

// ── I2C scan helper — call once to find ES7210 address ───────────────────────
void nanoid_mic_scan_i2c() {
    Serial.println("I2C scan (looking for ES7210 at 0x40-0x43):");
    for (uint8_t addr = 0x40; addr <= 0x43; addr++) {
        Wire.beginTransmission(addr);
        uint8_t err = Wire.endTransmission();
        Serial.printf("  0x%02X: %s\n", addr, err == 0 ? "FOUND" : "nothing");
    }
}

// ── Prep — reconfigure ES7210 while MCLK is live, before opening I2S ─────────
void nanoid_mic_prep() {
    if (!es7210_handle) return;
    es7210_codec_config_t codec_conf = {
        .sample_rate_hz = MIC_SAMPLE_RATE,
        .mclk_ratio     = 256,
        .i2s_format     = ES7210_I2S_FMT_I2S,
        .bit_width      = ES7210_I2S_BITS_16B,
        .mic_bias       = ES7210_MIC_BIAS_2V87,
        .mic_gain       = ES7210_MIC_GAIN_18DB,
        .flags          = { .tdm_enable = false }
    };
    es7210_config_codec(es7210_handle, &codec_conf);
    // Force gain registers: 18dB = 0x06 | 0x10 = 0x16
    Wire.beginTransmission(0x40); Wire.write(0x43); Wire.write(0x16); Wire.endTransmission();
    Wire.beginTransmission(0x40); Wire.write(0x44); Wire.write(0x16); Wire.endTransmission();
    Wire.beginTransmission(0x40); Wire.write(0x45); Wire.write(0x16); Wire.endTransmission();
    Wire.beginTransmission(0x40); Wire.write(0x46); Wire.write(0x16); Wire.endTransmission();
    Serial.println("ES7210 prepped.");
}

// ── Start recording ───────────────────────────────────────────────────────────
bool nanoid_mic_start() {
    if (!micReady || recording || processing) return false;
    if (!i2s_rx_open()) return false;
    recSamples = 0;
    i2s_channel_enable(rx_handle);
    // Flush any stale data sitting in DMA buffers
    uint8_t flush[512];
    size_t flushed = 0;
    for (int i = 0; i < 8; i++) i2s_channel_read(rx_handle, flush, sizeof(flush), &flushed, pdMS_TO_TICKS(10));
    delay(30);
    recording = true; // set after channel is enabled and flushed
    Serial.println("Mic: recording started.");
    return true;
}

// ── Stop recording + send to STT + release I2S pins ──────────────────────────
void nanoid_mic_stop() {
    if (!recording) return;
    recording = false;
    i2s_rx_close(); // release shared pins back to audio immediately
    Serial.printf("Mic: stopped. %d samples (%.2f sec at 44100Hz stereo).\n",
        recSamples, (float)recSamples / (44100.0f * 2));
    nanoid_audio_reinit(); // restore audio I2S pins after mic teardown

    // Diagnostic: find peak and print first 16 raw samples
    int16_t peak = 0;
    for (size_t i = 0; i < recSamples; i++) {
        int16_t v = abs(recBuffer[i]);
        if (v > peak) peak = v;
    }
    Serial.printf("Mic peak level: %d (0=silence, >500=signal)\n", peak);
    Serial.print("First 16 samples: ");
    for (int i = 0; i < 16 && i < (int)recSamples; i++) {
        Serial.printf("%d ", recBuffer[i]);
    }
    Serial.println();

    if (recSamples == 0 || strlen(googleApiKey) == 0) return;

    processing = true;

    // Stereo → mono: try right channel (index 1) — left was silent
    size_t monoSamples = recSamples / 2;
    for (size_t i = 0; i < monoSamples; i++) recBuffer[i] = recBuffer[i * 2 + 1];

    // Step 2: downsample 44100 → 16000 (ratio ~2.75625) using linear interpolation
    // Output fits in same buffer since it's smaller
    size_t outSamples = (size_t)((float)monoSamples * MIC_STT_RATE / MIC_SAMPLE_RATE);
    float ratio = (float)monoSamples / (float)outSamples;
    for (size_t i = 0; i < outSamples; i++) {
        float pos    = i * ratio;
        size_t idx   = (size_t)pos;
        float frac   = pos - idx;
        if (idx + 1 < monoSamples)
            recBuffer[i] = (int16_t)(recBuffer[idx] * (1.0f - frac) + recBuffer[idx + 1] * frac);
        else
            recBuffer[i] = recBuffer[idx];
    }
    Serial.printf("Resampled: %d → %d samples at %dHz\n", monoSamples, outSamples, MIC_STT_RATE);

    size_t dataBytes = outSamples * sizeof(int16_t);
    size_t wavLen    = 44 + dataBytes;
    uint8_t* wavBuf  = (uint8_t*)ps_malloc(wavLen);
    if (!wavBuf) { Serial.println("WAV alloc failed"); processing = false; return; }

    writeWavHeader(wavBuf, dataBytes, MIC_STT_RATE, 1, MIC_BITS);
    memcpy(wavBuf + 44, recBuffer, dataBytes);

    Serial.println("Sending to Google STT...");
    String transcript = sendToGoogleSTT(wavBuf, wavLen);
    free(wavBuf);

    if (transcript.length() > 0) {
        transcript.toCharArray(lastTranscript, sizeof(lastTranscript));
        Serial.print("Transcript: "); Serial.println(lastTranscript);
    } else {
        lastTranscript[0] = '\0';
        Serial.println("STT: no result.");
    }
    processing = false;
}

// ── Loop — called from main loop, just checks for buffer-full auto-stop ──────
void nanoid_mic_loop() {
    if (recording && recSamples >= MIC_BUF_SAMPLES) nanoid_mic_stop();
}

bool nanoid_mic_busy() { return recording || processing; }

const char* nanoid_mic_last_transcript() { return lastTranscript; }
