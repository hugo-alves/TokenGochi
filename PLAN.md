# Token Tamagotchi — Architecture Plan

A virtual pet that lives on the **M5Stack StopWatch Dev Kit (C152, ESP32-S3)**,
fed by your Claude Code / Codex CLI token usage, and chatty through
**Groq-hosted `whisper-large-v3-turbo`**.

---

## 0. System at a glance

```
+-------------------+      LAN / WiFi      +---------------------+      HTTPS     +---------------+
| M5Stack StopWatch |  --(HTTP JSON +      |  bridge.mjs (Mac)   |  --(multipart)-->|   Groq API   |
| (C152, ESP32-S3)  |     multipart WAV)-> |  Node 18+, no npm   |  <--(text)------| whisper-v3-  |
|                   |  <-(JSON)----------  |                     |                 |    turbo     |
|  - 1.75" AMOLED   |                      |  - /tokens_today    |                 +---------------+
|    466x466 round  |                      |  - /transcribe      |
|  - ES8311 mic+spk |                      |  - /pet/state       |
|  - 2 buttons+IRQ  |                      |  - /health          |
|  - vibration, IMU |                      +---------------------+
+-------------------+                                |
        ^                                            v
   (human)                                 ~/.claude/projects/
        ^                                  ~/.codex/sessions/
        |                                  (JSONL rollouts)
   (claude code / codex CLI)
```

Three runtime pieces, one trust boundary (the bridge). The watch never sees the
internet, your token logs, or your Groq key directly.

---

## 1. M5Stack StopWatch Dev Kit (C152) — confirmed

| spec            | value                                            |
|-----------------|--------------------------------------------------|
| SoC             | ESP32-S3R8 dual-core LX7 @ 240 MHz               |
| Flash / PSRAM   | 16 MB / **8 MB PSRAM**                           |
| Wi-Fi           | 2.4 GHz 802.11 b/g/n                             |
| Display         | 1.75" AMOLED round, **466×466**, CO5300 (QSPI)   |
| Touch           | CST820B, I2C shared bus                          |
| Audio           | ES8311 codec + MEMS mic + AW8737A amp + 8Ω/1W spk |
| Buttons         | **2 programmable** (KEYA on G2, KEYB on G1) + 1 power |
| Vibration       | built-in motor, PWM via M5IOE1 PYG9              |
| IMU             | BMI270 6-axis (I2C shared)                       |
| RTC             | RX8130CE (I2C shared)                            |
| Power           | M5PM1 + 450 mAh battery, USB-C                   |
| Size / weight   | 52×52×15.5 mm, 39 g                              |

**I2C bus is shared** (G47 SDA / G48 SCL) by touch, audio, IMU, RTC, M5IOE1
expander. Addresses to remember:
- ES8311 `0x18`
- BMI270 `0x68`
- RX8130CE `0x32`
- M5IOE1 `0x4F`
- CST820B (no address — handled by lib)

**I2S pins for audio** (mic capture + speaker playback, same bus):
- G18 MCLK, G17 BCLK, G16 ASDOUT (to speaker), G15 LRCK, G21 DSDIN (from mic)

**Power & IO enable** all go through the M5IOE1 expander: speaker amp,
vibration PWM, OLED reset, touch reset, audio power, MUX for USB/UART.

### 1.1 Library stack (PlatformIO)

From the official M5Stack quickstart:

```ini
[env:m5stack-stopwatch]
platform = espressif32 @ 6.12.0
board = esp32s3box               ; close enough; M5Unified handles overrides
framework = arduino
board_build.partitions = default_16MB.csv
board_build.arduino.memory_type = qio_opi
monitor_speed = 115200
build_flags =
    -DESP32S3
    -DBOARD_HAS_PSRAM
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DARDUINO_USB_MODE=1
lib_deps =
    M5Unified  = https://github.com/m5stack/M5Unified
    M5GFX      = https://github.com/m5stack/M5GFX
    M5PM1      = https://github.com/m5stack/M5PM1
    M5IOE1     = https://github.com/m5stack/M5IOE1
```

`M5Unified` is the right entry point — it knows the StopWatch's pins, gives us
`M5.Display`, `M5.Speaker`, `M5.Mic`, `M5.BtnA`, `M5.BtnB`, `M5.Power`, and
hooks into `M5GFX` for the round AMOLED.

---

## 2. UI / pet model (round AMOLED, 466×466)

The round display changes the design. The pet is a **centered circular sprite**
sitting in the visible disc; the corners are masked by the chassis. Status text
sits in a thin ring around the sprite.

