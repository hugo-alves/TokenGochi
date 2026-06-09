# Tools

Standalone Node scripts for developing and testing the Token Tamagotchi stack
without flashing the device. Zero dependencies — uses only the Node 18+
stdlib plus macOS built-ins (`say`, `afconvert`) and any of `sox` / `ffmpeg`
you happen to have on `PATH`.

## `synth-pet-state.mjs` — synthetic bridge

A drop-in replacement for `bridge/tamagotchi-bridge.mjs` that returns canned
values you control via CLI flags. Use it to iterate on the firmware's UI
without depending on the real token logs, Groq, or your Mac being on.

```sh
node tools/synth-pet-state.mjs
# synthetic bridge on :8788 — fixed=happy, food=10000, age=3600s
```

### Flags

| flag                | default          | what it does                                       |
|---------------------|------------------|----------------------------------------------------|
| `--port N`          | `8788`           | listen port (real bridge uses 8787)                |
| `--token T`         | bridge default   | bearer token the firmware sends                    |
| `--mood M`          | `happy`          | initial mood: `happy`\|`hungry`\|`sleepy`\|`sick`  |
| `--food N`          | `10000`          | `food_today`                                       |
| `--age N`           | `3600`           | age in seconds since pet "birth"                   |
| `--msg "..."`       | `""`             | initial `last_msg`                                 |
| `--total N`         | `0`              | `total_tokens_ever`                                |
| `--audio N`         | `0`              | `audio_runs_today`                                 |
| `--cycle N`         | `0` (off)        | rotate mood every N ms (great for testing renders) |
| `--transcribe-text` | `"synthetic…"`   | canned `text` from `/transcribe`                   |

### Examples

```sh
# happy, well-fed pet
node tools/synth-pet-state.mjs --mood happy --food 50000

# cycle through all four moods every 3s — watch the UI react
node tools/synth-pet-state.mjs --cycle 3000

# angry / hungry / sick at 0 tokens, for the "needs feeding" path
node tools/synth-pet-state.mjs --mood hungry --food 0 --age 2592000

# talk to the synth bridge the same way the firmware will
curl -H "Authorization: Bearer the-same-long-random-string-as-the-firmware" \
  http://localhost:8788/pet/state
```

When the firmware POSTs to `/transcribe`, the synth tool updates its
in-memory `last_msg` and `audio_runs_today` and returns the canned text —
mirroring what the real bridge does.

## `record-test-clip.mjs` — capture + transcribe

Generates or captures audio and POSTs it to the bridge's `/transcribe`
endpoint. Useful for smoke-testing the Whisper/Groq path from your Mac.

### Modes

```sh
# default: synthesize speech via macOS `say` and post it
node tools/record-test-clip.mjs

# pick what `say` says
node tools/record-test-clip.mjs --say "deploy the satellite"

# use an existing WAV
node tools/record-test-clip.mjs --in /path/to/clip.wav

# record N seconds from the default mic (needs sox `rec` or ffmpeg)
node tools/record-test-clip.mjs --seconds 3

# save the audio before posting (handy for debugging)
node tools/record-test-clip.mjs --say "test" --save /tmp/clip.wav

# point at a non-default bridge (e.g. the synth tool on :8788)
node tools/record-test-clip.mjs --url http://localhost:8788 --say "hi"
```

`--save` writes the generated/captured WAV to disk before posting — useful
for sanity-checking what the firmware will end up sending.

### Flags

| flag          | default            | what it does                              |
|---------------|--------------------|-------------------------------------------|
| `--url`       | `http://localhost:8787` | bridge base URL                      |
| `--token`     | bridge default     | bearer token                              |
| `--in FILE`   | —                  | use an existing WAV file                  |
| `--say TEXT`  | (uses default)     | macOS TTS via `say` + `afconvert`         |
| `--seconds N` | —                  | record N seconds via `rec` (sox) or `ffmpeg` |
| `--save FILE` | —                  | write the WAV to disk before posting      |

### Examples

```sh
# round-trip a real Groq call (you must have GROQ_API_KEY in bridge .env)
node tools/record-test-clip.mjs --say "what's my token count"

# dry-run against the synth tool
node tools/record-test-clip.mjs --url http://localhost:8788 --say "ping"

# capture a 3s mic clip
node tools/record-test-clip.mjs --seconds 3
```

## `capture-device-screen.mjs` — device screen PNG

Captures the current M5Stack StopWatch AMOLED frame over USB serial and writes
a PNG. This is a real firmware-side capture, not a camera photo: the firmware
keeps a PSRAM-backed display mirror and responds to the `TGSHOT` serial command.

### Usage

```sh
# auto-detect the USB serial port and write screenshots/tokengochi-<timestamp>.png
node tools/capture-device-screen.mjs

# stable path for comparisons, PR notes, or manual inspection
node tools/capture-device-screen.mjs --out screenshots/color-check.png

# use a specific serial port
node tools/capture-device-screen.mjs --port /dev/cu.usbmodem1101

# see available candidate ports
node tools/capture-device-screen.mjs --list-ports
```

By default the tool also updates `screenshots/latest.png`. The `screenshots/`
directory is gitignored because these PNGs are generated artifacts.

### Flags

| flag             | default                           | what it does                              |
|------------------|-----------------------------------|-------------------------------------------|
| `--port PATH`    | auto-detected USB serial          | serial port for the connected device      |
| `--baud N`       | `115200`                          | serial baud rate                          |
| `--out FILE`     | timestamped PNG under `screenshots/` | PNG output path                       |
| `--latest FILE`  | `screenshots/latest.png`          | also copy the capture to this path        |
| `--no-latest`    | —                                 | skip updating `latest.png`                |
| `--timeout-ms N` | `90000`                           | overall serial capture timeout            |
| `--list-ports`   | —                                 | print candidate serial ports and exit     |

## Typical dev loop

```sh
# terminal 1: synthetic bridge
node tools/synth-pet-state.mjs --cycle 4000

# terminal 2: poke at it like the firmware would
curl -H "Authorization: Bearer the-same-long-random-string-as-the-firmware" \
  http://localhost:8788/pet/state
node tools/record-test-clip.mjs --url http://localhost:8788 --say "hi buddy"
node tools/capture-device-screen.mjs --out screenshots/ui-check.png

# terminal 3: rebuild + flash the firmware with PROXY_URL=http://localhost:8788
cd firmware && pio run -t upload
```

When you're ready to test the real flow, stop the synth tool, restart the
real bridge (`launchctl kickstart -k gui/$(id -u)/com.tokengochi.bridge`),
and re-flash with the production `PROXY_URL`.
