# Nanoid 2.0 — LLM Context Prompt

Paste this entire document into any LLM to give it full context on the Nanoid project before asking for help.

---

## What is Nanoid?

Nanoid is an open-source AI-powered desk companion device. It runs on a Waveshare ESP32-S3 1.75" round AMOLED display and has a fully animated character, a voice pipeline, and an AI brain powered by Anthropic Claude. The user can speak to it, and it responds with text on screen and audio chatter sounds. Its personality and memory live on an SD card as plain text config files.

---

## Hardware

**Board:** Waveshare ESP32-S3-Touch-AMOLED-1.75
- ESP32-S3 dual-core, 8MB PSRAM
- 466×466 circular AMOLED touchscreen (round display — corners are clipped)
- ES7210 quad-mic array (I2C address 0x40)
- ES8311 audio codec (I2C address 0x18) + NS4150B amplifier
- QMI8658 IMU
- MicroSD card slot
- USB-C

**Other hardware:** MicroSD card (FAT32), small speaker (4Ω or 8Ω, JST connector)

---

## Software Stack

- **Arduino IDE 2.x** with ESP32 Arduino Core 3.x
- **ESP32-audioI2S** library for audio playback
- **Arduino_GFX_Library** for display
- **Google Cloud Speech-to-Text REST API** for voice transcription
- **Anthropic Claude API** (claude-haiku-4-5-20251001) for AI responses

---

## Codebase Structure

```
nanoid_phase1/
├── nanoid_phase1.ino   ← Main sketch (~1200 lines)
├── nanoid_audio.cpp/h  ← Audio playback (ES8311 + Audio library)
├── nanoid_mic.cpp/h    ← Mic capture (ES7210 + Google STT)
├── nanoid_llm.cpp/h    ← LLM integration (Anthropic + memory)
├── es8311.cpp/h        ← ES8311 codec driver (Wire-based I2C)
├── es8311_reg.h        ← ES8311 register map
├── es7210.cpp/h        ← ES7210 mic driver (Wire-based I2C)
└── es7210_reg.h        ← ES7210 register map
```

---

## Critical Hardware Rules

These are hard-won fixes — do not change them without good reason:

- **MCLK pin is GPIO42.** ES7210 and ES8311 share MCLK at 44100Hz × 256 = 11.29MHz. The mic must capture at 44100Hz stereo for this reason — not 16kHz.
- **I2S_NUM_0** is used for audio output. **I2S_NUM_1** is used for mic RX.
- The mic I2S channel is created on `nanoid_mic_start()` and deleted on `nanoid_mic_stop()` — the shared BCK/WS/MCLK pins must be free for audio at all times except during recording.
- After mic teardown, `nanoid_audio_reinit()` must be called to re-register the audio I2S pins.
- ES7210 and ES8311 drivers use **Wire-based I2C only** — the legacy `i2c_cmd_link` API conflicts with the new I2S driver.
- **Never** call `gfx->fillScreen(BG_COLOR)` anywhere in `nanoid_phase1.ino` except inside intentional glitch effects — not during mode transitions or returning to normal.
- `JUMP_FRAME_COUNT` is **11** (frames `jump1.bmp`–`jump11.bmp`).
- Audio volume sweet spot: `es8311_voice_volume_set(es_handle, 75, NULL)` and `audio.setVolume(12)`.
- `Audio.h` must be isolated in `nanoid_audio.cpp` — including it in the `.ino` corrupts enum scope and breaks touch.

---

## Pin Assignments

| Function | Pin |
|---|---|
| MCLK (shared ES8311 + ES7210) | GPIO42 |
| I2S BCK (shared) | GPIO9 |
| I2S WS (shared) | GPIO45 |
| I2S DO (audio out) | GPIO8 |
| I2S DIN (mic in) | GPIO10 |
| PA amp enable | GPIO46 (HIGH = on) |
| BOOT button | GPIO0 |
| I2C SDA | IIC_SDA (board define) |
| I2C SCL | IIC_SCL (board define) |

---

## State Machine

Nanoid runs a single-file state machine in `loop()`. States:

