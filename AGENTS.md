# Agent Guide: TokenGochi

This repo is a hardware plus backend project for a virtual pet that lives on an
M5Stack StopWatch Dev Kit and reacts to local Claude Code / Codex token usage.
Treat it as production-bound hardware work: verify before changing, keep edits
small, and never mix firmware, backend, deployment, and secret changes unless
the user explicitly asks for that scope.

## Repository Context

- Work from the repository root; do not assume a machine-specific absolute
  path.
- The default branch is `main`.
- Use `./tools/pio` for PlatformIO commands. It runs PlatformIO through `uv`
  with Python 3.12, avoiding host `pio` wrappers pinned to unsupported Python
  versions.

Status from `STATUS.md` is useful project context but should be re-verified
before making claims about a live device, deployment, or account.

## What This Project Is

TokenGochi has three runtime surfaces:

1. `firmware/`: PlatformIO Arduino firmware for the M5Stack StopWatch Dev Kit.
   It renders the pet on the round AMOLED, records audio from the watch mic,
   posts WAV audio to the backend, and polls pet state.
2. `bridge/`: zero-dependency Node 18+ local bridge. It reads local Claude Code
   and Codex logs, derives token totals and pet state, and can proxy audio to
   Groq Whisper.
3. `cloudflare/`: Cloudflare Worker plus D1 backend for public HTTPS access.
   The watch can talk to the Worker while the Mac publishes token totals
   outbound using `bridge/token-ingest.mjs`.

The product goal is a physical token-fed pet: it gets mood/state from usage,
shows stats on the watch, and can transcribe short voice interactions.

## Hardware And Host Setup

- Device: M5Stack StopWatch Dev Kit C152.
- MCU: ESP32-S3R8, 16 MB flash, 8 MB PSRAM.
- Display: 1.75 inch round AMOLED, 466x466, modeled as radius 233 centered at
  `(233, 233)`.
- Controls: KEYA on G2, KEYB on G1, plus power/reset.
- Audio: MEMS mic and speaker through the M5Unified/M5GFX stack.
- WiFi: 2.4 GHz only. Captive portals are not supported by the firmware.
- Development host: macOS for StopWatch flashing and launchd integration;
  Linux is supported for the optional VPS token source.

Prefer an iPhone hotspot or known home router for firmware WiFi verification.
Cafe/public WiFi with a browser portal can authenticate and then deauth the
watch before DHCP completes.

## Current Work And Known Constraints

- `firmware/src/config.h` contains an example Worker URL that each developer
  must replace or override for their environment.
- Local fallback is still supported by changing `PROXY_URL` to the Mac LAN URL,
  usually `http://<mac-lan-ip>:8787`.
- `GROQ_API_KEY` is not configured for the live local bridge unless
  `/health` says otherwise; `/transcribe` will return 503 without it.
- WiFi diagnostic logging was added in `firmware/src/main.cpp` during the last
  documented debugging pass. It is safe to leave until WiFi is verified on a
  non-captive network.
- Cloudflare staging and production must stay separate. Never point staging at
  production D1, production secrets, or production traffic.
- Local transcript-log token counting is the default. The unsupported Codex
  account-usage path is read-only, experimental, and requires explicit opt-in.
- Production deploys, migrations, and environment changes require explicit
  confirmation in the same turn.

## Secret And State Boundaries

Do not print, commit, or casually inspect secret-bearing files:

- `bridge/.env`
- `firmware/src/secrets.h`
- `cloudflare/.dev.vars`
- any `wrangler secret` values

Gitignored runtime state and logs include:

- `bridge/state.json`
- `bridge/bridge.log`
- `bridge/ingest.log`
- `firmware/.pio/`
- `screenshots/`
- `cloudflare/.wrangler/`
- `cloudflare/dist/`

If a secret appears to have been committed, stop and report it before doing any
more work. Rotation and history cleanup need explicit handling.

## Repo Entry Protocol

At the start of any task, verify and report:

- current directory
- whether this is a Git repo
- active branch
- Git status, including uncommitted and untracked files

Use `rtk git status --short` or native `git status --short`. If the working
tree is dirty, identify existing changes before editing and do not overwrite
or mix with them.

## Development Commands

Root-level orientation:

```sh
rtk git status --short
rtk git log --oneline -5
find . -maxdepth 2 -type f | sort
```

Bridge:

```sh
cd bridge
node _test_pure.mjs
node _test_transcribe.mjs
node tamagotchi-bridge.mjs --once
curl http://localhost:8787/health
```

Firmware:

```sh
cd firmware
../tools/pio run
../tools/pio run -t upload
../tools/pio device monitor
```

Cloudflare:

```sh
cd cloudflare
npm install
npm run build
npm run dev:staging
npm run deploy:staging
```

Device capture:

```sh
node tools/capture-device-screen.mjs
```

Use the exact surface relevant to the change. A bridge unit test does not
verify firmware, a firmware build does not verify the real watch, and local
success does not verify staging or production.

## UI And Firmware Constraints

- Keep screen layout inside the round 466x466 display. Use
  `docs/display-geometry.json` and `docs/display-capture.md` for geometry.
- Avoid assuming square corners are visible. Routine screenshots under
  `screenshots/` are gitignored.
- KEYA/KEYB behavior is documented in `firmware/README.md`; preserve the
  current interaction model unless the task is explicitly about changing it.
- Audio capture is short WAV audio for transcription. Respect memory and
  timeout limits in `firmware/src/config.h`.
- Prefer M5Unified abstractions over hand-wiring pins unless debugging hardware
  initialization itself.

## Backend Constraints

- The local bridge is intentionally zero-dependency Node 18+ stdlib code.
  Do not add npm dependencies to `bridge/` without a strong reason.
- Auth uses bearer tokens. Do not weaken auth for convenience.
- `/transcribe` depends on Groq. If `GROQ_API_KEY` is missing, 503 is expected
  and the bridge should keep serving state endpoints.
- The Worker stores state in D1. Run staging first and smoke-test before any
  production promotion.

## Verification Standard

Use `verified` only for claims backed by current command output, logs,
screenshots, device serial output, or browser/API evidence. Use `not verified`
for assumptions or status inherited from older docs.

For bug fixes:

1. Reproduce the failing path first when practical.
2. Make one scoped change.
3. Run the smallest relevant check.
4. Report exact evidence and current Git status.

For UI/device work, prefer real device evidence: serial monitor logs and
`tools/capture-device-screen.mjs` screenshots. If the device is unavailable,
say `not verified` for device behavior.

For firmware UI, touchscreen, display layout, sprite, or other watch-visible
changes, do not stop at a successful build when the physical StopWatch is
available. Flash the device, let it boot to the affected screen, then capture
and inspect at least one round-masked screenshot with
`tools/capture-device-screen.mjs`. Report the flash command, screenshot path,
and whether the live device result is `verified`. If flashing or capture is
blocked, report the blocker and mark live device verification as `not verified`.

## Git Discipline

- Do not commit automatically unless the user explicitly authorizes commits.
- Do not use `git add .` blindly. Stage by file or patch.
- Before committing, show files changed, a diff summary, and the proposed
  commit message.
- Never hide failing or unverified work in a commit.
- Before destructive Git operations, show the exact command and wait for
  explicit confirmation.

At handoff, report:

- branch
- latest commit made, if any
- uncommitted changes, if any
- checks run
- `verified` / `not verified` status
- known risks or follow-ups
