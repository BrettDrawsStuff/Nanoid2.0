/*
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_types.h"
#include "esp_err.h"
#include "driver/i2c.h"

#define ES8311_ADDRRES_0 0x18u
#define ES8311_ADDRESS_0 0x18u
#define ES8311_ADDRESS_1 0x19u

#ifdef __cplusplus
extern "C" {
#endif

typedef void *es8311_handle_t;

typedef enum {
    ES8311_MIC_GAIN_MIN = -1,
    ES8311_MIC_GAIN_0DB,
    ES8311_MIC_GAIN_6DB,
    ES8311_MIC_GAIN_12DB,
    ES8311_MIC_GAIN_18DB,
    ES8311_MIC_GAIN_24DB,
    ES8311_MIC_GAIN_30DB,
    ES8311_MIC_GAIN_36DB,
    ES8311_MIC_GAIN_42DB,
    ES8311_MIC_GAIN_MAX
} es8311_mic_gain_t;

typedef enum {
    ES8311_FADE_OFF = 0,
    ES8311_FADE_4LRCK,
    ES8311_FADE_8LRCK,
    ES8311_FADE_16LRCK,
    ES8311_FADE_32LRCK,
    ES8311_FADE_64LRCK,
    ES8311_FADE_128LRCK,
    ES8311_FADE_256LRCK,
    ES8311_FADE_512LRCK,
    ES8311_FADE_1024LRCK,
    ES8311_FADE_2048LRCK,
    ES8311_FADE_4096LRCK,
    ES8311_FADE_8192LRCK,
    ES8311_FADE_16384LRCK,
    ES8311_FADE_32768LRCK,
    ES8311_FADE_65536LRCK
} es8311_fade_t;

typedef enum es8311_resolution_t {
    ES8311_RESOLUTION_16 = 16,
    ES8311_RESOLUTION_18 = 18,
    ES8311_RESOLUTION_20 = 20,
    ES8311_RESOLUTION_24 = 24,
    ES8311_RESOLUTION_32 = 32
} es8311_resolution_t;

typedef struct es8311_clock_config_t {
    bool mclk_inverted;
    bool mclk_from_mclk_pin;
    bool sclk_inverted;
    int  mclk_frequency;
    int  sample_frequency;
} es8311_clock_config_t;

esp_err_t es8311_init(es8311_handle_t dev, const es8311_clock_config_t *const clk_cfg,
                      const es8311_resolution_t res_in, const es8311_resolution_t res_out);
esp_err_t es8311_voice_volume_set(es8311_handle_t dev, int volume, int *volume_set);
esp_err_t es8311_voice_volume_get(es8311_handle_t dev, int *volume);
esp_err_t es8311_voice_mute(es8311_handle_t dev, bool enable);
esp_err_t es8311_microphone_gain_set(es8311_handle_t dev, es8311_mic_gain_t gain_db);
esp_err_t es8311_microphone_config(es8311_handle_t dev, bool digital_mic);
esp_err_t es8311_sample_frequency_config(es8311_handle_t dev, int mclk_frequency, int sample_frequency);
esp_err_t es8311_voice_fade(es8311_handle_t dev, const es8311_fade_t fade);
esp_err_t es8311_microphone_fade(es8311_handle_t dev, const es8311_fade_t fade);
void      es8311_register_dump(es8311_handle_t dev);
es8311_handle_t es8311_create(const i2c_port_t port, const uint16_t dev_addr);
void      es8311_delete(es8311_handle_t dev);

#ifdef __cplusplus
}
#endif
