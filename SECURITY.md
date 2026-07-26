# Security policy

## Reporting a vulnerability

Use GitHub private vulnerability reporting for security issues. Do not include
tokens, credentials, private transcript content, audio, or infrastructure
details in a public issue.

If private reporting is unavailable, open a public issue containing only a
request for a private contact channel.

## Sensitive local data

Never commit or share:

- `bridge/.env`
- `firmware/src/secrets.h`
- `cloudflare/.dev.vars`
- `~/.codex/auth.json`
- bridge state or logs
- Cloudflare or Groq credentials

If any credential is exposed, revoke or rotate it at its provider before
cleaning Git history. Removing a secret from the latest commit does not remove
it from earlier commits.

## Trust boundaries

- The bridge reads local Claude Code and Codex CLI transcript files to count
  usage. It does not need transcript content outside the local process.
- Voice audio is sent to the configured Groq endpoint when transcription is
  enabled.
- Successful transcript text is stored on the device and in local pet state.
  Cloudflare mode also persists it in the operator's D1 database.
- The unsupported Codex account-usage integration is disabled by default,
  reads an existing access token without modifying `auth.json`, and should be
  treated as experimental.
- HTTP services bind to loopback by default. Expose them only through a trusted
  LAN or an authenticated HTTPS tunnel.

Only the latest `main` branch is supported with security fixes.
