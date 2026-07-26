# Project status

TokenGochi is a public-beta-quality hardware project, not a turnkey consumer
product. It has three independently deployable surfaces:

- `firmware/`: M5Stack StopWatch and compact M5StickC-class firmware.
- `bridge/`: local token scanning, pet state, and optional Groq transcription.
- `cloudflare/`: optional Worker and D1 backend for HTTPS device access.

## Current capabilities

- Two documented firmware targets with battery-oriented defaults.
- Local Claude Code and Codex CLI transcript-log token counting.
- Authenticated local bridge and VPS token-source endpoints.
- Optional Cloudflare ingestion, persistence, and transcription proxy.
- Device settings, voice capture, transcript history, battery status, and
  serial framebuffer capture.

## Security defaults

- The local bridge and token source bind to loopback unless explicitly exposed.
- The bridge refuses missing, short, or placeholder device tokens.
- Secrets and runtime state are gitignored and installers restrict local secret
  files to the current user.
- Transcript text, bearer tokens, and upstream response bodies are not logged.
- Codex account-usage access is unsupported, read-only, disabled by default,
  and requires explicit experimental opt-in.

## Verification

The release checklist is:

```sh
node bridge/_test_pure.mjs
node bridge/_test_token_source.mjs
node bridge/_test_transcribe.mjs
cd cloudflare && npm run build
cd ../firmware && ../tools/pio test -e native
cd .. && gitleaks git . --redact
```

Firmware builds should also pass for both documented targets:

```sh
cd firmware
../tools/pio run -e m5stack-stopwatch
../tools/pio run -e m5stickc-plus2
```

Physical-device behavior, battery-life claims, staging, and production must be
reported as verified only when backed by current device or live-environment
evidence.

## Known limitations

- The account-usage endpoint is not a supported public API and may change.
- Voice transcription sends audio to the configured Groq endpoint.
- Users must provide their own Wi-Fi credentials, bearer tokens, Worker URL,
  D1 database IDs, Cloudflare secrets, and optional Groq key.
- Production deployment and hardware-specific behavior are environment-owned;
  this repository does not ship shared production credentials or infrastructure.
