# Firmware

ESP32-S3 firmware for the **M5Stack StopWatch Dev Kit (C152)**. Renders a
round-AMOLED virtual pet fed by your Claude Code / Codex CLI token usage.
It can talk to either the local bridge (`../bridge`) or the Cloudflare worker
(`../cloudflare`).

## Quick start

### 1. Use the repo PlatformIO wrapper

```sh
cd ..
./tools/pio --version
```

### 2. Create `src/secrets.h`

```sh
cp src/secrets.h.example src/secrets.h
# then edit secrets.h with your ordered WiFi networks + the bridge's DEVICE_TOKEN
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
../tools/pio run -t upload
../tools/pio device monitor        # optional, 115200 baud
```

First build downloads the ESP32 toolchain + M5Unified + ArduinoJson
(~5 min on a fresh machine). Subsequent builds are seconds.

### 5. Capture the device screen

The firmware supports the `TGSHOT` serial command through
`../tools/capture-device-screen.mjs`. It captures the current 466×466 display
mirror over USB serial and writes a PNG with the round-screen alpha mask:

```sh
cd ..
node tools/capture-device-screen.mjs
```

Captures are written under `screenshots/`, which is gitignored.
The round-screen geometry is stored in `../docs/display-geometry.json`.

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

1. Connects to the first visible saved WiFi network in `secrets.h`.
2. Pings the bridge at `PROXY_URL` to confirm it can reach the LAN.
3. Polls `GET /pet/state` every 30 s and renders the pet face on the round AMOLED (yellow happy / orange hungry / blue sleepy / green sick, with a 4-frame blink animation).
4. **KEYA short** while idle opens the device-only transcript history.
5. **KEYB short** while idle enters voice input mode without recording.
6. **KEYB short** or tapping the mic starts recording from the MEMS mic. The top of the disc shows `REC` + elapsed seconds + a pulsing red bar. In Auto voice mode, the recorder trims silence and sends after a detected pause; fixed 10/20/30 s caps are still available. **KEYB short** or tapping the mic while recording sends immediately. **KEYA** cancels recording and returns to the pet without uploading.
7. When recording completes, the firmware trims the captured PCM to the detected speech window, muxes it into a 16 kHz/16-bit/mono WAV, and POSTs it as `audio/wav` to `PROXY_URL/transcribe`. Quiet clips are rejected locally without uploading.
8. The bridge forwards the audio to Groq Whisper and returns `{text, duration_s, lang, ms_groq}`. The firmware stores successful transcripts on the device only for 7 days, capped at 30 entries, then shows the latest transcript word-wrapped across pages; press A or tap the screen to page, B to dismiss.
9. A short chirp plays for success, a longer low chirp for failure, plus a vibration buzz on success. Sound, volume, brightness, vibration, voice mode, auto-dim, and battery warnings are device settings. A `?` icon shows when the bridge is unreachable.
10. Polling the bridge every 30 s in the background keeps the mood and food count fresh.
11. On boot, if the bridge has a `last_msg` from a previous session, it's shown for 3 s as a "last heard:" greeting.
12. **KEYB hold** or tapping the outer home ring while idle opens stats view (mood, food today, total tokens, audio runs, WiFi RSSI, bridge host). **KEYA short**, tap, or timeout returns home.
13. **KEYB short** while stats opens the reset confirm prompt; **KEYB short** or tapping no cancels; **KEYA short** or tapping yes posts `/pet/reset` (new birth time, clear `last_msg`, zero today's audio runs). A success chirp + buzz confirms.
14. **KEYA + KEYB hold** from pet, voice idle, or stats opens device settings. The settings menu includes Auto/10/20/30 s voice mode, brightness, volume, feedback, auto-dim, and battery status. Use touch to open visible setting chips, **KEYB short** to cycle focus or values, **KEYB hold** to open/return from a selected item, and **KEYA short** to go back or return to the pet.
15. In transcript history, use **KEYB short** to move older, **KEYA short** or tap to open the selected transcript, **KEYB short** inside a transcript to return to the list, and **KEYB hold** from the list to return to the pet.

## Buttons

| button                  | state          | action                                                                 |
|-------------------------|----------------|------------------------------------------------------------------------|
| **KEYA short**          | voice          | back to idle (pet face), no recording                                |
| **KEYA short**          | idle           | open device-only transcript history                                  |
| **KEYA press**          | recording      | cancel recording → back to idle, no upload                           |
| **KEYA short**          | showing        | next transcript page                                                   |
| **KEYA short**          | history list   | open selected history entry                                            |
| **KEYA short**          | history entry  | next transcript page                                                   |
| **KEYA short**          | confirm        | confirm reset → POST `/pet/reset`                                      |
| **KEYA short**          | settings       | back to idle (pet face)                                                |
| **KEYA short**          | stats          | back to idle (pet face)                                                |
| **KEYA short**          | error          | dismiss error                                                          |
| **KEYA + KEYB hold**    | idle/voice/stats | open device settings                                                  |
| **KEYB short**          | idle           | enter voice input mode, no recording                                  |
| **KEYB short**          | voice          | start recording                                                        |
| **KEYB short**          | recording      | stop recording → POST `/transcribe`                                   |
| **KEYB hold**           | idle           | show stats view                                                        |
| **KEYB short**          | settings menu  | move selected settings item                                            |
| **KEYB hold**           | settings menu  | open selected settings item                                            |
| **KEYB short**          | settings item  | cycle/change the visible setting                                       |
| **KEYB hold**           | settings item  | return to settings menu                                                |
| **KEYB short**          | stats          | show confirm prompt                                                    |
| **KEYB short**          | confirm        | cancel confirm → back to stats                                         |
| **KEYB short**          | showing        | dismiss transcript → back to idle                                      |
| **KEYB short**          | history list   | cycle to the next older entry                                          |
| **KEYB hold**           | history list   | back to idle (pet face)                                                |
| **KEYB short**          | history entry  | back to history list                                                   |
| **KEYB short**          | error          | dismiss error                                                          |
| **touch tap**           | idle home ring | show stats view                                                        |
| **touch tap**           | voice mic      | start recording                                                        |
| **touch tap**           | recording mic  | stop recording → POST `/transcribe`                                    |
| **touch tap**           | stats          | back to idle (pet face)                                                |
| **touch tap**           | confirm yes/no | confirm reset or cancel back to stats                                  |
| **touch tap**           | settings menu  | open tapped settings item                                              |
| **touch tap**           | settings item  | change tapped setting or refresh battery status                        |
| **touch tap**           | showing        | next transcript page                                                   |
| **touch tap**           | history list   | open selected history entry                                            |
| **touch tap**           | history entry  | next transcript page                                                   |

## File map

```
firmware/src/
├── main.cpp           setup + loop, pet/voice/stats state machine
├── config.h           PROXY_URL, timeouts, screen geometry (edit me)
├── secrets.h          gitignored, copy from secrets.h.example
├── audio.h / .cpp     M5.Mic slot recording, WAV mux, M5.Speaker chirps
├── net.h / .cpp       HTTPClient wrappers, /pet/state GET, /transcribe POST
├── pet_state.h        PetState struct
├── transcript_log.h/.cpp device-only transcript ring log, 7-day retention
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
