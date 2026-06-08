# Bridge

Self-contained Node script (`tamagotchi-bridge.mjs`) that reads your local
Claude Code / Codex CLI transcripts, derives pet stats, and optionally
proxies audio to Groq Whisper. No `npm install`, no background workers, no
deps beyond the Node 18+ stdlib.

## Endpoints

| method | path           | auth | returns                                                     |
|--------|----------------|------|-------------------------------------------------------------|
| GET    | `/health`      | no   | `{ok, version, groq_configured, whisper_model}`             |
| GET    | `/tokens_today`| yes  | `{tokens_today, breakdown: {claude, codex}, ts}`            |
| GET    | `/pet/state`   | yes  | derived pet: `{mood, age_s, food_today, last_msg, ...}`     |
| POST   | `/transcribe`  | yes  | raw `audio/wav` body → Groq → `{text, duration_s, lang, ms_groq}` |

`POST /transcribe` requires `GROQ_API_KEY`; otherwise returns 503. Body is
capped at 1 MB (≈30 s of 16 kHz mono 16-bit PCM).

## Run

Foreground (one terminal tab):

```sh
node tamagotchi-bridge.mjs
```

One-shot snapshot (prints tokens + state, exits):

```sh
node tamagotchi-bridge.mjs --once
```

## Install as a launchd agent (recommended)

Survives reboots, restarts on crash, logs to `bridge.log`:

```sh
./install.sh
```

What it does:
1. Resolves the absolute path to your `node` binary.
2. Bakes `__NODE_BIN__` and `__BRIDGE_DIR__` into a copy of the plist template.
3. Writes `~/Library/LaunchAgents/com.tokengochi.bridge.plist`.
4. `launchctl load -w`s it (RunAtLoad + KeepAlive on crash).

Manage it:

```sh
launchctl list | grep tokengochi   # status + pid
tail -f bridge.log                  # follow logs
./uninstall.sh                      # stop + remove
```

If you don't have a `.env` yet, the install prints a reminder. The bridge
will still come up — just with the default `DEVICE_TOKEN`.

## Configuration

Copy `.env.example` to `.env` and edit. The bridge loads `.env` at startup;
the process env always wins. See the file for available keys.

## Tests

```sh
node _test_pure.mjs          # 11 logic tests (mood, bumpTotals, dates)
node _test_transcribe.mjs    # 5 e2e tests with a mock Groq server
```

The mock-Groq test spawns a tiny local server, points the bridge at it via
`GROQ_URL`, posts a synthesized WAV, and asserts both the response and the
state.json side effects. Use this to verify changes before flashing firmware.

## Files

| file                                       | what                                                  |
|--------------------------------------------|-------------------------------------------------------|
| `tamagotchi-bridge.mjs`                    | the bridge                                            |
| `_test_pure.mjs`, `_test_transcribe.mjs`   | tests                                                 |
| `com.tokengochi.bridge.plist`              | launchd template (placeholders: `__NODE_BIN__`, `__BRIDGE_DIR__`) |
| `install.sh`, `uninstall.sh`               | launchd installer / remover                           |
| `state.json`                               | pet persistent state (auto-created, gitignored)       |
| `bridge.log`                               | launchd stdout/stderr (auto-created, gitignored)      |
| `.env`                                     | your secrets (gitignored)                             |
| `.env.example`                             | template                                              |
| `.gitignore`                               | ignores `.env`, `state.json`, `bridge.log`            |
