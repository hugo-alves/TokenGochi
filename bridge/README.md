# Bridge

Self-contained Node script (`tamagotchi-bridge.mjs`) that reads local Claude
Code / Codex CLI transcript token counts, derives pet stats, and optionally
proxies audio to Groq Whisper. No `npm install`, no background workers, no
dependencies beyond the Node 18+ stdlib.

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
cp .env.example .env
# Set DEVICE_TOKEN to: openssl rand -hex 32
node tamagotchi-bridge.mjs
```

The HTTP server binds to `127.0.0.1` by default. Set `HOST=0.0.0.0` only when
the watch must connect directly over a trusted LAN.

One-shot snapshot (prints tokens + state, exits):

```sh
node tamagotchi-bridge.mjs --once
```

## Experimental Codex account usage

Local transcript-log scanning is the default. An experimental mode can read an
existing Codex access token and call an unsupported account-usage endpoint:

```sh
TOKEN_USAGE_SOURCE=codex-account
EXPERIMENTAL_CODEX_ACCOUNT_USAGE=1
```

This mode is opt-in, never refreshes or rewrites `~/.codex/auth.json`, and may
stop working without notice. The endpoint reports percentages rather than raw
tokens, so TokenGochi maps `metric_used_percent * 1000` into the existing food
contract.

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

The installer requires `.env` with a non-placeholder `DEVICE_TOKEN` of at
least 32 characters, restricts `.env` to the current user, and writes the
generated plist with user-only permissions.

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
runtime logs to `ingest.log`. Secrets stay in the user-only `.env`; they are
not copied into the plist.

## VPS token-source server

`token-source.mjs` is the pull-mode equivalent for a VPS. It serves the same
snapshot shape from `GET /tokens_today`, including optional `usage` metadata,
authenticated with `TOKEN_SOURCE_TOKEN`.

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

The token source binds to `127.0.0.1` by default. Keep it on loopback and
publish it through Cloudflare Tunnel, Tailscale Funnel, or another
authenticated HTTPS route. Configure the Worker `TOKEN_SOURCE_URL` with that
public HTTPS URL; a Worker cannot fetch a private tailnet address directly.

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
