/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modified: I2C calls rewritten to use Arduino Wire instead of legacy
 * ESP-IDF i2c_cmd_link API, to avoid conflicts with the new I2S driver.
 */

#include <inttypes.h>
#include <stdlib.h>
#include "es7210.h"
#include "es7210_reg.h"
#include "esp_log.h"
#include "esp_check.h"
#include "Wire.h"

const static char *TAG = "ES7210";

struct es7210_dev_t {
    i2c_port_t i2c_port;
    uint8_t    i2c_addr;
};

typedef struct {
    uint32_t mclk;
    uint32_t lrck;
    uint8_t  ss_ds;
    uint8_t  adc_div;
    uint8_t  dll;
    uint8_t  doubler;
    uint8_t  osr;
    uint8_t  mclk_src;
    uint32_t lrck_h;
    uint32_t lrck_l;
} coeff_div_t;

static const coeff_div_t es7210_coeff_div[] = {
    {12288000,  8000,  0x00, 0x03, 0x01, 0x00, 0x20, 0x00, 0x06, 0x00},
    {16384000,  8000,  0x00, 0x04, 0x01, 0x00, 0x20, 0x00, 0x08, 0x00},
    {19200000,  8000,  0x00, 0x1e, 0x00, 0x01, 0x28, 0x00, 0x09, 0x60},
    {4096000,   8000,  0x00, 0x01, 0x01, 0x00, 0x20, 0x00, 0x02, 0x00},
    {11289600,  11025, 0x00, 0x02, 0x01, 0x00, 0x20, 0x00, 0x01, 0x00},
    {12288000,  12000, 0x00, 0x02, 0x01, 0x00, 0x20, 0x00, 0x04, 0x00},
    {19200000,  12000, 0x00, 0x14, 0x00, 0x01, 0x28, 0x00, 0x06, 0x40},
    {4096000,   16000, 0x00, 0x01, 0x01, 0x01, 0x20, 0x00, 0x01, 0x00},
    {19200000,  16000, 0x00, 0x0a, 0x00, 0x00, 0x1e, 0x00, 0x04, 0x80},
    {16384000,  16000, 0x00, 0x02, 0x01, 0x00, 0x20, 0x00, 0x04, 0x00},
    {12288000,  16000, 0x00, 0x03, 0x01, 0x01, 0x20, 0x00, 0x03, 0x00},
    {11289600,  22050, 0x00, 0x01, 0x01, 0x00, 0x20, 0x00, 0x02, 0x00},
    {12288000,  24000, 0x00, 0x01, 0x01, 0x00, 0x20, 0x00, 0x02, 0x00},
    {19200000,  24000, 0x00, 0x0a, 0x00, 0x01, 0x28, 0x00, 0x03, 0x20},
    {12288000,  32000, 0x00, 0x03, 0x00, 0x00, 0x20, 0x00, 0x01, 0x80},
    {16384000,  32000, 0x00, 0x01, 0x01, 0x00, 0x20, 0x00, 0x02, 0x00},
    {19200000,  32000, 0x00, 0x05, 0x00, 0x00, 0x1e, 0x00, 0x02, 0x58},
    {11289600,  44100, 0x00, 0x01, 0x01, 0x01, 0x20, 0x00, 0x01, 0x00},
    {12288000,  48000, 0x00, 0x01, 0x01, 0x01, 0x20, 0x00, 0x01, 0x00},
    {19200000,  48000, 0x00, 0x05, 0x00, 0x01, 0x28, 0x00, 0x01, 0x90},
};

static const coeff_div_t *es7210_get_coeff(uint32_t mclk, uint32_t lrck) {
    for (int i = 0; i < (int)(sizeof(es7210_coeff_div) / sizeof(coeff_div_t)); i++) {
        if (es7210_coeff_div[i].lrck == lrck && es7210_coeff_div[i].mclk == mclk)
            return &es7210_coeff_div[i];
    }
    return NULL;
}

// Wire-based I2C write — replaces old i2c_cmd_link API
static esp_err_t es7210_write_reg(es7210_dev_handle_t handle, uint8_t reg_addr, uint8_t reg_val) {
    if (!handle) return ESP_ERR_INVALID_ARG;
    Wire.beginTransmission(handle->i2c_addr);
    Wire.write(reg_addr);
    Wire.write(reg_val);
    uint8_t err = Wire.endTransmission(true);
    if (err) { printf("ES7210 I2C write error: %u\n", err); return ESP_FAIL; }
    return ESP_OK;
}

