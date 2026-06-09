# Bridge

Self-contained Node script (`tamagotchi-bridge.mjs`) that reads Codex account
usage through the same OAuth API path CodexBar uses, or falls back to local
Claude Code / Codex CLI transcripts, derives pet stats, and optionally proxies
audio to Groq Whisper. No `npm install`, no background workers, no deps beyond
the Node 18+ stdlib.

For Cloudflare deployments, either use `token-ingest.mjs` to publish token
totals to a public Worker, or run `token-source.mjs` on a VPS and let the
Worker pull snapshots from it.

## Endpoints

| method | path           | auth | returns |
|--------|----------------|------|-------------------------------------------------------------|
| GET    | `/health`      | no   | `{ok, version, groq_configured, whisper_model}` |
| GET    | `/tokens_today`| yes  | `{tokens_today, breakdown: {claude, codex}, usage?, ts}` |
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

## Codex account usage

`TOKEN_USAGE_SOURCE=auto` is the default. In that mode, the bridge first reads
`~/.codex/auth.json`, refreshes the OAuth token when needed, and calls the
Codex account usage endpoint. This is subscription-wide rate-limit usage, the
same class of data CodexBar shows, not per-machine session logs.

The account endpoint reports percentages rather than raw tokens. To preserve
the existing backend contract, TokenGochi maps the weekly
`metric_used_percent * 1000` to `breakdown.codex` and `food_today`, and also
includes `usage.codex.metric_used_percent` so firmware/UI can display the real
weekly percent.

The bridge also computes a CodexBar-style pace signal on the weekly window:
`usage.codex.pace.expected_used_percent`, `actual_used_percent`,
`delta_percent`, `balance_kind`, `balance_label`, `stage`, `eta_seconds`, and
`will_last_to_reset`. Until TokenGochi has enough historical samples to build
CodexBar's historical weekly curve, this uses CodexBar's linear fallback:
expected usage equals elapsed window percentage. Behind pace is reserve and
makes the pet very hungry; ahead pace is deficit and makes the pet very happy.
The current ladder is:

| pace stage | mood |
|---|---|
| `slightly_behind`, `behind`, `far_behind` | `very hungry` |
| `on_track` | `happy` |
| `slightly_ahead`, `ahead`, `far_ahead` | `very happy` |

Set `TOKEN_USAGE_SOURCE=local` to force the old transcript-log scanner.

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

## VPS token-source server

`token-source.mjs` is the pull-mode equivalent for a VPS. It serves the same
snapshot shape from `GET /tokens_today`, including optional `usage` metadata,
authenticated with
`TOKEN_SOURCE_TOKEN`.

```sh
cp .env.example .env
# set TOKEN_SOURCE_TOKEN to a long random value
TOKEN_SOURCE_TOKEN=... node token-source.mjs
curl http://127.0.0.1:8790/health
curl http://127.0.0.1:8790/tokens_today -H "Authorization: Bearer $TOKEN_SOURCE_TOKEN"
```

For a Linux VPS with user systemd:

```sh
./install-token-source-systemd.sh
systemctl --user status tokengochi-token-source.service
journalctl --user -u tokengochi-token-source.service -f
```

The service can bind on the VPS, but Cloudflare cannot reach a private
Tailscale IP such as `100.78.209.61` directly. Publish the local service with
Cloudflare Tunnel or Tailscale Funnel and configure the Worker
`TOKEN_SOURCE_URL` to the resulting public HTTPS token-source URL. On the
currently verified VPS, Tailscale reports `g33k-kid-agent.taild47216.ts.net`,
so the expected Funnel base URL is
`https://g33k-kid-agent.taild47216.ts.net`.

## Configuration

Copy `.env.example` to `.env` and edit. The bridge loads `.env` at startup;
process env always wins. See the file for available keys.

## Files

| file |
|---|
| `tamagotchi-bridge.mjs` |
| `token-ingest.mjs` |
| `token-source.mjs` |
| `_test_pure.mjs`, `_test_transcribe.mjs`, `_test_token_source.mjs` |
| `com.tokengochi.bridge.plist` |
| `com.tokengochi.ingest.plist` |
| `tokengochi-token-source.service` |
| `install.sh`, `uninstall.sh` |
| `install-ingest.sh`, `uninstall-ingest.sh` |
| `install-token-source-systemd.sh`, `uninstall-token-source-systemd.sh` |
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
node _test_token_source.mjs  # VPS token-source auth + snapshot contract
```

The mock-Groq test spawns a tiny local server, points the bridge at it via
`GROQ_URL`, posts a synthesized WAV, and asserts both the response and the
state.json side effects. Use this to verify changes before flashing firmware.
