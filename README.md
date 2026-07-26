# Token Tamagotchi 🐣

A virtual pet for small M5Stack ESP32 devices that lives on your Claude Code /
Codex CLI token usage. The primary target is the **M5Stack StopWatch Dev Kit
(ESP32-S3)**, with a compact **M5StickC Plus2-class** target for the smaller
240x135 screen.

```
         ⌚  ← the StopWatch
   🍔  ⬆ tokens  ──▶  🌕 happy / 😟 hungry / 💤 sleepy / 🤒 sick
   ⬇ text         ⬆  466x466 round AMOLED, 4 mood-sprite frames + blink
  🌉 bridge/worker⌨  B voice mode → B record/send → Whisper on Groq → text
  ~/TokenGochi    📡  HTTPS API, fallback LAN mode available
```

## Hardware

- **M5Stack StopWatch Dev Kit** (C152, ESP32-S3R8, 8 MB PSRAM, 16 MB flash)
  — has 1.75" round AMOLED, 2 buttons, MEMS mic, speaker, vibration motor.
- **M5StickC Plus2-class ESP32-PICO device** — compact 240x135 display target
  without touch/vibration. Build with PlatformIO env `m5stickc-plus2`.
- **A Mac or VPS** running Node 18+. Local mode runs `bridge/tamagotchi-bridge.mjs` on LAN; cloud mode runs a Cloudflare Worker and can either receive pushed snapshots from `bridge/token-ingest.mjs` or pull from `bridge/token-source.mjs` on a VPS.
- **A Groq API key** (free tier is fine) — only needed for `/transcribe`.

## First-run guide (5 minutes)

### 1. Start the backend

#### Option A — Local bridge (LAN fallback)

```sh
cd bridge
./install.sh                 # copies a launchd agent, starts it on :8787
launchctl list | grep tokengochi   # confirm it’s running
curl http://localhost:8787/health  # {ok, version, groq_configured}
```

If you don’t have a Groq key yet, set one in `bridge/.env`:

```sh
cp bridge/.env.example bridge/.env
echo "GROQ_API_KEY=gsk_..." >> bridge/.env
launchctl kickstart -k gui/$(id -u)/com.tokengochi.bridge
```

#### Option B — Cloudflare backend (recommended for guest/public Wi-Fi)

```sh
cd cloudflare
npm install
npm run deploy:staging       # set D1 IDs/secrets first; see cloudflare/README.md
```

Then choose where token totals come from.

By default, the bridge uses the same Codex account-usage source as CodexBar
when `~/.codex/auth.json` contains OAuth login tokens. That reports subscription
rate-limit usage as a percentage, not raw local session tokens. TokenGochi maps
1% weekly account usage to 1000 food units for the backend contract, and the
firmware displays the weekly account percentage when that metadata is present.
If account usage is unavailable in `TOKEN_USAGE_SOURCE=auto`, the bridge falls
back to local Claude/Codex transcript logs.

For Codex account usage, the pet mood is driven by pace against the weekly
window. TokenGochi uses CodexBar's linear pace fallback: expected usage is the
elapsed fraction of the reset window, and actual usage is compared against it
with CodexBar's 2% / 6% / 12% thresholds. Behind pace is shown as reserve:
you are leaving weekly tokens unused, so the pet becomes very hungry. Ahead of
pace is shown as deficit: you are spending faster than the even weekly pace, so
the pet becomes very happy.

- `slightly_behind` / `behind` / `far_behind` -> `very hungry`
- `on_track` -> `happy`
- `slightly_ahead` / `ahead` / `far_ahead` -> `very happy`

Push snapshots from the current machine:

```sh
cd bridge
node token-ingest.mjs --once
```

Or run a VPS token source and let Cloudflare pull from it:

```sh
cd bridge
TOKEN_SOURCE_TOKEN=... node token-source.mjs
# expose :8790 through Cloudflare Tunnel or Tailscale Funnel
# set Cloudflare TOKEN_SOURCE_URL to the public HTTPS token-source URL
```

The VPS Tailscale IP can be used for SSH/operator access, but the Worker needs
a public HTTPS tunnel/Funnel URL; it cannot fetch a private `100.x` tailnet IP
directly.

### 2. Get the backend URL

```sh
cd firmware
# for local mode, use:
ipconfig getifaddr en0
# e.g. 192.168.1.42 -> PROXY_URL "http://192.168.1.42:8787"

# for cloud mode, use the Worker URL, for example:
# PROXY_URL "https://tokengochi-staging.your-account.workers.dev"
```

You’ll paste the selected URL into firmware `src/config.h` in step 3.

### 3. Flash the firmware