```
            ╭─────────────╮
        ╭───╋   pet face  ╋───╮
        │   │   (sprite)  │   │
        │   ╰─────────────╯   │
        │  mood:  happy  :)  │   ← 24 px text ring
        │  42k tk · 3d 4h    │
        ╰───────────────────╯
```

- Sprite: **200×200** centered, masked to a circle. 4 moods × 4 blink frames =
  16 frames, ~25 KB raw / ~6 KB in PROGMEM. Stored as 16-bit RGB565.
- Mood ring: 1 px status arc on the outer ring (green = happy, yellow = hungry,
  red = sick, dim blue = sleepy).
- Status text: under the sprite, ≤24 px tall. Two lines: mood + tokens, age.
- All drawing uses `M5.Display` (`LovyanGFX` under the hood). The 466×466 round
  canvas with a 233 px radius means we have ~170,000 visible pixels — plenty
  for expressive face work.

**Round-clip helper**: every draw call goes through a `clipToCircle()` that
sets the canvas scissor to the disc; nothing leaks into the hidden corners.

---

## 3. Inputs

| input           | short press                | long press (>600ms)        |
|-----------------|----------------------------|----------------------------|
| **KEYA (G2)**   | next page of transcript    | **hold to record**         |
| **KEYB (G1)**   | toggle "stats" / pet view  | reset pet (triple-tap guard) |
| **touch tap**   | dismiss / back to pet face | (reserved)                 |
| **IMU shake**   | (reserved) wake / feed     | —                          |

Recording starts on KEYA long-press, ends on release. While recording:
- top of the ring shows a red `REC` arc and elapsed seconds
- 16 kHz / mono / 16-bit PCM streams into a **PSRAM ring buffer** (8 MB
  available → 30 s of audio = 960 KB, fits with room to spare)
- cap at 30 s and force-stop if exceeded

---

## 4. Audio capture & playback

- **Capture**: I2S RX from MEMS mic → PSRAM ring → WAV mux on stop
- **Playback**: ES8311 → AW8737A → 8Ω speaker. Used in v1 for **short UI
  sounds** (chirp on mood change, click on button press, error tone on
  transcribe fail). No TTS in v1.
- The I2S config is shared; we switch the codec mode between record and play
  via the M5Unified `Speaker` / `Mic` APIs.

---

## 5. Networking

- WiFi creds in `firmware/src/secrets.h` (gitignored)
- `PROXY_URL` (bridge base URL) compiled in via `build_flags` or set in a
  tiny on-screen config menu
- All requests: `Authorization: Bearer ${DEVICE_TOKEN}`
- Background: `GET /pet/state` every 30 s; back off to 5 s while recording
- All HTTP via `HTTPClient` with `setTimeout(8000)` — Whisper round-trip is
  1–3 s, leave headroom

---

## 6. Bridge server (`bridge/tamagotchi-bridge.mjs`)

Keep it zero-dep. Node 18+ stdlib only.

### 6.1 Existing (keep, but harden)

- `GET /tokens_today` — bearer-authed, 30 s cache, walks Claude + Codex rollouts.
- Add `content-type: application/json` on the response (missing today).
- Add a `seen` set in `computeCodex` to dedupe across session-resume files.

### 6.2 New endpoints

| method | path          | body                              | returns                                       |
|--------|---------------|-----------------------------------|-----------------------------------------------|
| GET    | `/pet/state`  | —                                 | `{ mood, age_s, food_today, last_msg, ts }`   |
| POST   | `/transcribe` | `multipart/form-data` `file=*.wav` | `{ text, duration_s, lang }`                  |
| GET    | `/health`     | —                                 | `{ ok: true, groq_configured: bool, version }`|

`/pet/state` is **derived**:
- `food_today` ← `tokens_today` (existing pipeline, cache reused)
- `mood` rule:
  - `sleepy` if local hour 23:00–07:00
  - `sick`   if `food_today === 0` AND hour ≥ 22:00
  - `hungry` if `food_today < 5_000`
  - `happy`  otherwise
- `age_s` ← `now - pet_birth_ts` (read from `state.json`, written on first run)
- `last_msg` ← from `state.json` (so firmware can show "you said: ..." on wake)

`/transcribe`:
1. parse multipart, stream the audio to a tmp file (or pipe straight to Groq)
2. POST to `https://api.groq.com/openai/v1/audio/transcriptions`
   - model: `whisper-large-v3-turbo` (env-overridable to `whisper-large-v3`)
   - `response_format: json`
   - language auto-detect
