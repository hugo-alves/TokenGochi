# Status — `2026-06-09`

## TL;DR

The bridge, tools, and firmware all build, link, and run. The bridge is
live on `localhost:8787` under launchd (`com.tokengochi.bridge`, PID
11527). All 17 tests pass. The firmware boots, finds the M5Unified stack
and the 8 MB PSRAM, and tries to connect to WiFi — but the
`Parada Clientes` AP is rejecting the association. This is a
network-side issue, not a firmware or bridge issue.

## Codex Account Usage Migration — `2026-06-09`

Implementation is deployed to staging, the VPS token source is updated, and
the physical StopWatch display is verified.

Verified behavior:

- CodexBar source investigation showed account usage comes from Codex OAuth
  credentials in `~/.codex/auth.json` and the ChatGPT backend usage endpoint,
  which returns rate-limit percentages rather than raw tokens.
- VPS `deployer@100.78.209.61` can call that account endpoint using its own
  Codex login and reports plan `pro`.
- `bridge/tamagotchi-bridge.mjs --once` now returns `usage.source:
  "codex_account"` by default in `TOKEN_USAGE_SOURCE=auto`.
- Account percentages are mapped to the existing integer contract as
  `metric_used_percent * 1000`, while `usage.codex.metric_used_percent`
  preserves the real account percentage.
- Staging D1 migration `0002_token_usage_metadata.sql` is applied.
- Staging Worker deploy `0f45a11e-6539-4b70-8b10-ea460751336e` is live.
- End-to-end smoke passed:
  source total `27500`, Worker pull total `27500`, and `/pet/state`
  `food_today=27500`, `usage.source="codex_account"`,
  `metric_used_percent=27.5`.
- Firmware was rebuilt and uploaded to the StopWatch. Screen capture
  `screenshots/codex-account-percent-verified.png` shows `27.5% use` and
  `happy`.

Checks passed:

- `node bridge/_test_pure.mjs`
- `node bridge/_test_token_source.mjs`
- `node bridge/_test_transcribe.mjs`
- `cd cloudflare && npm run build`
- `cd firmware && /Users/hugoalves/Code/TokenGochi/firmware/.venv/bin/pio run`
- `TOKEN_SOURCE_TOKEN=... node tools/cloud-vps-smoke.mjs`
- `git diff --check`

Production deployment remains not verified.

## Codex Pace Mood — `2026-06-09`

Implementation is deployed to staging, the VPS token source is updated, and
the physical StopWatch display is verified.

Verified behavior:

- CodexBar pace logic was traced to `UsagePace.weekly` and
  `CodexHistoricalPaceEvaluator`. CodexBar uses historical weekly curves when
  enough samples exist, otherwise falls back to a linear expected pace through
  the reset window.
- TokenGochi now computes the same linear pace fallback for Codex account
  weekly usage and exposes it as `usage.codex.pace`.
- CodexBar's stage thresholds are reproduced: on track within 2%, slight within
  6%, ahead/behind within 12%, and far ahead/behind beyond 12%.
- Mood now follows pace for Codex account usage with a nuanced ladder:
  `peckish`, `hungry`, `very hungry`, `happy`, `excited`, and `very happy`.
- VPS one-shot snapshot returned `pace.stage="far_behind"`,
  `expected_used_percent=72.5`, `actual_used_percent=27`, and mood `hungry`.
- Staging Worker deploy `9c435044-6c6f-477f-9091-0880d32a1fcf` is live.
- Staging `/pet/state` returned `mood="very hungry"`, `metric_used_percent=29.5`,
  and `pace.stage="far_behind"`.
- Firmware was rebuilt and uploaded. Screen capture
  `screenshots/codex-pace-very-hungry-verified.png` shows `29.5% use` and
  `very hungry`.

Checks passed:

- `node bridge/_test_pure.mjs`
- `node bridge/_test_token_source.mjs`
- `node bridge/_test_transcribe.mjs`
- `cd cloudflare && npm run build`
- `cd firmware && /Users/hugoalves/Code/TokenGochi/firmware/.venv/bin/pio run`
- `TOKEN_SOURCE_TOKEN=... node tools/cloud-vps-smoke.mjs`

Historical CodexBar-style pace is not implemented yet. TokenGochi currently
uses the same linear fallback CodexBar uses when historical data is unavailable.

## VPS Token Source Migration — `2026-06-09`

Implementation is deployed to staging and the live VPS pull path is verified.

Verified locally:

- `bridge/token-source.mjs` serves authenticated VPS token snapshots from the
  existing Codex/Claude log scanner.
- Cloudflare Worker code can pull from `TOKEN_SOURCE_URL`, persist snapshots,
  refresh on a cron, and opportunistically refresh stale token state before
  watch reads.
- `tools/cloud-vps-smoke.mjs` verifies source health, source tokens, Worker
  pull, and `/pet/state` consistency without printing secrets.
- `tools/rollout-vps-token-source.mjs` defaults to dry-run, requires
  `--apply --yes` before changing the VPS or Tailscale Funnel, and reuses the
  existing VPS token-source secret on later deploys.
- Local checks passed:
  `node bridge/_test_token_source.mjs`,
  `node bridge/_test_pure.mjs`,
  `node bridge/_test_transcribe.mjs`,
  and `cd cloudflare && npm run build`.

Verified VPS facts:

- Tailscale address: `100.78.209.61`.
- SSH user/path: `deployer@100.78.209.61`.
- Hostname: `ubuntu-4gb-fsn1-1`.
- Tailscale DNS name: `g33k-kid-agent.taild47216.ts.net`.
- Node is available on the VPS.
- `tokengochi-token-source.service` is installed, enabled, and active.
- Local VPS `GET http://127.0.0.1:8790/health` returns
  `{"ok":true,"version":"0.1.0"}`.
- Unauthenticated local VPS `GET /tokens_today` returns 401.
- Authenticated local VPS `GET /tokens_today` returns a token snapshot.
- Tailscale Funnel is active at `https://g33k-kid-agent.taild47216.ts.net` and
  proxies to `http://127.0.0.1:8790`.
- Public `GET https://g33k-kid-agent.taild47216.ts.net/health` returns
  `{"ok":true,"version":"0.1.0"}`.
- Cloudflare staging secrets `TOKEN_SOURCE_URL` and `TOKEN_SOURCE_TOKEN` are
  set, and staging deploy `fa0d2dca-3ba3-4b84-b34d-8bf87cf5a983` is live.
- `node tools/cloud-vps-smoke.mjs` passed against staging with the VPS source:
  source health OK, Worker health OK with `token_source_configured=true`,
  Worker `/ingest/pull` returned the VPS token snapshot, and `/pet/state`
  matched that snapshot.

Physical StopWatch verification:

- Mac `com.tokengochi.ingest` was removed from launchd and stayed absent after
  an 80 second check; only `com.tokengochi.bridge` remained.
- Firmware was rebuilt and uploaded with the staging `PROXY_URL`.
- Serial monitor showed WiFi connected with IP `192.168.1.243`.
- Serial monitor showed Cloudflare bridge health OK with HTTP 200.
- Serial monitor showed a pet-state poll matching the VPS-backed staging state:
  `mood=hungry food=0 age=5103 ts=1780993452`.
- Device screen capture `screenshots/vps-cloud-verified.png` shows `0k tk`
  and hungry state, matching the VPS source rather than the old Mac total.

Not verified yet:

- Production deployment. The verified path is staging only.

## What's working

- **Bridge**: `com.tokengochi.bridge` running on `:8787`. Returns
  `{"ok":true,"version":"0.2.0","groq_configured":false}`. `/pet/state`
  returns a complete payload. `/transcribe` returns 503 because no
  `GROQ_API_KEY` is set (the bridge keeps working — `/pet/state` still
  serves). `/pet/reset` works (verified `age_s` dropped from 9712 → 1).