```sh
cd firmware
cp src/secrets.h.example src/secrets.h
# edit src/secrets.h with your ordered WiFi networks and the bridge's DEVICE_TOKEN
# edit src/config.h to set PROXY_URL to your selected backend URL
```

Use the repo PlatformIO wrapper. It runs PlatformIO through `uv` with a
supported Python version:

```sh
../tools/pio --version
```

For the StopWatch, put it into download mode (hold the side button ~2 s until
the green LED is solid), then:

```sh
../tools/pio run -t upload
../tools/pio device monitor     # optional: 115200 baud serial log
```

For the M5StickC Plus2-class target:

```sh
../tools/pio run -e m5stickc-plus2 -t upload --upload-port /dev/cu.usbserial-...
../tools/pio device monitor --port /dev/cu.usbserial-... --baud 115200
```

### 4. Use the device

| action                                  | how                                            |
|-----------------------------------------|------------------------------------------------|
| See the pet’s mood on the disc          | just look at it (polls every 5 min)            |
| Check stats (mood / today / total / RSSI)| hold KEYB or tap the outer home ring           |
| Return home from stats                  | tap the stats screen or short-press KEYA       |
| Reset the pet (new birth, clear chat)   | stats → KEYB → KEYA, or tap yes in confirm     |
| Enter voice input mode                  | short-press KEYB                               |
| Record and send voice                   | KEYB/tap mic; Auto sends after a voice pause   |
| Cancel voice recording                  | press KEYA while recording                     |
| Open device settings                    | hold KEYA + KEYB from pet, voice, or stats     |
| Change voice mode                       | settings → Voice → tap Auto/10/20/30 s         |
| Adjust brightness / volume / feedback   | settings → Bright / Volume / Feed              |
| Check battery / warnings                | settings → Batt                                |
| Page through the transcript              | short-press KEYA or tap the screen while reading|
| Dismiss the transcript                  | short-press KEYB                                |

For a visual button/mode reference, open `docs/device-interactions.html`.

Power saver defaults lower both targets to 35% brightness, 40% volume, Wi-Fi
modem sleep, reduced CPU clock, passive sleep CPU downclock, 5 minute active
pet polling, 10 minute passive asleep polling, direct reconnect to the last
known-good SSID before scanning, 15 s auto-dim to 1%, display sleep after 30 s
of safe idle time, reduced Wi-Fi TX power while connected, MCU light sleep
with 5 s passive wake intervals, and 5 minute asleep Wi-Fi reconnect checks.
When the device is passively asleep, the Wi-Fi radio powers down between polls
or user interactions. Older saved device settings are migrated down to that
profile on first boot after the settings-version bump. The StopWatch target
skips MCU light sleep while a USB serial host is attached so diagnostics and
screen capture stay reliable, but keeps light sleep enabled for standalone or
power-only use.

### 5. Develop / iterate

- **Want a different pet?** `node tools/generate-sprites.mjs` regenerates
  `firmware/src/sprites.h` from the procedural artist in that file. The
  `tools/dump-sprite.mjs` script writes PNG previews of every frame to
  `/tmp/pet-sprites/` so you can see them on the Mac.
- **Want to test the bridge path without the device?**
  `node tools/synth-pet-state.mjs --port 8800 --cycle 3000` runs a canned
  bridge. `node tools/record-test-clip.mjs --url http://localhost:8800 --say "hi"`
  posts a TTS clip to it and prints the (mock) transcript.
- **Want a device-screen capture?**
  `node tools/capture-device-screen.mjs` captures the current AMOLED frame over
  USB serial and writes a PNG under `screenshots/`. Use the default round mask
  for the StopWatch, or `--mask none` for rectangular compact targets. StopWatch
  display geometry is documented in `docs/display-geometry.json`.

## API reference

| method | path           | auth | body              | returns                                       |
|--------|----------------|------|-------------------|-----------------------------------------------|
| GET    | `/health`      | no   | —                 | `{ok, version, groq_configured, whisper_model}` |
| GET    | `/tokens_today`| yes  | —                 | `{tokens_today, breakdown: {claude, codex}, ts}` |
| GET    | `/pet/state`   | yes  | —                 | `{mood, age_s, food_today, last_msg, last_msg_ts, total_tokens_ever, audio_runs_today, breakdown, ts}` |
| POST   | `/transcribe`  | yes  | `audio/wav` bytes | `{text, duration_s, lang, ms_groq}`           |
| POST   | `/pet/reset`   | yes  | —                 | same shape as `/pet/state` after the reset     |
| POST   | `/ingest/tokens` | ingest | token snapshot | stores pushed token totals in Cloudflare       |
| POST   | `/ingest/pull` | ingest | —                 | Cloudflare fetches the configured VPS token source |

## Architecture (one page)

