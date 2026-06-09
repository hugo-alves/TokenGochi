# Cloudflare Worker backend

This folder hosts a Cloudflare Worker replacement for the LAN bridge.
The watch talks to a public HTTPS endpoint only. Token totals can arrive from
the legacy outbound publisher (`token-ingest.mjs`) or from a VPS token source
that Cloudflare pulls on a cron and on stale watch reads.

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
- `TOKEN_SOURCE_URL` (public HTTPS URL for the VPS token source; either the
  base origin or the exact `/tokens_today` URL is accepted)
- `TOKEN_SOURCE_TOKEN` (required when Cloudflare pulls from a VPS source)

Set these non-secret vars per environment in `wrangler.jsonc`:

- `TOKEN_SOURCE_MAX_AGE_SECONDS` — how old stored tokens can be before a watch
  read attempts a refresh; default is 75 seconds

## Deploy flow

1. Install wrangler: `npm install`
2. Create and bind staging/prod D1 DBs in `wrangler.jsonc`.
3. Set secrets for the selected environment.
4. `npm run deploy:staging` or `npm run deploy:production`

Always smoke-test staging before promoting flashed firmware to production.

## Endpoints

| method | path | auth | purpose |
|---|---|---|---|
| GET | `/health` | none | Worker health and configuration flags |
| GET | `/tokens_today` | `DEVICE_TOKEN` | watch token totals |
| GET | `/pet/state` | `DEVICE_TOKEN` | watch pet state |
| POST | `/pet/reset` | `DEVICE_TOKEN` | reset pet state |
| POST | `/transcribe` | `DEVICE_TOKEN` | proxy watch WAV audio to Groq |
| POST | `/ingest/tokens` | `INGEST_TOKEN` | legacy push ingestion |
| POST | `/ingest/pull` | `INGEST_TOKEN` | pull one snapshot from the configured VPS token source |

## VPS pull mode

Run `bridge/token-source.mjs` on the VPS where Codex is logged in. It reads the
VPS-local `~/.codex` / `~/.claude` logs by shelling out to
`tamagotchi-bridge.mjs --once` and serves only token snapshots.

The VPS Tailscale IP is useful for operator SSH, but a Cloudflare Worker cannot
fetch that private tailnet address directly. Expose the token source through
Cloudflare Tunnel, Tailscale Funnel, or another HTTPS route, then set the
Worker `TOKEN_SOURCE_URL` to that public HTTPS URL. For the currently verified
VPS at `100.78.209.61`, read-only checks showed the Tailscale DNS name
`g33k-kid-agent.taild47216.ts.net`, so the expected Funnel base URL is
`https://g33k-kid-agent.taild47216.ts.net` after Funnel is enabled.

Required Worker values:

```sh
cd cloudflare
wrangler secret put TOKEN_SOURCE_URL --env staging
wrangler secret put TOKEN_SOURCE_TOKEN --env staging
npm run deploy:staging
```

Manual pull smoke:

```sh
curl -X POST "$CLOUDFLARE_WORKER_URL/ingest/pull" \
  -H "Authorization: Bearer $INGEST_TOKEN"
```

End-to-end smoke:

```sh
CLOUDFLARE_WORKER_URL=... \
DEVICE_TOKEN=... \
INGEST_TOKEN=... \
TOKEN_SOURCE_URL=https://your-vps-token-source.example.com \
TOKEN_SOURCE_TOKEN=... \
node ../tools/cloud-vps-smoke.mjs
```

The guarded rollout helper prints the same VPS commands by default and only
executes them when passed both `--apply` and `--yes`:

```sh
node ../tools/rollout-vps-token-source.mjs
node ../tools/rollout-vps-token-source.mjs --apply --yes
```