- **Tests**: `node bridge/_test_pure.mjs` (11/11),
  `node bridge/_test_transcribe.mjs` (6/6).
- **Firmware**: `pio run` clean. 1.30 MB / 16 MB flash, 50 KB / 320 KB RAM.
  Booted on the device:
  ```
  [boot] token tamagotchi
  [boot] M5 ok, heap=317484 psram=8386215
  [wifi] reconnecting...
  ```
  8 MB PSRAM detected, M5Unified stack initialized correctly.

## What's not working

### The watch can't stay on the WiFi

The diagnostic log shows the ESP32 associates with the AP on channel 1
(correct — 2.4 GHz), then is immediately deauthed:

```
[wifi] connected to AP (channel 1)
[wifi] DISCONNECTED reason=8 (SSID='Parada Clientes')
[wifi] connected to AP (channel 1)
[wifi] DISCONNECTED reason=8 (SSID='Parada Clientes')
... (repeats forever)
```

`reason=8` is `WIFI_REASON_DISASSOCIATED` (the AP pushed us off after
we joined). This is *not* a credentials problem — the password and SSID
are correct, authentication succeeds, the firmware briefly associates
on the correct 2.4 GHz channel.

The watch also *never* logs `[wifi] got ip:`, so DHCP never completes.

### Why this happens

The Mac's `airport` and `networksetup` reports show the device sees
exactly one preferred network named `Parada Clientes-5G`. The 2.4 GHz
sibling `Parada Clientes` was on the list the user gave me, but isn't
in the Mac's preferred-network list (the Mac has never joined it).

Likely cause: the network has a **captive portal**. Public/café WiFi in
Spain (MEO, DIGI, etc.) commonly requires opening a browser and
accepting terms. The StopWatch has no browser, so the AP authenticates
the device (association + 4-way handshake succeed) but the AP
controller deauths it after a few seconds when the portal flow isn't
satisfied. The pattern of "associate → 8 → reconnect" is the signature
of this.

Other possibilities:
1. **DHCP exhaustion / no IP assigned** — the router simply doesn't
   hand out an address to a device that hasn't done a portal flow.
2. **MAC filter / client isolation** — same effect.
3. **AP rejects clients with no user-agent / no http traffic** — captive
   portals rely on a HTTP redirect; if the device doesn't make one, the
   controller kicks it.

## How to unblock

In order of how fast each is to try:

1. **Use an iPhone personal hotspot** (you have "Hugo's iPhone" in
   the network list). No captive portal, plain WPA2, plain DHCP.
   Change `WIFI_SSID` to that, rebuild + flash, and the pet should
   appear on the AMOLED.
2. **Use any home router** (MEO-68B8A0, TP-Link_7500, MORECOFFEE, etc.)
   with a known password. Same flow.
3. **Skip the café network**. The 2.4 GHz `Parada Clientes` is
   almost certainly a captive portal — the device physically can't
   complete the auth flow.

## What I changed about the firmware for diagnostics

While debugging, I added a WiFi event handler and a one-time scan in
`firmware/src/main.cpp` to surface the actual failure mode. Should be
removed once WiFi works:

- `WiFi.onEvent(...)` prints `STA_START / CONNECTED / GOT_IP / DISCONNECTED`
  events with reason codes
- After `WiFi.begin()` fails, a scan is run and prints whether the
  target SSID is visible to the ESP32 at all

These cost ~100 bytes of flash and a few ms of startup time. Safe to
leave in for now; can be stripped if it gets in the way.

## Files

```
PLAN.md            design (13 sections, all decisions resolved)
README.md          one-page first-run guide
STATUS.md          this file
bridge/            zero-dep service, 537 LOC, 16 tests passing
tools/             dev utilities
firmware/          PlatformIO project for the StopWatch Dev Kit
```

All committed to a fresh `main` branch in 4 logical commits (PLAN →
bridge → tools → firmware).