| State | Description |
|---|---|
| `FACE_NORMAL` | Idle — floating animation, blink, context sprite |
| `FACE_REACTION` | Scared reaction sequence only |
| `TEXT_REACTION` | Showing text ("hi!", "hisssss.", "ugh.") |
| `FACE_HOLD` | Holding reaction sprite before returning to normal |
| `FACE_SAD` | Inactivity sad mode (10 min timeout) |
| `FACE_SLEEP` | Deep sleep mode (30 min timeout) |
| `TEXT_SLEEP` | "zzz..." text before sleep sprite |
| `FACE_GLITCH` | Random glitch animation |
| `FACE_JUMP` | Jump animation (11 frames) |
| `FACE_WALK` | Walk cycle animation |
| `FACE_WAVE` | Wave animation |
| `FACE_LISTEN_PREP` | 400ms non-blocking codec reinit before recording |
| `FACE_LISTEN` | Recording audio |
| `FACE_THINKING` | Processing STT + waiting for LLM response |
| `FACE_TALKING` | Scrolling LLM response text on screen |

---

## Voice Pipeline

1. Hold BOOT button 2.5s → `FACE_LISTEN_PREP` (400ms, ES7210 reconfigured while MCLK live)
2. → `FACE_LISTEN` — mic I2S opens, FreeRTOS capture task on core 0 drains DMA buffer
3. Release button (minimum 1.5s after mic start) → `nanoid_mic_stop()`
4. Capture task stores stereo 44100Hz samples in PSRAM buffer
5. `nanoid_mic_stop()` → stereo-to-mono (right channel) → linear interpolation resample 44100→16kHz
6. WAV header built in PSRAM → base64 encoded → POST to Google Cloud STT REST API
7. Transcript returned → `FACE_THINKING`
8. Transcript sent to Anthropic Claude → LLM response parsed
9. Response sanitized (em dashes, curly quotes → ASCII) → word-wrapped → `FACE_TALKING`
10. Text scrolls up screen, random talk sound plays → emotion reaction → `FACE_NORMAL`

---

## Audio Architecture

- ES8311 initialized via proper driver before `audio.setPinout()`
- `audio.loop()` runs on a 20ms `esp_timer` (not in main loop)
- After mic session, `nanoid_audio_reinit()` calls `audio.setPinout()` + `audio.setVolume()` to restore pins
- Talk sounds: `talk1.wav`–`talk10.wav` in `/snd/` on SD, auto-detected sequentially, random pick per response

---

## Mic Architecture

- `nanoid_mic_init()` — allocates PSRAM buffer, ES7210 I2C config only. No I2S.
- `nanoid_mic_prep()` — reconfigures ES7210 codec (called during LISTEN_PREP while MCLK is live)
- `nanoid_mic_start()` — opens I2S RX channel, enables, flushes DMA, sets `recording = true`
- `micCaptureTask` — FreeRTOS task pinned to core 0, drains I2S into PSRAM buffer continuously while `recording == true`
- `nanoid_mic_stop()` — sets `recording = false`, deletes I2S channel, calls `nanoid_audio_reinit()`, processes audio, sends to STT

---

## LLM Architecture (`nanoid_llm.cpp`)

- API key loaded from `/anthropic.cfg` on SD
- System prompt built from `/personality.cfg` + `/user.cfg` + `/memory.txt` on each call
- Conversation history: last 5 exchanges kept in RAM, cleared on sleep/wake
- Response capped at 60 tokens ("1–2 sentences, cut to 1 if it would be 3")
- Emotion detection: Claude appends `[EMOTION:happy/sad/mad/scared/disgust]` tag, parsed client-side
- After each exchange, a second Claude call updates `/memory.txt` with a bullet-point summary of learned facts
- `nanoid_llm_clear_history()` called on wake from sleep

---

## SD Card Config Files

| File | Contents |
|---|---|
| `/anthropic.cfg` | Anthropic API key (one line) |
| `/google.cfg` | Google Cloud API key (one line) |
| `/wifi.cfg` | Line 1: SSID, Line 2: password |
| `/location.cfg` | Line 1: latitude, Line 2: longitude |
| `/personality.cfg` | Nanoid's character definition (plain text, ~1500 chars max) |
| `/user.cfg` | Info about the user (plain text, ~1000 chars max) |
| `/memory.txt` | Auto-generated persistent memory (bullet points, updated each session) |

