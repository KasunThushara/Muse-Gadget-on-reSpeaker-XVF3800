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

/* Seeed reSpeaker XVF3800 with the XIAO ESP32-S3.
 * Hardware and pin references:
 *   https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_getting_started/
 *   https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_record_playback/
 *   https://wiki.seeedstudio.com/respeaker_xvf_3800_i2c_list/
 *   https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_gpio/
 * The XVF3800 runs the 16 kHz I2S firmware as the I2S slave, so the ESP32-S3
 * is the I2S master and generates BCLK/WS. Its NS/AEC/AGC are left as shipped;
 * this file only moves audio and does the small amount of control Muse needs
 * (mute state, amplifier, volume).
 */
#include "voice_board.h"

#include "xvf3800.h"

#include <math.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Temporary bring-up aid: play a 1 kHz tone for one second at boot so the
// speaker path can be checked without the network or Muse. Set to 0 when done.
#define XVF3800_TONE_TEST 0

#define CHUNK 320
static const char *TAG = "link.xvf3800";
static i2s_chan_handle_t s_rx, s_tx;
static int32_t s_raw[CHUNK * 2];
static bool s_mic_on;

void voice_board_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    // The AIC3104 exposes ten levels (0..9) through the application servicer.
    uint8_t level = (uint8_t)((percent * 9 + 50) / 100);
    esp_err_t err = xvf3800_volume(level);
    if (err != ESP_OK) ESP_LOGW(TAG, "volume: %s", esp_err_to_name(err));
}

// The XVF3800 runs AEC on the channel this board reads, so the microphone can
// stay open while the speaker plays.
bool voice_board_echo_cancelled(void) {
    return true;
}

bool voice_board_muted(void) {
    return xvf3800_muted();
}

int voice_board_dial_steps(void) { return 0; }

void voice_board_amp(bool on) {
    esp_err_t err = xvf3800_amp_enable(on);
    if (on && err == ESP_OK) {
        // The GPO servicer can take a moment to apply the write.
        vTaskDelay(pdMS_TO_TICKS(20));
        // X0D11, X0D30 (mute), X0D31 (amp), X0D33 (ring power), X0D39.
        uint8_t gpo[5] = {0};
        if (xvf3800_read(XVF3800_RES_GPO, 0, gpo, sizeof(gpo)) == ESP_OK) {
            ESP_LOGI(TAG, "amp on: X0D30=%u X0D31=%u X0D33=%u",
                     gpo[1], gpo[2], gpo[3]);
        }
    }
}

esp_err_t voice_board_mic_start(void) {
    if (!s_rx) return ESP_ERR_INVALID_STATE;
    esp_err_t err = i2s_channel_enable(s_rx);
    if (err == ESP_OK) s_mic_on = true;
    return err;
}

void voice_board_mic_stop(void) {
    if (s_mic_on) {
        i2s_channel_disable(s_rx);
        s_mic_on = false;
    }
}

size_t voice_board_mic_read(int16_t *pcm, size_t frames, int *peak) {
    *peak = 0;
    if (!s_mic_on) return 0;
    if (frames > CHUNK) frames = CHUNK;
    size_t bytes = 0;
    if (i2s_channel_read(s_rx, s_raw, frames * 2 * sizeof(int32_t), &bytes, 500) != ESP_OK) {
        return 0;
    }
    size_t count = bytes / (2 * sizeof(int32_t));
    for (size_t i = 0; i < count; i++) {
        // Left is the XVF3800's processed channel. Swap to s_raw[2*i+1] if the
        // test recordings come out silent.
        pcm[i] = (int16_t)(s_raw[2 * i] >> 16);
        int magnitude = pcm[i] < 0 ? -(int)pcm[i] : pcm[i];
        if (magnitude > *peak) *peak = magnitude;
    }
    return count;
}

