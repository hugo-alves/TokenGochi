# Token Tamagotchi 🐣

A virtual pet for the **M5Stack StopWatch Dev Kit (ESP32-S3)** that lives on your
Claude Code / Codex CLI token usage. Watch it get fat while you ship code, get
sleepy when you stop, and chat back when you hold the button.

```
         ⌚  ← the StopWatch
   🍔  ⬆ tokens  ──▶  🌕 happy / 😟 hungry / 💤 sleepy / 🤒 sick
   ⬇ text         ⬆  466x466 round AMOLED, 4 mood-sprite frames + blink
  🌉 bridge       ⌨  hold A to record → Whisper on Groq → text on screen
  ~/TokenGochi    📡  HTTP, LAN-only, no cloud, no account
```

## Hardware

- **M5Stack StopWatch Dev Kit** (C152, ESP32-S3R8, 8 MB PSRAM, 16 MB flash)
  — has 1.75" round AMOLED, 2 buttons, MEMS mic, speaker, vibration motor.
- **A Mac** (or any Linux box) running Node 18+ on the same LAN. The bridge is
  zero-dependency: just `node bridge/tamagotchi-bridge.mjs`.
- **A Groq API key** (free tier is fine) — only needed for `/transcribe`.

## First-run guide (5 minutes)

### 1. Start the bridge

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

### 2. Get the bridge URL

```sh
ipconfig getifaddr en0
# e.g. 192.168.1.42
```

You’ll paste that into the firmware in step 4.

### 3. Flash the firmware

```sh
cd firmware
cp src/secrets.h.example src/secrets.h
# edit src/secrets.h with your WiFi SSID, password, and the bridge's DEVICE_TOKEN
# edit src/config.h to set PROXY_URL to http://<bridge-ip>:8787
```

Install PlatformIO once:

```sh
pipx install platformio        # or: pip3 install --user platformio
# (system Python 3.10–3.13 required; see "Troubleshooting" if you have 3.14+)
```

Put the StopWatch into download mode (hold the side button ~2 s until the
green LED is solid), then:

```sh
pio run -t upload
pio device monitor             # optional: 115200 baud serial log
```

### 4. Use the device

| action                                  | how                                            |
|-----------------------------------------|------------------------------------------------|
| See the pet’s mood on the disc          | just look at it (polls every 30 s)             |
| Check stats (mood / today / total / RSSI)| short-press KEYB                               |
| Reset the pet (new birth, clear chat)   | KEYB → KEYA in the confirm overlay             |
| Talk to your pet                        | hold KEYA ≥ 600 ms → release → wait ~1 s       |
| Page through the transcript              | short-press KEYA while reading                  |
| Dismiss the transcript                  | short-press KEYB                                |

### 5. Develop / iterate

- **Want a different pet?** `node tools/generate-sprites.mjs` regenerates
  `firmware/src/sprites.h` from the procedural artist in that file. The
  `tools/dump-sprite.mjs` script writes PNG previews of every frame to
  `/tmp/pet-sprites/` so you can see them on the Mac.
- **Want to test the bridge path without the device?**
  `node tools/synth-pet-state.mjs --port 8800 --cycle 3000` runs a canned
  bridge. `node tools/record-test-clip.mjs --url http://localhost:8800 --say "hi"`
  posts a TTS clip to it and prints the (mock) transcript.

## API reference

| method | path           | auth | body              | returns                                       |
|--------|----------------|------|-------------------|-----------------------------------------------|
| GET    | `/health`      | no   | —                 | `{ok, version, groq_configured, whisper_model}` |
| GET    | `/tokens_today`| yes  | —                 | `{tokens_today, breakdown: {claude, codex}, ts}` |
| GET    | `/pet/state`   | yes  | —                 | `{mood, age_s, food_today, last_msg, last_msg_ts, total_tokens_ever, audio_runs_today, breakdown, ts}` |
| POST   | `/transcribe`  | yes  | `audio/wav` bytes | `{text, duration_s, lang, ms_groq}`           |
| POST   | `/pet/reset`   | yes  | —                 | same shape as `/pet/state` after the reset     |

## Architecture (one page)

```
+-------------------+      LAN      +-------------------+     HTTPS    +-------------+
|  StopWatch        |  HTTP JSON    |  bridge.mjs       |  multipart   |  Groq API   |
|  (ESP32-S3, C++)  | <-----------> |  Node 18+ stdlib  | <-----------> |  Whisper    |
|                   |               |                   |               |  v3 turbo   |
|  466x466 AMOLED   |               |  walks JSONL in:  |               +-------------+
|  2 buttons        |               |   ~/.claude/      |
|  mic + speaker    |               |   ~/.codex/        |
|  vibration motor  |               |                   |
+-------------------+               |  state.json in    |
                                    |  bridge/ dir       |
                                    +-------------------+
```

- **Firmware** (`firmware/`) — PlatformIO + Arduino + M5Unified. State
  machine: `IDLE / RECORDING / TRANSCRIBING / SHOWING / STATS / CONFIRM / ERROR`.
  4-sprite mood faces in `src/sprites.h` (~200 KB RGB565 in flash, PROGMEM).
- **Bridge** (`bridge/`) — Self-contained `tamagotchi-bridge.mjs`, runs as a
  launchd agent, no `npm install`. Reads Claude Code + Codex CLI rollouts to
  compute `tokens_today`, derives mood from time-of-day + tokens, proxies audio
  to Groq, persists pet state in `state.json`.
- **Tools** (`tools/`) — Dev utilities: canned bridge, Mac-mic → bridge, sprite
  generator, PPM previewer.

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
| `PlatformIO requires Python 3.10–3.13`           | Use a venv with `python3.12 -m venv .venv && .venv/bin/pip install platformio`        |
| `/transcribe → 503`                              | Add `GROQ_API_KEY` to `bridge/.env`; the bridge doesn’t crash, it just refuses       |
| Watch stuck on `wifi failed`                     | Wrong SSID/pass in `secrets.h`; the 2.4 GHz radio only (no 5 GHz)                     |
| `bridge down` icon on the watch                 | `ping <PROXY_URL host>` from the watch’s WiFi; check `bridge/bridge.log`             |
| Pet stuck on the same mood for hours            | `codex` / `claude` rollouts are at the wrong path; check the paths in the bridge     |
| Sprites look wrong on the device                 | Regenerate with `node tools/generate-sprites.mjs`, re-flash, eyeball `/tmp/pet-sprites/` |
| Want to nuke everything and start over          | `bridge/uninstall.sh && rm bridge/state.json`                                        |

## Project layout

```
TokenGochi/
├── PLAN.md                              design + decisions
├── README.md                            you are here
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
│       ├── main.cpp                     setup + main loop, 7-state FSM
│       ├── config.h                     PROXY_URL + timeouts
│       ├── secrets.h.example           SSID + DEVICE_TOKEN
│       ├── audio.h / .cpp               M5.Mic record + WAV mux + M5.Speaker
│       ├── net.h / .cpp                 HTTPClient wrappers
│       ├── pet_state.h                  PetState struct
│       ├── pet_sprite.h / .cpp          renderer
│       ├── ui.h / .cpp                  round-disc helpers
│       └── sprites.h                    16 RGB565 frames (auto-generated)
└── tools/
    ├── README.md
    ├── synth-pet-state.mjs              canned bridge
    ├── record-test-clip.mjs             Mac-mic → bridge
    ├── generate-sprites.mjs            sprite generator
    └── dump-sprite.mjs                  PPM previewer
```

## License

MIT.
