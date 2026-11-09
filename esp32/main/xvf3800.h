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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Seeed reSpeaker XVF3800 control over I2C.
//
// The XVF3800 is the I2C slave at 0x2C. Its frame format is
//   write: [resid] [cmd] [len] [data...]
//   read:  write [resid] [cmd | 0x80] [len + 1], then read [status] [data...]
// where status 0 is success and 64 asks for a retry. This module owns the one
// I2C master bus (SDA=5, SCL=6) so the audio driver and the LED backend can
// share it.

#define XVF3800_I2C_ADDR    0x2C
#define XVF3800_LED_COUNT   12

// Servicers.
#define XVF3800_RES_GPO     20
#define XVF3800_RES_IO      36
#define XVF3800_RES_APP     48

// GPO pins on the XVF3800.
#define XVF3800_GPO_MUTE    30   // high = mute the mics and light the mute LED
#define XVF3800_GPO_AMP     31   // low = amplifier enabled
#define XVF3800_GPO_LED_PWR 33   // high = WS2812 ring powered

// LED effects.
#define XVF3800_LED_OFF       0
#define XVF3800_LED_BREATHING 1
#define XVF3800_LED_RAINBOW   2
#define XVF3800_LED_SOLID     3
#define XVF3800_LED_DOA       4
#define XVF3800_LED_RING      5

// Idempotent: creates the I2C bus and the 0x2C device on first call.
esp_err_t xvf3800_init(void);

// Raw servicer access. `read` returns `len` payload bytes (the status byte is
// consumed) and retries when the chip answers 64.
esp_err_t xvf3800_write(uint8_t resid, uint8_t cmd, const uint8_t *data, size_t len);
esp_err_t xvf3800_read(uint8_t resid, uint8_t cmd, uint8_t *out, size_t len);

// True when the hardware mute is on or the state cannot be read (fails closed).
bool xvf3800_muted(void);

// Amplifier enable (on = audible) and WS2812 ring power.
esp_err_t xvf3800_amp_enable(bool on);
esp_err_t xvf3800_ring_power(bool on);

// LED ring. Colour is 8-bit R/G/B; brightness is 0..255.
esp_err_t xvf3800_led_effect(uint8_t effect);
esp_err_t xvf3800_led_colour(uint8_t r, uint8_t g, uint8_t b);
esp_err_t xvf3800_led_brightness(uint8_t brightness);
// Per-LED colours, 3 bytes per LED, `leds` <= 12.
esp_err_t xvf3800_led_ring(const uint8_t *rgb, size_t leds);

// AIC3104 headphone and line-out level, 0..9.
esp_err_t xvf3800_volume(uint8_t level);

// The AIC3104 codec sits on the same I2C bus at 0x18. Its output stages are
// powered down after reset, so a board that plays audio has to bring them up.
esp_err_t xvf3800_codec_write(uint8_t reg, uint8_t val);
esp_err_t xvf3800_codec_init(void);

// Firmware version: MAJOR, MINOR, PATCH.
esp_err_t xvf3800_version(uint8_t out[3]);