esp_err_t voice_board_speaker_write(const int32_t *frames, size_t count) {
    // The player produces 48 kHz stereo. Average each three frames down to the
    // XVF3800's 16 kHz bus; player chunks are always a multiple of three.
    if (!s_tx) return ESP_ERR_INVALID_STATE;
    if (count % 3) return ESP_ERR_INVALID_SIZE;
    // Keep the player's 3 KB task stack free for I2S and logging.
    int32_t out[32 * 2];
    while (count) {
        size_t n = count / 3;
        if (n > 32) n = 32;
        for (size_t i = 0; i < n; i++) {
            for (size_t c = 0; c < 2; c++) {
                int64_t sum = (int64_t)frames[6 * i + c] + frames[6 * i + 2 + c] +
                              frames[6 * i + 4 + c];
                out[2 * i + c] = (int32_t)(sum / 3);
            }
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, out, n * 2 * sizeof(int32_t), &written, 1000);
        if (err != ESP_OK) return err;
        if (written != n * 2 * sizeof(int32_t)) return ESP_ERR_TIMEOUT;
        frames += 6 * n;
        count -= 3 * n;
    }
    return ESP_OK;
}

#if XVF3800_TONE_TEST
static void tone_test(void) {
    static int32_t buf[CHUNK * 2];
    for (int i = 0; i < CHUNK; i++) {
        int32_t s = (int32_t)(sinf(2.0f * 3.14159265f * 1000.0f * i / VOICE_MIC_RATE) *
                              0.25f * 2147483647.0f);
        buf[2 * i] = s;
        buf[2 * i + 1] = s;
    }
    for (int n = 0; n < 50; n++) {   // 50 x 20 ms = 1 s
        size_t written = 0;
        if (i2s_channel_write(s_tx, buf, sizeof(buf), &written, 1000) != ESP_OK) break;
    }
}
#endif

esp_err_t voice_board_init(void) {
    esp_err_t err = xvf3800_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "XVF3800 I2C init failed: %s", esp_err_to_name(err));
        return err;
    }

    // The AIC3104's output stages are down after reset; bring them up or the
    // JST speaker stays silent.
    esp_err_t codec_err = xvf3800_codec_init();
    if (codec_err == ESP_OK) {
        ESP_LOGI(TAG, "AIC3104 outputs powered (DAC 0 dB, HP + line out)");
    } else {
        ESP_LOGW(TAG, "AIC3104 init: %s", esp_err_to_name(codec_err));
    }

    uint8_t version[3];
    if (xvf3800_version(version) == ESP_OK) {
        ESP_LOGI(TAG, "XVF3800 firmware %u.%u.%u; 16 kHz I2S slave, ESP32 is master",
                 version[0], version[1], version[2]);
    } else {
        ESP_LOGW(TAG, "XVF3800 did not answer version; check the I2S firmware");
    }
    (void)xvf3800_amp_enable(false);

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 6;
    chan.dma_frame_num = CHUNK;
    chan.auto_clear = true;
    err = i2s_new_channel(&chan, &s_tx, &s_rx);
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_MIC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = 8,
            .ws = 7,
            // The XVF3800 uses the opposite data-pin order from the reSpeaker Lite.
            .dout = 44,
            .din = 43,
        },
    };
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_tx, &cfg);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_rx, &cfg);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(err));
        return err;
    }

#if XVF3800_TONE_TEST
    // Bring-up aid: enable the JST amplifier, check the GPO really went low,
    // then play one second of 1 kHz so the speaker path can be heard.
    (void)xvf3800_amp_enable(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    uint8_t gpo[5] = {0};
    if (xvf3800_read(XVF3800_RES_GPO, 0, gpo, sizeof(gpo)) == ESP_OK) {
        ESP_LOGI(TAG, "tone test GPO: X0D30=%u X0D31=%u X0D33=%u",
                 gpo[1], gpo[2], gpo[3]);
    } else {
        ESP_LOGW(TAG, "tone test: could not read GPO");
    }
    tone_test();
    (void)xvf3800_amp_enable(false);
#endif

    voice_board_set_volume(60);
    ESP_LOGI(TAG, "reSpeaker XVF3800 audio ready; XIAO BOOT is push-to-talk");
    return ESP_OK;
}
