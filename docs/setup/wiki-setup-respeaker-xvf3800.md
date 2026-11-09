# Muse Gadget on reSpeaker XVF3800 + XIAO ESP32-S3

A complete setup guide: keys, phone app, prerequisites, XMOS I2S firmware,
hardware preparation, build, flash, and pairing.

> Built for this workspace's `seeed-respeaker-xvf3800` port. The port is **not
> upstream** in `facebookincubator/muse-gadget-sdk` (nor in the Seeed fork), so
> see [Clone the project](#4-clone-the-project) for the files it adds.

---

## Quick checklist

- [ ] Muse **SDK token** (`mgst_…`)
- [ ] Optional **AssemblyAI** key for spoken replies
- [ ] **Muse app** on the phone, Developer mode on
- [ ] **ESP-IDF v6.0.3** installed
- [ ] XVF3800 on the **I2S slave 16 kHz** firmware
- [ ] JST **5 W speaker** (or 3.5 mm headphones)
- [ ] 2.4 GHz Wi-Fi credentials
- [ ] USB-C **data** cable

---

## 1. What you are building

A Muse gadget using the Seeed **reSpeaker XVF3800** 4-mic array with a **XIAO
ESP32-S3**. It pairs with the Muse phone app over BLE, joins Wi-Fi, and holds an
encrypted Noise session to your Muse VM.

- **Hold** the button: record a voice note. Muse transcribes it and replies; the
  reply is shown in the app and spoken on the speaker via TTS.
- **Tap** (under 1 s): start a live voice call (if `MUSE_REALTIME_VOICE` is on).
- **Tap** again: hang up.

Architecture at a glance:

```
XIAO ESP32-S3 ──BLE──> Muse app (pairing)
      │
      └──Wi-Fi──> encrypted Noise session ──> Muse VM
                        │
                        ├── /link-control : device.health, voice.configure, …
                        └── /chat/stream  : voice note → transcript → reply
```

Audio/control wiring:

| Signal | XIAO GPIO |
|---|---|
| I2S BCLK | 8 |
| I2S WS / LRCLK | 7 |
| I2S data out (host → XVF3800) | 44 |
| I2S data in (XVF3800 → host) | 43 |
| I2C SDA | 5 |
| I2C SCL | 6 |
| XMOS I2C address | `0x2C` |
| AIC3104 codec address | `0x18` |

The XIAO is the **I2S master**; the XVF3800 runs the **I2S slave 16 kHz**
firmware and owns its 12-LED ring and amplifier.

---

## 2. Accounts and API keys

| Key / credential | Required? | Where to get it | Where it goes |
|---|---|---|---|
| **Muse SDK token** (`mgst_…`, 48 chars) | **Yes** | <https://gadgets.muse.ai/settings/sdk-tokens> | `CONFIG_GADGET_SDK_TOKEN` in the build `sdkconfig` |
| **AssemblyAI key** (32 chars) | Optional — spoken replies | <https://www.assemblyai.com/dashboard> | `CONFIG_MUSE_TTS_API_KEY` in the build `sdkconfig` |
| **Groq key** (`gsk_…`) | Alternative TTS | <https://console.groq.com/keys> | Same, with the Groq URL/model |
| **Wi-Fi SSID + password** | Yes | Your 2.4 GHz network/hotspot | Muse app during pairing |
| Secure Boot / flash encryption key | **No** | — | Leave off; do not burn eFuses |
| NVS encryption key | **No** | Auto-derived from eFuse | Optional `CONFIG_HOMEHUB_NVS_ENCRYPTION` |
| Device / access token | **No** | Minted during pairing | NVS automatically |
| Manufacturer attestation key | **No** | — | Community pairing only |

**Never commit keys.**

- `esp32/build-*/sdkconfig` is gitignored — put the keys there.
- Do **not** put keys in `esp32/devices/sdkconfig.*` (tracked).
- Do not paste keys into chat/issues.

### TTS provider settings

Current default (AssemblyAI):

```ini
CONFIG_MUSE_TTS_ENABLE=y
CONFIG_MUSE_TTS_URL="https://tts.us.assemblyai.com/v1/openai/audio/speech"
CONFIG_MUSE_TTS_MODEL="assemblyai-tts-en"
CONFIG_MUSE_TTS_VOICE="alloy"
CONFIG_MUSE_TTS_RESPONSE_FORMAT="mp3"
CONFIG_MUSE_TTS_MAX_CHARS=800
CONFIG_MUSE_TTS_TIMEOUT_MS=15000
CONFIG_MUSE_TTS_API_KEY="YOUR_ASSEMBLYAI_KEY"
```

Groq alternative (Orpheus, 200-char input limit):

```ini
CONFIG_MUSE_TTS_URL="https://api.groq.com/openai/v1/audio/speech"
CONFIG_MUSE_TTS_MODEL="canopylabs/orpheus-v1-english"
CONFIG_MUSE_TTS_VOICE="hannah"
CONFIG_MUSE_TTS_RESPONSE_FORMAT="wav"
CONFIG_MUSE_TTS_MAX_CHARS=200
CONFIG_MUSE_TTS_API_KEY="gsk_..."
```

The phone app has **no TTS provider setting**; TTS is entirely firmware-side.

---

## 3. Phone app

- Install the **Muse** app (Meta) on Android or iOS.
- Sign in with your Muse account.
- Turn on **Settings → Devices → Developer mode**.
- Muse launched in the **US and Canada**; the account/app is required for
  pairing and for showing the reply text.
- The app shows replies as **text**; the gadget speaks them if TTS is configured.

---

## 4. Clone the project

```sh
git clone https://github.com/facebookincubator/muse-gadget-sdk.git
cd muse-gadget-sdk
```

> **The XVF3800 port is local to this workspace, not upstream.** A fresh clone
> does not contain it. Copy/apply these files and changes, or commit the port to
> your own branch/fork and clone that.

Files added:

```
esp32/devices/sdkconfig.seeed-respeaker-xvf3800
esp32/main/voice_board_xvf3800.c
esp32/main/xvf3800.c
esp32/main/xvf3800.h
```

Files changed:

```
esp32/main/Kconfig.projbuild      (HOMEHUB_LED_BACKEND_XVF3800, button options)
esp32/main/CMakeLists.txt         (xvf3800.c, voice_board_xvf3800.c)
esp32/main/led_status.c           (XVF3800 ring over I2C)
esp32/main/button.c               (active-high button, optional power GPIO)
esp32/main/voice.c                (tap-vs-hold measurement)
esp32/tools/board.sh              (seeed-respeaker-xvf3800 case)
esp32/components/muse/Kconfig     (MUSE_TTS_*)
esp32/components/muse/muse_chat_session.cpp  (start_tts / WAV / MP3)
```

---

## 5. Software prerequisites

- **ESP-IDF v6.0.3** — required by `esp32/cmake/validate_config.cmake`:

  ```sh
  git clone -b v6.0.3 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6.0.3
  ~/esp/esp-idf-v6.0.3/install.sh esp32s3
  . ~/esp/esp-idf-v6.0.3/export.sh
  ```

  v6.0.2 also compiles, but only with
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=n`. That workaround is
  **not for production** (earlier IDF releases leak heap on every TLS handshake
  with that option on).

- **Git**, **Python 3**, a **USB-C data cable**.
- Windows:
  - The XIAO console is native USB (`303a:1001`) and appears as `COMx`.
  - `tools/board.sh` is bash; run it in **Git Bash** or use `idf.py` directly.
- **dfu-util** for the XMOS firmware:
  - Windows: download `dfu-util-0.11-binaries`, add `dfu-util.exe` to `Path`,
    and use **Zadig** to install **WinUSB** if `dfu-util -l` cannot open the
    device.
  - macOS: `brew install dfu-util`
  - Linux: `sudo apt install dfu-util` and add the udev rule:
    ```
    SUBSYSTEM=="usb", ATTRS{idVendor}=="2886", MODE="0666", GROUP="plugdev"
    ```
- Optional: `xvf_host` / **reSpeaker Console** for XMOS checks.

---

## 6. XMOS I2S firmware (I2S slave, 16 kHz)

The XIAO is the **I2S master**. The XVF3800 must run the **I2S slave 16 kHz**
firmware; the USB firmware does **not** work with the XIAO.

### 6.1 Enter Safe Mode

1. Power the device off completely.
2. Press and **hold the Mute button**.
3. Reconnect power while holding it.
4. The red LED blinks — Safe Mode is active (supports USB and I2C DFU).

### 6.2 Get the firmware

```sh
git clone https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY.git
```

Do **not** use GitHub's "Save as" on individual files (they get corrupted).
Clone the repo or download the ZIP.

The image for this build:

```
xmos_firmwares/i2s/respeaker_xvf3800_i2s_slave_v1.0.9_16k.bin
```

### 6.3 Flash it

Use the **XMOS USB-C port** (the one near the 3.5 mm jack), not the XIAO's port.
Follow the vendor guide:

- <https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY/blob/master/xmos_firmwares/dfu_guide.md>
- <https://wiki.seeedstudio.com/respeaker_xvf3800_introduction/#update-firmware>
- <https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_getting_started/>

Generic DFU command (for images that carry the DFU suffix):

```sh
dfu-util -R -e -a 1 -D /path/to/respeaker_xvf3800_i2s_slave_v1.0.9_16k.bin
```

> The I2S firmware does not support USB DFU in normal mode; Safe Mode does. If
> the image requires I2C DFU, use the vendor's `xvf_i2c_dfu` / reSpeaker Console
> as described in the repo.

### 6.4 Verify

The Muse firmware logs:

```
link.xvf3800: XVF3800 firmware 1.0.9; 16 kHz I2S slave, ESP32 is master
```

or run:

```sh
xvf_host VERSION
```

---

## 7. Prepare the XVF3800 (66 mm array)

- The four mics sit on a **66 mm array** (coordinates ±33 mm). Mount it with the
  **mic inlet / Seeed logo side facing the room**; keep the array unobstructed.
- Enclosure and mechanical files:
  - [2D mechanical drawing](https://files.seeedstudio.com/wiki/respeaker_xvf3800_usb/respeaker_xvf3800_2d_mechanical_drawing.pdf)
  - [3D model](https://files.seeedstudio.com/wiki/respeaker_xvf3800_usb/3d/respeaker_mic_array_xvf3800_1_with-xiao-0820.stp)
  - [Enclosure upper](https://files.seeedstudio.com/wiki/respeaker_xvf3800_usb/3d/1-up.stp) /
    [lower](https://files.seeedstudio.com/wiki/respeaker_xvf3800_usb/3d/1-down.stp)
- **Speaker:** JST **5 W** speaker (or 3.5 mm headphones). The amplifier is
  enabled by XVF3800 **X0D31 low**; the firmware drives it.
- **Antenna:** attach the XIAO antenna; use **2.4 GHz** Wi-Fi.
- **Mute / reset:** the XVF3800's own Mute and Reset buttons remain hardware
  controls.

### Optional external button (Grove Button)

The user's wiring: **VCC → D3, SIG → D0, GND → GND**.

| Grove Button | XIAO | GPIO |
|---|---|---|
| GND | GND | — |
| VCC | **D3** | 4 (driven high) |
| SIG | **D0** | 1 (input, active high) |
| NC | — | — |

Firmware settings (already in the overlay):

```ini
CONFIG_HOMEHUB_BUTTON_GPIO=1
CONFIG_HOMEHUB_BUTTON_POWER_GPIO=4
CONFIG_HOMEHUB_BUTTON_ACTIVE_HIGH=y
CONFIG_HOMEHUB_BUTTON_LABEL="TALK"
```

The Grove Button is **active high**; the firmware drives GPIO4 high to power it
and reads GPIO1 with a pull-down.

---

## 8. Build

```sh
cd esp32
```

### 8.1 Set the keys

```sh
idf.py -B build-seeed-respeaker-xvf3800 menuconfig
```

Set under **ESP32 Device SDK**:

- **Muse Gadgets SDK token** → `mgst_…`
- TTS options (under the **Muse** menu) → provider, URL, model, voice, API key

Or edit `build-seeed-respeaker-xvf3800/sdkconfig` directly:

```ini
CONFIG_GADGET_SDK_TOKEN="mgst_…"
CONFIG_MUSE_TTS_ENABLE=y
CONFIG_MUSE_TTS_URL="https://tts.us.assemblyai.com/v1/openai/audio/speech"
CONFIG_MUSE_TTS_MODEL="assemblyai-tts-en"
CONFIG_MUSE_TTS_VOICE="alloy"
CONFIG_MUSE_TTS_RESPONSE_FORMAT="mp3"
CONFIG_MUSE_TTS_MAX_CHARS=800
CONFIG_MUSE_TTS_API_KEY="YOUR_ASSEMBLYAI_KEY"
```

> The generated `build-*/sdkconfig` overrides the overlay. After changing
> `devices/sdkconfig.seeed-respeaker-xvf3800`, update or delete the generated
> `sdkconfig`.

### 8.2 Compile

```sh
tools/board.sh seeed-respeaker-xvf3800 build
```

or directly:

```sh
idf.py -B build-seeed-respeaker-xvf3800 \
  -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-seeed-respeaker-xvf3800/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.seeed-respeaker-xvf3800" \
  build
```

The image lands at:

```
esp32/build-seeed-respeaker-xvf3800/muse-gadget.bin
```

---

## 9. Flash

```sh
tools/board.sh seeed-respeaker-xvf3800 flash COM10
```

or directly:

```sh
idf.py -B build-seeed-respeaker-xvf3800 \
  -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-seeed-respeaker-xvf3800/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.seeed-respeaker-xvf3800" \
  -p COM10 flash
```

- Replace `COM10` with the XIAO's port (`ls /dev/cu.usbmodem*` on macOS,
  `/dev/ttyACM*` on Linux).
- If flashing cannot connect: hold **BOOT**, tap **RESET**, release **BOOT**,
  retry.
- Close any serial monitor that holds the port.
- Reflashing keeps the pairing/Wi-Fi; `erase-flash` wipes everything including
  NVS and pairing.

---

## 10. Pair and test

1. Muse app → **Settings → Devices → Developer mode ON**.
2. **Add Device (+)** → `MuseGadget-respeaker-XXXXXX`.
3. **Press** the button (or the Grove Button) to confirm.
4. Choose the **2.4 GHz** Wi-Fi; enter the password.
5. Wait for the **green ring**.
6. **Hold** the button, speak, release:
   - `link.voice: recorded X.Xs`
   - `muse_chat_session: TTS: N chars`
   - `link.voice: reply: X.Xs of speech`
   - The reply is spoken on the speaker and shown in the app.
7. **Volume:** ask Muse (“Set volume to 80”) or use `voice.configure`.
8. **Mute:** the XVF3800 Mute button; when muted, the button keeps its setup
   role.
9. **Reset setup:** hold the button for **5 seconds** (forgets pairing/Wi-Fi).

---

## 11. Troubleshooting

| Symptom | Check |
|---|---|
| No COM port | USB **data** cable; BOOT+RESET; driver |
| Build fails on v6.0.2 | Use **v6.0.3**, or disable `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY` (not production) |
| No sound | JST speaker seated; amp `X0D31=0`; volume; `XVF3800 firmware 1.0.9` |
| TTS silent | Log `TTS: N chars`; `TTS attempt … failed` → network/key; AssemblyAI key is 32 chars, no `gsk_` |
| Groq `sock < 0` | The hotspot/network cannot reach `api.groq.com`; use AssemblyAI or a local TTS proxy |
| No pairing | Developer mode; 2.4 GHz; valid SDK token; press the button |
| Ring not green | Wi-Fi/internet; app provisioning; log `setup=done wifi=up ws=up` |
| Button not detected | Grove: VCC→D3/GPIO4, SIG→D0/GPIO1, GND→GND; active-high config |
| Mic silent | Try the other I2S slot (`s_raw[2*i+1]`) in `voice_board_xvf3800.c` |

Useful log lines:

```
link.xvf3800: AIC3104 outputs powered (DAC 0 dB, HP + line out)
link.xvf3800: XVF3800 firmware 1.0.9; 16 kHz I2S slave, ESP32 is master
link.xvf3800: reSpeaker XVF3800 audio ready
link.voice:   ready
link.ble:     advertising as MuseGadget-respeaker-XXXXXX
muse_chat_session: connected to VM … 
muse_chat_session: TTS: N chars
muse_chat_session: reply audio: 16000 Hz, 1 ch, 64 kbps
link.xvf3800: amp on: X0D30=0 X0D31=0 X0D33=1
```

---

## 12. Security notes

- The SDK token ships inside the firmware (or NVS). Treat it as an identifier,
  not a password; revoke/rotate at `gadgets.muse.ai` if it leaks.
- The TTS key is in `esp32/build-*/sdkconfig`, which is gitignored. Do not commit
  it.
- Community pairing has no manufacturer attestation and cannot stop an active
  MITM; set it up on a trusted network.
- Secure Boot and flash encryption stay **off**; the app is signed with the
  committed development key so you can reflash freely.
- NVS encryption is recommended (`CONFIG_HOMEHUB_NVS_ENCRYPTION`) and is derived
  from an eFuse HMAC key automatically; decide before storing credentials.
- Do not enable `CONFIG_HOMEHUB_PAIRING_EFUSE_AUTH` unless you have Secure Boot.

---

## 13. References

- Muse Gadget SDK: <https://github.com/facebookincubator/muse-gadget-sdk>
- Seeed reSpeaker XVF3800 + XIAO:
  <https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_getting_started/>
- XMOS firmware update:
  <https://wiki.seeedstudio.com/respeaker_xvf3800_introduction/#update-firmware>
- I2S playback/record:
  <https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_record_playback/>
- I2C command list:
  <https://wiki.seeedstudio.com/respeaker_xvf_3800_i2c_list/>
- GPIO/GPI control:
  <https://wiki.seeedstudio.com/respeaker_xvf3800_xiao_gpio/>
- Vendor firmware repo:
  <https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY>
- AssemblyAI TTS:
  <https://www.assemblyai.com/docs/tts/api-reference/http-api/create-speech-openai>
- Groq TTS:
  <https://console.groq.com/docs/text-to-speech>
