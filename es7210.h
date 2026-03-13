/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c.h"

#define ES7210_ADDRRES_00 (0x40)
#define ES7210_ADDRESS_01 (0x41)
#define ES7210_ADDRESS_10 (0x42)
#define ES7210_ADDRESS_11 (0x43)

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ES7210_I2S_FMT_I2S   = 0x00,
    ES7210_I2S_FMT_LJ    = 0x01,
    ES7210_I2S_FMT_DSP_A = 0x03,
    ES7210_I2S_FMT_DSP_B = 0x13
} es7210_i2s_fmt_t;

typedef enum {
    ES7210_I2S_BITS_16B = 16,
    ES7210_I2S_BITS_18B = 18,
    ES7210_I2S_BITS_20B = 20,
    ES7210_I2S_BITS_24B = 24,
    ES7210_I2S_BITS_32B = 32
} es7210_i2s_bits_t;

typedef enum {
    ES7210_MIC_GAIN_0DB    = 0,
    ES7210_MIC_GAIN_3DB    = 1,
    ES7210_MIC_GAIN_6DB    = 2,
    ES7210_MIC_GAIN_9DB    = 3,
    ES7210_MIC_GAIN_12DB   = 4,
    ES7210_MIC_GAIN_15DB   = 5,
    ES7210_MIC_GAIN_18DB   = 6,
    ES7210_MIC_GAIN_21DB   = 7,
    ES7210_MIC_GAIN_24DB   = 8,
    ES7210_MIC_GAIN_27DB   = 9,
    ES7210_MIC_GAIN_30DB   = 10,
    ES7210_MIC_GAIN_33DB   = 11,
    ES7210_MIC_GAIN_34_5DB = 12,
    ES7210_MIC_GAIN_36DB   = 13,
    ES7210_MIC_GAIN_37_5DB = 14
} es7210_mic_gain_t;

typedef enum {
    ES7210_MIC_BIAS_2V18 = 0x00,
    ES7210_MIC_BIAS_2V26 = 0x10,
    ES7210_MIC_BIAS_2V36 = 0x20,
    ES7210_MIC_BIAS_2V45 = 0x30,
    ES7210_MIC_BIAS_2V55 = 0x40,
    ES7210_MIC_BIAS_2V66 = 0x50,
    ES7210_MIC_BIAS_2V78 = 0x60,
    ES7210_MIC_BIAS_2V87 = 0x70
} es7210_mic_bias_t;

typedef struct es7210_dev_t *es7210_dev_handle_t;

typedef struct {
    i2c_port_t i2c_port;
    uint8_t    i2c_addr;
} es7210_i2c_config_t;

typedef struct {
    uint32_t          sample_rate_hz;
    uint32_t          mclk_ratio;
    es7210_i2s_fmt_t  i2s_format;
    es7210_i2s_bits_t bit_width;
    es7210_mic_bias_t mic_bias;
    es7210_mic_gain_t mic_gain;
    struct {
        uint32_t tdm_enable: 1;
    } flags;
} es7210_codec_config_t;

esp_err_t es7210_new_codec(const es7210_i2c_config_t *i2c_conf, es7210_dev_handle_t *handle_out);
esp_err_t es7210_del_codec(es7210_dev_handle_t handle);
esp_err_t es7210_config_codec(es7210_dev_handle_t handle, const es7210_codec_config_t *codec_conf);
esp_err_t es7210_config_volume(es7210_dev_handle_t handle, int8_t volume_db);
esp_err_t es7210_reset(es7210_dev_handle_t handle);

#ifdef __cplusplus
}
#endif