```
+----------------------+      HTTPS      +----------------------+     D1      +-------------+
|  M5Stack device      | ------------->  |  Cloudflare Worker   | ---------> |  Groq API   |
|  HTTPS PROXY_URL     |                |  /pet, /transcribe   |            |  Whisper    |
+----------------------+                +----------------------+            +-------------+
       |      ^
       |      | optional LAN fallback
       |      v
+----------------------+
| bridge (LAN) +        |
| token ingest/source   |
+----------------------+
```

- **Firmware** (`firmware/`) — PlatformIO + Arduino + M5Unified. State
  machine: `IDLE / VOICE_IDLE / RECORDING / TRANSCRIBING / SHOWING / HISTORY_LIST / HISTORY_READING / STATS / CONFIRM / SETTINGS / SETTINGS_SAVED / ERROR`.
  4-sprite mood faces in `src/sprites.h` (~200 KB RGB565 in flash, PROGMEM).
- **Bridge/token source** (`bridge/`) — Self-contained `tamagotchi-bridge.mjs`
  in LAN mode, plus `token-ingest.mjs` for push publishing and
  `token-source.mjs` for VPS pull mode.
- **Cloudflare backend** (`cloudflare/`) — HTTPS API running at public URL and
  persists state in Cloudflare D1. Watch requests now avoid LAN restrictions.
- **Tools** (`tools/`) — Dev utilities: canned bridge, Mac-mic → bridge, sprite
  generator, PPM previewer, and device-screen capture.

## Testing

```sh
node bridge/_test_pure.mjs          # 11 pure-logic tests
node bridge/_test_transcribe.mjs    # 5 e2e tests with a mock Groq server
```

Both must exit 0. The e2e test spawns the real bridge and a mock Groq on
random ports, exercises every endpoint, and rolls back `state.json` at the
end so it leaves no side effects.

## Troubleshooting

| symptom                                          | fix                                                                                  |
|--------------------------------------------------|--------------------------------------------------------------------------------------|
| `PlatformIO requires Python 3.10–3.13`           | Use `./tools/pio` from the repo root; it runs PlatformIO through `uv` with Python 3.12 |
| `/transcribe → 503`                              | Add `GROQ_API_KEY` to `bridge/.env`; the bridge doesn’t crash, it just refuses       |
| Watch stuck on `wifi failed`                     | No saved SSID is visible, wrong password in `secrets.h`, or a non-2.4 GHz network     |
| `bridge down` icon on the watch                 | `ping <PROXY_URL host>` from the watch’s WiFi; check `bridge/bridge.log`             |
| Pet stuck on the same mood for hours            | `codex` / `claude` rollouts are at the wrong path; check the paths in the bridge     |
| Sprites look wrong on the device                 | Regenerate with `node tools/generate-sprites.mjs`, re-flash, eyeball `/tmp/pet-sprites/` |
| Want to nuke everything and start over          | `bridge/uninstall.sh && rm bridge/state.json`                                        |

## Project layout

```
TokenGochi/
├── PLAN.md                              design + decisions
├── README.md                            you are here
├── docs/
│   ├── display-capture.md               screenshot geometry + mask notes
│   └── display-geometry.json            round AMOLED usable-area data
├── bridge/
│   ├── tamagotchi-bridge.mjs           the server
│   ├── com.tokengochi.bridge.plist     launchd template
│   ├── install.sh / uninstall.sh
│   ├── _test_pure.mjs                   logic tests
│   ├── _test_transcribe.mjs             e2e tests
│   ├── state.json                       pet persistent state (gitignored)
│   ├── .env.example                     config template
│   └── README.md
├── firmware/
│   ├── platformio.ini
│   ├── README.md
│   └── src/
│       ├── main.cpp                     setup + main loop, firmware FSM
│       ├── config.h                     PROXY_URL + timeouts
│       ├── secrets.h.example           SSID + DEVICE_TOKEN
│       ├── audio.h / .cpp               M5.Mic record + WAV mux + M5.Speaker
│       ├── net.h / .cpp                 HTTPClient wrappers
│       ├── pet_state.h                  PetState struct
│       ├── transcript_log.h / .cpp      device-only transcript history
│       ├── pet_sprite.h / .cpp          renderer
│       ├── ui.h / .cpp                  round-disc helpers
│       └── sprites.h                    16 RGB565 frames (auto-generated)
└── tools/
    ├── README.md
    ├── synth-pet-state.mjs              canned bridge
    ├── record-test-clip.mjs             Mac-mic → bridge
    ├── capture-device-screen.mjs        USB serial screen capture
    ├── generate-sprites.mjs            sprite generator
    └── dump-sprite.mjs                  PPM previewer
```

## License

MIT.