3. write `last_msg` to `state.json`, append a small ring log
4. return transcript to firmware

`/transcribe` requires `GROQ_API_KEY`. If missing → `503 { error: "groq not
configured" }` and log a one-time setup hint. Never crash the server on it.

### 6.3 Auth

One shared `DEVICE_TOKEN` for v1. Same constant on both sides. The watch is on
your LAN; if you want a per-device token later, the request already carries one
header so swapping in a lookup is a one-file change.

### 6.4 Persistence

`state.json` next to the bridge:
- `pet_birth_ts` — real age, not uptime
- `last_msg`, `last_msg_ts` — "you said: ..." on wake
- `total_tokens_ever` — nice pet stat
- `audio_runs_today` — for "you talked to me 12 times today!" displays

No DB, no locks. Worst case the file gets a stale write and we rebuild from
the JSONL next request.

---

## 7. Whisper / Groq integration

### 7.1 Endpoint

`POST https://api.groq.com/openai/v1/audio/transcriptions`

| model                     | cost / hr audio | typical latency (10 s clip) |
|---------------------------|-----------------|------------------------------|
| `whisper-large-v3-turbo`  | $0.033          | ~0.7 s                        |
| `whisper-large-v3`        | $0.111          | ~1.2 s                        |

**Default**: `whisper-large-v3-turbo`. Override with `GROQ_WHISPER_MODEL`.

### 7.2 Audio contract

- container: WAV (PCM, 16-bit, **16 kHz**, mono)
- max size: 30 s hard cap on the firmware side → 960 KB → well under Groq's 25 MB
- mime: `audio/wav`, field name `file`

### 7.3 Latency budget (10 s clip, end-to-end)

| step                                  | budget |
|---------------------------------------|--------|
| button debounce + record start        | 50 ms  |
| record 10 s of speech                 | 10 000 ms |
| HTTP POST 960 KB over WiFi (LAN)      | 800 ms |
| Groq round-trip                       | 700 ms |
| JSON parse + display                  | 50 ms  |
| **total p50**                         | **~11.6 s** (dominated by speech) |

For a 2 s message: **~3.6 s** to words on screen. Feels right for a pet.

---

## 8. End-to-end data flow

### 8.1 Pet state refresh (background)

```
loop (every 30 s):
  GET  http://<bridge>/pet/state  Authorization: Bearer <token>
  -> { mood, age_s, food_today, last_msg, ts }
  draw sprite(mood); draw ring + status line
  if mood changed: chirp() + vibrate(40ms)
```

### 8.2 Hold-to-talk

```
KEYA pressed & held > 600 ms:
  chirp(800Hz, 80ms); start I2S record into PSRAM ring
  update REC arc + elapsed-seconds overlay
KEYA released:
  stop record; mux WAV header; POST /transcribe
  on 200:
    draw transcript (paged with KEYA); update last_msg
    chirp(1200Hz, 120ms)
  on 4xx/5xx/timeout:
    draw sad face for 2 s; vibration triple-tap
  in both cases: return to pet view
```

### 8.3 Failure modes handled in v1

- bridge unreachable → firmware shows `?` icon, retries with backoff
- groq 4xx/5xx → firmware shows sad face; bridge logs the body
- recording too long → firmware force-stops at 30 s, sends what it has
- token scan slow on first call → bridge serves stale cache; firmware gets `ts`
  so it can tell data is fresh

---

## 9. Repo layout (proposed)

```
TokenGochi/
├── PLAN.md                          ← you are here
├── README.md                        ← how to run the whole thing
├── bridge/
│   ├── tamagotchi-bridge.mjs        ← existing, extended
│   ├── .env.example                 ← GROQ_API_KEY, DEVICE_TOKEN, PORT
│   └── state.json                   ← created at runtime, gitignored
├── firmware/
│   ├── platformio.ini               ← from M5Stack quickstart
│   ├── src/
│   │   ├── main.cpp
│   │   ├── pet.h / pet.cpp          ← sprite frames, mood mapping
│   │   ├── audio.h / audio.cpp      ← I2S capture/playback + WAV mux
│   │   ├── net.h / net.cpp          ← HTTPClient wrappers
│   │   ├── ui.h / ui.cpp            ← round-clip + status ring
│   │   └── secrets.h                ← gitignored; WiFi + PROXY_URL + TOKEN
│   ├── assets/
│   │   └── sprites.h                ← PROGMEM RGB565 bitmaps
│   └── test/                        ← host-side mock tests for the parser
└── tools/
    ├── synth-pet-state.mjs          ← mock the bridge for firmware dev
    └── record-test-clip.mjs         ← record mic clip, POST to /transcribe
```