---

## Sprite System

- Format: 24-bit BMP, no compression
- Face sprites: 466×466px
- Background color: `BG_COLOR = 0x31A8` (dark teal) — used as the display background
- Walk frames: `walk1.bmp`–`walk4.bmp`
- Jump frames: `jump1.bmp`–`jump11.bmp` (exactly 11)
- Wave frames: variable count
- Weather/time sprites: 28 total variants covering normal/morning/night × hot/cold/rain/snow × blink states
- Sprites stream from SD on demand — nothing is preloaded into PSRAM

---

## Display Notes

- Screen is **circular** — the corners of the 466×466 framebuffer are clipped
- Safe text area for a round display: approximately 300px wide centered (x: 83–383)
- Text rendering uses `Arduino_GFX` built-in font — **ASCII only** (0x20–0x7E)
- Unicode characters (em dash, curly quotes, ellipsis) must be sanitized to ASCII before display
- Text size 3 = 18px wide × 24px tall per character, ~16 chars per line in safe zone
- Scrolling text: 4px per tick at 80ms intervals, targeted `fillRect` clearing per line (no `fillScreen`)

---

## Key Constants

```cpp
#define BG_COLOR         0x31A8   // display background teal
#define SCREEN_W         466
#define SCREEN_H         466
#define SCREEN_CX        233
#define SCREEN_CY        233
#define LONG_PRESS_MS    2500     // boot button hold to activate mic
#define BLINK_INTERVAL   3000     // ms between blinks
#define BLINK_LENGTH     60       // ms blink frame is shown
#define SAD_TIMEOUT      600000   // 10 minutes inactivity → sad
#define SLEEP_TIMEOUT    1800000  // 30 minutes inactivity → sleep
#define JUMP_FRAME_COUNT 11
#define WALK_FRAME_COUNT 4
#define MIC_SAMPLE_RATE  44100    // capture rate (must match MCLK)
#define MIC_STT_RATE     16000    // Google STT target rate
#define MIC_MAX_SECONDS  8        // max recording length
#define REACTION_TEXT_DURATION  8000   // ms text shows in reactions
#define REACTION_HOLD_DURATION  5000   // ms sprite holds before normal
#define SCROLL_SPEED_MS  80       // ms between scroll redraws
#define SCROLL_PX_PER_TICK 4     // pixels per scroll step
```

---

## Common Issues & Solutions

**Audio stops working after mic session**
→ `nanoid_audio_reinit()` must be called after `i2s_rx_close()`. The mic and audio share physical pins — when the mic's I2S channel is deleted, the pin mux is released for all channels.

**Mic captures silence / peak always 0 or 32767**
→ Check that I2S slot config uses `PHILIPS_SLOT` not `MSB_SLOT`. MSB causes bit-framing misalignment with the ES7210.
→ Check that capture rate is 44100Hz, not 16kHz. The shared MCLK at 11.29MHz doesn't support 16kHz.

**STT returns empty / totalBilledTime: 0s**
→ Check peak level — if 32767, signal is clipping. Reduce `ES7210_MIC_GAIN`.
→ Check sample count — if very low (<10000 for a 2s recording), the FreeRTOS capture task isn't running. Verify `xTaskCreatePinnedToCore` call in `nanoid_mic_init()`.

**Display flickers during text scroll**
→ Do not use `gfx->fillScreen()` on every scroll tick. Use targeted `fillRect` per line only.

**Compilation error: Audio.h enum conflict**
→ `Audio.h` must only be included in `nanoid_audio.cpp`. Never include it in `.ino` or other modules.

**Upload fails**
→ Hold BOOT button, press/release RESET, release BOOT, then upload immediately.

---

## What's Working (as of current build)

- Full animated state machine with 15 states
- Weather-aware sprites (28 variants)
- Touch, shake, button interactions
- Full voice pipeline: mic capture → STT → LLM → display
- Persistent memory across sessions via SD card
- Conversation history within session
- Scrolling word-wrapped text display
- Emotion-tagged reactions from LLM
- Talk sounds during response display
- Web dashboard on local network
- All audio: boot, reaction, idle, walk, jump, wave, talk, thinking sounds