static esp_err_t es7210_set_i2s_format(es7210_dev_handle_t handle, es7210_i2s_fmt_t fmt,
                                        es7210_i2s_bits_t bits, bool tdm) {
    uint8_t reg_val = 0;
    switch (bits) {
        case ES7210_I2S_BITS_16B: reg_val = 0x60; break;
        case ES7210_I2S_BITS_18B: reg_val = 0x40; break;
        case ES7210_I2S_BITS_20B: reg_val = 0x20; break;
        case ES7210_I2S_BITS_24B: reg_val = 0x00; break;
        case ES7210_I2S_BITS_32B: reg_val = 0x80; break;
        default: return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_SDP_INTERFACE1_REG11, fmt | reg_val), TAG, "I2C error");
    uint8_t tdm_val = 0;
    switch (fmt) {
        case ES7210_I2S_FMT_I2S:
        case ES7210_I2S_FMT_LJ:   tdm_val = 0x02; break;
        case ES7210_I2S_FMT_DSP_A:
        case ES7210_I2S_FMT_DSP_B: tdm_val = 0x01; break;
        default: return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_SDP_INTERFACE2_REG12, tdm ? tdm_val : 0x00), TAG, "I2C error");
    return ESP_OK;
}

static esp_err_t es7210_set_i2s_sample_rate(es7210_dev_handle_t handle, uint32_t sample_rate, uint32_t mclk_ratio) {
    uint32_t mclk = sample_rate * mclk_ratio;
    const coeff_div_t *c = es7210_get_coeff(mclk, sample_rate);
    if (!c) {
        ESP_LOGE(TAG, "Unsupported sample rate %"PRIu32" with mclk %"PRIu32, sample_rate, mclk);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_OSR_REG07, c->osr), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_MAINCLK_REG02,
        c->adc_div | (c->doubler << 6) | (c->dll << 7)), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_LRCK_DIVH_REG04, c->lrck_h), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_LRCK_DIVL_REG05, c->lrck_l), TAG, "I2C error");
    return ESP_OK;
}

esp_err_t es7210_new_codec(const es7210_i2c_config_t *i2c_conf, es7210_dev_handle_t *handle_out) {
    if (!i2c_conf || !handle_out) return ESP_ERR_INVALID_ARG;
    struct es7210_dev_t *h = (struct es7210_dev_t *)calloc(1, sizeof(struct es7210_dev_t));
    if (!h) return ESP_ERR_NO_MEM;
    h->i2c_port = i2c_conf->i2c_port;
    h->i2c_addr = i2c_conf->i2c_addr;
    *handle_out = h;
    return ESP_OK;
}

esp_err_t es7210_del_codec(es7210_dev_handle_t handle) {
    if (!handle) return ESP_ERR_INVALID_ARG;
    free(handle);
    return ESP_OK;
}

esp_err_t es7210_config_codec(es7210_dev_handle_t handle, const es7210_codec_config_t *conf) {
    if (!handle || !conf) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_RESET_REG00, 0xFF), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_RESET_REG00, 0x32), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_TIME_CONTROL0_REG09, 0x30), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_TIME_CONTROL1_REG0A, 0x30), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_ADC12_HPF1_REG23, 0x2A), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_ADC12_HPF2_REG22, 0x0A), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_ADC34_HPF1_REG21, 0x2A), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_ADC34_HPF2_REG20, 0x0A), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_set_i2s_format(handle, conf->i2s_format, conf->bit_width,
        conf->flags.tdm_enable), TAG, "I2S format error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_ANALOG_REG40, 0xC3), TAG, "I2C error");
    // Mic bias
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_MIC12_BIAS_REG41, conf->mic_bias), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_MIC34_BIAS_REG42, conf->mic_bias), TAG, "I2C error");
    // Mic gain
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_MIC1_GAIN_REG43, conf->mic_gain | 0x10), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_MIC2_GAIN_REG44, conf->mic_gain | 0x10), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_MIC3_GAIN_REG45, conf->mic_gain | 0x10), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_MIC4_GAIN_REG46, conf->mic_gain | 0x10), TAG, "I2C error");
    // Power on MICs
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_MIC1_POWER_REG47, 0x08), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_MIC2_POWER_REG48, 0x08), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_MIC3_POWER_REG49, 0x08), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_MIC4_POWER_REG4A, 0x08), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_set_i2s_sample_rate(handle, conf->sample_rate_hz, conf->mclk_ratio), TAG, "sample rate error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_POWER_DOWN_REG06, 0x04), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_MIC12_POWER_REG4B, 0x0F), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_MIC34_POWER_REG4C, 0x0F), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_RESET_REG00, 0x71), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_RESET_REG00, 0x41), TAG, "I2C error");
    printf("ES7210 initialized.\n");
    return ESP_OK;
}

esp_err_t es7210_config_volume(es7210_dev_handle_t handle, int8_t volume_db) {
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (volume_db < -95 || volume_db > 32) return ESP_ERR_INVALID_ARG;
    uint8_t reg_val = 191 + volume_db * 2;
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_ADC1_DIRECT_DB_REG1B, reg_val), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_ADC2_DIRECT_DB_REG1C, reg_val), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_ADC3_DIRECT_DB_REG1D, reg_val), TAG, "I2C error");
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_ADC4_DIRECT_DB_REG1E, reg_val), TAG, "I2C error");
    return ESP_OK;
}

esp_err_t es7210_reset(es7210_dev_handle_t handle) {
    if (!handle) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_RESET_REG00, 0xFF), TAG, "I2C error");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(es7210_write_reg(handle, ES7210_RESET_REG00, 0x32), TAG, "I2C error");
    return ESP_OK;
}