---

## 10. Resolved decisions

1. **M5 device** → M5Stack StopWatch Dev Kit (C152, ESP32-S3R8, 8 MB PSRAM)
2. **Framework** → PlatformIO + Arduino, M5Unified + M5GFX + M5PM1 + M5IOE1
3. **Recording cap** → 30 s (PSRAM is plentiful; was 10 s on the StickC plan)
4. **Pet replies** → transcribe only in v1; speaker reserved for UI chirps/tone alerts
5. **Whisper model** → `whisper-large-v3-turbo`, env-overridable
6. **Auth** → one shared `DEVICE_TOKEN` for v1
7. **Pet death/reset** → rule-based mood only in v1, no death state; long-press B
   resets the pet (re-creates `state.json` with new `pet_birth_ts`)

---

## 11. Resolved (final v1 scope)

1. **Pet art** → generate 16 original procedural face variants (4 moods × 4
   frames) with `tools/generate-sprites.mjs` and embed them as RGB565 in
   `firmware/src/sprites.h`.
2. **Bridge uptime** → ship a `com.tokengochi.bridge.plist` `launchd` agent in
   `bridge/` and document `launchctl load -w` in the README. RunAtLoad +
   KeepAlive. Logs to `bridge/bridge.log`.
3. **Time-of-day** → bridge's local time. No TZ plumbing in v1.
4. **Pet roster** → single pet, single `state.json`.

---

## 12. Implementation order

Each step is independently testable. Stop after any step and the system still
works (or has a clean placeholder).

1. **Bridge: extend the existing `tamagotchi-bridge.mjs`**
   - add `content-type: application/json` to `/tokens_today`
   - add `/pet/state` (derived from `/tokens_today` + clock)
   - add `state.json` persistence (birth_ts, last_msg)
   - add `dotenv`-free env loading for `GROQ_API_KEY` / `DEVICE_TOKEN` / `PORT`
   - **test**: `node bridge/tamagotchi-bridge.mjs --once` and `curl` against it

2. **Bridge: add `/transcribe` and `/health`**
   - hand-rolled multipart parser (no deps) to keep zero-install
   - pipe to Groq, return `{ text, duration_s, lang }`
   - stub the multipart body in a unit test before wiring the real fetch

3. **Bridge: ship the launchd agent**
   - `bridge/com.tokengochi.bridge.plist`
   - `bridge/install.sh` (one-liner that loads it)
   - README section

4. **Tools: build the firmware dev loop**
   - `tools/synth-pet-state.mjs` — serves canned `/pet/state` so you can
     iterate on the UI without the real bridge
   - `tools/record-test-clip.mjs` — record mic on the Mac, POST to
     `/transcribe`, print transcript. End-to-end smoke test of the bridge.

5. **Firmware: scaffold + WiFi + state polling**
   - `firmware/platformio.ini` from the M5Stack quickstart
   - `secrets.h` template
   - `main.cpp` with `M5.begin()`, WiFi connect, 30 s `GET /pet/state` loop
   - **test**: screen shows mood arc + token count, no recording yet

6. **Firmware: pet face on the round AMOLED**
   - 16 sprites in `assets/sprites.h`
   - `ui.cpp` with `clipToCircle()` + ring renderer
   - mood-change chirp + vibration

7. **Firmware: audio in / out**
   - PSRAM ring buffer, I2S mic capture, WAV mux
   - `M5.Speaker` for chirps/error tones

8. **Firmware: hold-to-talk end-to-end**
   - KEYA long-press → record → POST `/transcribe` → page transcript on screen
   - error path: sad face + triple-vibrate
   - **milestone**: real words on the watch after speaking into it

9. **Firmware polish**
   - stats view (KEYB), reset pet (long KEYB), last_msg on wake
   - low-power / RTC wake (out of scope for v1 — noted as v2)

10. **README + first-run guide** — single doc covering: install bridge,
    install launchd agent, set Groq key, set WiFi creds, flash firmware, what
    each button does.

---

## 13. Out of scope for v1 (parking lot)

- TTS playback of the pet's "replies" (we have a speaker; defer)
- LLM-backed pet conversations (needs `/chat` + prompt design + cost)
- IMU shake-to-feed, gesture interactions
- RTC-based deep sleep with scheduled wake
- Multi-pet stable
- OTA firmware updates
- Per-device bearer tokens
- HTTPS / mDNS (HTTP on LAN is fine for a local pet)
