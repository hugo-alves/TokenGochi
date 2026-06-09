# Cloudflare Worker backend

This folder hosts a Cloudflare Worker replacement for the LAN bridge.
The watch talks to a public HTTPS endpoint only, while the Mac stays
outbound-only (publishing token snapshots via `token-ingest.mjs`).

## Files

- `package.json` — wrangler scripts
- `wrangler.jsonc` — staging/production environments and D1 binding names
- `migrations/0001_initial.sql` — first schema for pet/token state and transcripts
- `src/index.ts` — API handlers and contract
- `src/auth.ts` — bearer-token helpers
- `src/pet.ts` — mood + payload types
- `src/storage.ts` — D1-backed pet state and token total persistence
- `src/groq.ts` — Groq transcription proxy and WAV duration parsing

## Environment secrets

Set these per environment with `wrangler secret put`:

- `DEVICE_TOKEN` (watch/device auth)
- `INGEST_TOKEN` (Mac outbound publish auth)
- `GROQ_API_KEY` (required for `/transcribe`)

## Deploy flow

1. Install wrangler: `npm install`
2. Create and bind staging/prod D1 DBs in `wrangler.jsonc`.
3. Set secrets for the selected environment.
4. `npm run deploy:staging` or `npm run deploy:production`

Always smoke-test staging before promoting flashed firmware to production.
