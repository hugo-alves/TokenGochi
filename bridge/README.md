# Bridge

Self-contained Node script (`tamagotchi-bridge.mjs`) that reads your local
Claude Code / Codex CLI transcripts, derives pet stats, and optionally
proxies audio to Groq Whisper. No `npm install`, no background workers, no
deps beyond the Node 18+ stdlib.

For Cloudflare deployments, use `token-ingest.mjs` to publish only token
totals to a public Worker while all local file reads stay on the Mac.

## Endpoints

| method | path           | auth | returns |
|--------|----------------|------|-------------------------------------------------------------|
| GET    | `/health`      | no   | `{ok, version, groq_configured, whisper_model}` |
| GET    | `/tokens_today`| yes  | `{tokens_today, breakdown: {claude, codex}, ts}` |
| GET    | `/pet/state`   | yes  | derived pet: `{mood, age_s, food_today, last_msg, ...}` |
| POST   | `/pet/reset`   | yes  | same shape as `/pet/state` |
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

## Cloudflare ingest client

`token-ingest.mjs` reuses the bridge token parser and posts only
`/tokens_today` snapshots to the Worker ingest endpoint.

```sh
node token-ingest.mjs --once
INGEST_TOKEN=... CLOUDFLARE_WORKER_URL=... node token-ingest.mjs
```

Install/remove:

```sh
./install-ingest.sh
./uninstall-ingest.sh
```

It creates `~/Library/LaunchAgents/com.tokengochi.ingest.plist` and writes
runtime logs to `ingest.log`.

## Configuration

Copy `.env.example` to `.env` and edit. The bridge loads `.env` at startup;
process env always wins. See the file for available keys.

## Files

| file |
|---|
| `tamagotchi-bridge.mjs` |
| `token-ingest.mjs` |
| `_test_pure.mjs`, `_test_transcribe.mjs` |
| `com.tokengochi.bridge.plist` |
| `com.tokengochi.ingest.plist` |
| `install.sh`, `uninstall.sh` |
| `install-ingest.sh`, `uninstall-ingest.sh` |
| `state.json` |
| `bridge.log` |
| `ingest.log` |
| `.env` |
| `.env.example` |
| `.gitignore` |

## Tests

```sh
node _test_pure.mjs          # 11 logic tests (mood, bumpTotals, dates)
node _test_transcribe.mjs    # 5 e2e tests with a mock Groq server
```

The mock-Groq test spawns a tiny local server, points the bridge at it via
`GROQ_URL`, posts a synthesized WAV, and asserts both the response and the
state.json side effects. Use this to verify changes before flashing firmware.
