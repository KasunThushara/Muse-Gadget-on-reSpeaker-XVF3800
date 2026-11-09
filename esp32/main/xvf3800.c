/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "xvf3800.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "link.xvf3800";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static i2c_master_dev_handle_t s_codec;
static SemaphoreHandle_t s_lock;
static bool s_ready;

// Largest payload this driver moves: 12 LEDs x 4 bytes for LED_RING_COLOR.
#define XVF3800_MAX_WRITE 64
#define XVF3800_MAX_READ  32

esp_err_t xvf3800_init(void) {
    if (s_ready) return ESP_OK;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_ready) {
        i2c_master_bus_config_t bus = {
            .i2c_port = 0,
            .sda_io_num = 5,
            .scl_io_num = 6,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t err = i2c_new_master_bus(&bus, &s_bus);
        if (err == ESP_OK) {
            i2c_device_config_t dev = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = XVF3800_I2C_ADDR,
                .scl_speed_hz = 100000,
            };
            err = i2c_master_bus_add_device(s_bus, &dev, &s_dev);
        }
        if (err == ESP_OK) {
            // The AIC3104 codec is on the same bus. It is optional: a failure
            // here only means the output stages cannot be configured directly.
            i2c_device_config_t codec = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = 0x18,
                .scl_speed_hz = 100000,
            };
            (void)i2c_master_bus_add_device(s_bus, &codec, &s_codec);
        }
        if (err == ESP_OK) {
            s_ready = true;
        } else {
            ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_lock);
    return s_ready ? ESP_OK : ESP_FAIL;
}

esp_err_t xvf3800_write(uint8_t resid, uint8_t cmd, const uint8_t *data, size_t len) {
    if (!s_ready || len > XVF3800_MAX_WRITE - 3) return ESP_ERR_INVALID_STATE;
    uint8_t buf[XVF3800_MAX_WRITE];
    buf[0] = resid;
    buf[1] = cmd;
    buf[2] = (uint8_t)len;
    if (len) memcpy(buf + 3, data, len);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = i2c_master_transmit(s_dev, buf, 3 + len, 100);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) ESP_LOGW(TAG, "write %u/%u: %s", resid, cmd, esp_err_to_name(err));
    return err;
}

esp_err_t xvf3800_read(uint8_t resid, uint8_t cmd, uint8_t *out, size_t len) {
    if (!s_ready || !out || len == 0 || len + 1 > XVF3800_MAX_READ) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t req[3] = {resid, (uint8_t)(cmd | 0x80), (uint8_t)(len + 1)};
    uint8_t resp[XVF3800_MAX_READ];

    // The chip answers 64 while it is not ready, so give it a few tries.
    for (int attempt = 0; attempt < 3; attempt++) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        esp_err_t err = i2c_master_transmit(s_dev, req, sizeof(req), 100);
        if (err == ESP_OK) err = i2c_master_receive(s_dev, resp, len + 1, 100);
        xSemaphoreGive(s_lock);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "read %u/%u: %s", resid, cmd, esp_err_to_name(err));
            return err;
        }
        if (resp[0] == 0) {
            memcpy(out, resp + 1, len);
            return ESP_OK;
        }
        if (resp[0] != 64) {
            ESP_LOGW(TAG, "read %u/%u: status %u", resid, cmd, resp[0]);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t xvf3800_version(uint8_t out[3]) {
    return xvf3800_read(XVF3800_RES_APP, 0, out, 3);
}

bool xvf3800_muted(void) {
    // X1D09 is the first of the three GPIs; it is high when the button is out.
    uint8_t gpi[3] = {0};
    if (xvf3800_read(XVF3800_RES_IO, 0, gpi, sizeof(gpi)) != ESP_OK) return true;
    return gpi[0] == 0;
}

esp_err_t xvf3800_amp_enable(bool on) {
    // X0D31 is active low.
    uint8_t payload[2] = {XVF3800_GPO_AMP, (uint8_t)(on ? 0 : 1)};
    return xvf3800_write(XVF3800_RES_GPO, 1, payload, sizeof(payload));
}

esp_err_t xvf3800_ring_power(bool on) {
    uint8_t payload[2] = {XVF3800_GPO_LED_PWR, (uint8_t)(on ? 1 : 0)};
    return xvf3800_write(XVF3800_RES_GPO, 1, payload, sizeof(payload));
}

esp_err_t xvf3800_led_effect(uint8_t effect) {
    return xvf3800_write(XVF3800_RES_GPO, 12, &effect, 1);
}

esp_err_t xvf3800_led_brightness(uint8_t brightness) {
    return xvf3800_write(XVF3800_RES_GPO, 13, &brightness, 1);
}

esp_err_t xvf3800_led_colour(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t rgb[4] = {r, g, b, 0};
    return xvf3800_write(XVF3800_RES_GPO, 16, rgb, sizeof(rgb));
}

esp_err_t xvf3800_led_ring(const uint8_t *rgb, size_t leds) {
    if (!rgb || leds > XVF3800_LED_COUNT) return ESP_ERR_INVALID_ARG;
    uint8_t buf[XVF3800_LED_COUNT * 4];
    for (size_t i = 0; i < leds; i++) {
        buf[4 * i] = rgb[3 * i];
        buf[4 * i + 1] = rgb[3 * i + 1];
        buf[4 * i + 2] = rgb[3 * i + 2];
        buf[4 * i + 3] = 0;
    }
    return xvf3800_write(XVF3800_RES_GPO, 19, buf, leds * 4);
}

esp_err_t xvf3800_volume(uint8_t level) {
    if (level > 9) level = 9;
    esp_err_t err = xvf3800_write(XVF3800_RES_APP, 11, &level, 1);  // headphones
    if (err == ESP_OK) err = xvf3800_write(XVF3800_RES_APP, 12, &level, 1);  // line out
    return err;
}

esp_err_t xvf3800_codec_write(uint8_t reg, uint8_t val) {
    if (!s_ready || !s_codec) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = {reg, val};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = i2c_master_transmit(s_codec, buf, sizeof(buf), 100);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "codec 0x%02x=0x%02x: %s", reg, val, esp_err_to_name(err));
    }
    return err;
}

esp_err_t xvf3800_codec_init(void) {
    // Seeed's I2C playback example: DAC at 0 dB, and both the headphone and
    // line outputs powered and unmuted. Without this the JST speaker stays
    // silent even though the I2S stream is correct.
    static const uint8_t init[][2] = {
        {0x00, 0x00},  // page 0
        {0x2B, 0x00},  // left DAC volume: 0 dB
        {0x2C, 0x00},  // right DAC volume: 0 dB
        {0x33, 0x0D},  // HPLOUT: 0 dB, unmuted, powered
        {0x41, 0x0D},  // HPROUT: the same
        {0x56, 0x0B},  // left line out: 0 dB, unmuted, powered
        {0x5D, 0x0B},  // right line out: the same
    };
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(init) / sizeof(init[0]) && err == ESP_OK; i++) {
        err = xvf3800_codec_write(init[i][0], init[i][1]);
    }
    return err;
}
