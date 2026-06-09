# Firmware

ESP32-S3 firmware for the **M5Stack StopWatch Dev Kit (C152)**. Renders a
round-AMOLED virtual pet fed by your Claude Code / Codex CLI token usage.
It can talk to either the local bridge (`../bridge`) or the Cloudflare worker
(`../cloudflare`).

## Quick start

### 1. Install PlatformIO

```sh
# CLI
pipx install platformio    # or: pip3 install --user platformio
# or use the VSCode extension
```

### 2. Create `src/secrets.h`

```sh
cp src/secrets.h.example src/secrets.h
# then edit secrets.h with your real WiFi + the bridge's DEVICE_TOKEN
```

### 3. Set `PROXY_URL`

Edit `src/config.h` to point at your backend URL:

```sh
# get your Mac's LAN IP
ipconfig getifaddr en0
# 192.168.1.42   <- put this in config.h, e.g.:
# #define PROXY_URL "http://192.168.1.42:8787"

# Cloudflare worker URL:
# https://tokengochi-staging.YOUR_ACCOUNT.workers.dev
# #define PROXY_URL "https://tokengochi-staging.YOUR_ACCOUNT.workers.dev"
```

### 4. Flash

Connect the StopWatch via USB-C, put it in download mode by holding the
reset button ~2 s until the green LED lights, then:

```sh
cd firmware
pio run -t upload
pio device monitor        # optional, 115200 baud
```

First build downloads the ESP32 toolchain + M5Unified + ArduinoJson
(~5 min on a fresh machine). Subsequent builds are seconds.

### 5. Capture the device screen

The firmware supports the `TGSHOT` serial command through
`../tools/capture-device-screen.mjs`. It captures the current 466×466 display
mirror over USB serial and writes a PNG:

```sh
cd ..
node tools/capture-device-screen.mjs
```

Captures are written under `screenshots/`, which is gitignored.

## Layout

```
firmware/
├── platformio.ini         M5Stack quickstart config (esp32s3box + StopWatch libs)
├── README.md              this file
└── src/
    ├── main.cpp           setup + loop, glue
    ├── config.h           PROXY_URL, timeouts, screen geometry (edit me)
    ├── secrets.h          gitignored, copy from secrets.h.example
    ├── secrets.h.example  template
    ├── pet_state.h        PetState struct
    ├── net.h / net.cpp    HTTPClient wrapper, fetchPetState
    └── ui.h / ui.cpp      round-AMOLED drawing helpers + TGSHOT mirror
```

## What this does (step 5 + 7 + 8 + 9)

When booted, the StopWatch:

1. Connects to WiFi.
2. Pings the bridge at `PROXY_URL` to confirm it can reach the LAN.
3. Polls `GET /pet/state` every 30 s and renders the pet face on the round AMOLED (yellow happy / orange hungry / blue sleepy / green sick, with a 4-frame blink animation).
4. **Hold KEYA ≥ 600 ms** to start recording from the MEMS mic. The top of the disc shows `REC` + elapsed seconds + a pulsing red bar. Recording auto-stops at 10 s.
5. On release, the firmware muxes the captured PCM into a 16 kHz/16-bit/mono WAV, POSTs it as `audio/wav` to `PROXY_URL/transcribe`.
6. The bridge forwards the audio to Groq Whisper and returns `{text, duration_s, lang, ms_groq}`. The firmware shows the transcript word-wrapped across pages; press A to page, B to dismiss.
7. A short chirp plays for success, a longer low chirp for failure, plus a vibration buzz on success. A `?` icon shows when the bridge is unreachable.
8. Tapping the bridge every 30 s in the background keeps the mood and food count fresh.
9. On boot, if the bridge has a `last_msg` from a previous session, it's shown for 3 s as a "last heard:" greeting.
10. **KEYB short** while idle → stats view (mood, food today, total tokens, audio runs, WiFi RSSI, bridge host).
11. **KEYB short** while stats → confirm prompt; **KEYB short** again → cancel; **KEYA short** while confirm → POST `/pet/reset` (new birth time, clear `last_msg`, zero today's audio runs). A success chirp + buzz confirms.

## Buttons

| button                  | state          | action                                                                 |
|-------------------------|----------------|------------------------------------------------------------------------|
| **KEYA hold ≥ 600 ms**  | idle / showing | start / stop recording                                                |
| **KEYA short**          | showing        | next transcript page                                                   |
| **KEYA short**          | confirm        | confirm reset → POST `/pet/reset`                                      |
| **KEYA short**          | stats          | back to idle (pet face)                                                |
| **KEYA short**          | error          | dismiss error                                                          |
| **KEYB short**          | idle           | show stats view                                                        |
| **KEYB short**          | stats          | show confirm prompt                                                    |
| **KEYB short**          | confirm        | cancel confirm → back to stats                                         |
| **KEYB short**          | showing        | dismiss transcript → back to idle                                      |
| **KEYB short**          | error          | dismiss error                                                          |

## File map

```
firmware/src/
├── main.cpp           setup + loop, the hold-A-to-talk state machine
├── config.h           PROXY_URL, timeouts, screen geometry (edit me)
├── secrets.h          gitignored, copy from secrets.h.example
├── audio.h / .cpp     M5.Mic slot recording, WAV mux, M5.Speaker chirps
├── net.h / .cpp       HTTPClient wrappers, /pet/state GET, /transcribe POST
├── pet_state.h        PetState struct
├── pet_sprite.h/.cpp  sprite renderer (mood ↔ index, blink tick, drawCentered)
├── ui.h / .cpp        round-disc display helpers (status, mood, REC, transcript)
└── sprites.h          16× 80×80 RGB565 faces, auto-generated by tools/generate-sprites.mjs
```

## Pin map (from M5Stack docs)

| peripheral       | pins                                           |
|------------------|------------------------------------------------|
| AMOLED (CO5300)  | G39 CS, G40 SCK, G38 TE, G41–G46 D0–D3 (QSPI)  |
| Touch (CST820B)  | G47 SDA, G48 SCL, G13 INT (shared I2C)         |
| Audio (ES8311)   | G18 MCLK, G17 BCLK, G16 DOUT, G15 LRCK, G21 DIN|
| Buttons          | G1 (KEYB blue), G2 (KEYA yellow)                |
| Vibration        | M5IOE1 PYG9 (PWM) via M5.Power.setVibration()   |
| I2C bus          | G47 SDA / G48 SCL (shared: touch, audio, IMU, RTC, M5IOE1) |

You don't need to touch any of this in v1 — `M5Unified` wires it all up.

## Pin map (from M5Stack docs)

| peripheral       | pins                                           |
|------------------|------------------------------------------------|
| AMOLED (CO5300)  | G39 CS, G40 SCK, G38 TE, G41–G46 D0–D3 (QSPI)  |
| Touch (CST820B)  | G47 SDA, G48 SCL, G13 INT (shared I2C)         |
| Audio (ES8311)   | G18 MCLK, G17 BCLK, G16 DOUT, G15 LRCK, G21 DIN|
| I2C bus          | G47 SDA / G48 SCL (shared: touch, audio, IMU, RTC, M5IOE1) |
| Buttons          | G1 (KEYB blue), G2 (KEYA yellow)                |
| Vibration        | M5IOE1 PYG9 (PWM)                              |
| Speaker amp      | M5IOE1 PYG10 (enable)                          |

You don't need to touch any of this in v1 — `M5Unified` wires it all up.
