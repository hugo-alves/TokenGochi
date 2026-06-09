#pragma once
// Build-time configuration (NOT secrets). Edit and rebuild.

#ifndef PROXY_URL
// Cloudflare worker URL (HTTPS). Keep only local-only LAN URL here for local fallback.
// For staging/prod, flash the matching firmware build that points to that worker URL.
#define PROXY_URL "https://tokengochi-staging.pissa.workers.dev"
#endif

#ifndef POLL_INTERVAL_MS
#define POLL_INTERVAL_MS 30000   // how often to GET /pet/state
#endif

#ifndef HTTP_TIMEOUT_MS
#define HTTP_TIMEOUT_MS 8000     // HTTPClient.setTimeout
#endif

#ifndef TRANSCRIBE_TIMEOUT_MS
#define TRANSCRIBE_TIMEOUT_MS 30000  // Cloudflare -> Groq can take a few seconds
#endif

#ifndef TRANSCRIPT_LOG_RETENTION_DAYS
#define TRANSCRIPT_LOG_RETENTION_DAYS 7
#endif

#ifndef TRANSCRIPT_LOG_MAX_ENTRIES
#define TRANSCRIPT_LOG_MAX_ENTRIES 30
#endif

#ifndef TRANSCRIPT_LOG_TEXT_BYTES
#define TRANSCRIPT_LOG_TEXT_BYTES 512
#endif

#ifndef TRANSCRIPT_LOG_LANG_BYTES
#define TRANSCRIPT_LOG_LANG_BYTES 12
#endif

// Round AMOLED is 466x466 with a ~233 px visible radius. We clip drawing
// to the inscribed circle so nothing leaks into the chassis corners.
#define SCREEN_W      466
#define SCREEN_H      466
#define SCREEN_CX     (SCREEN_W / 2)
#define SCREEN_CY     (SCREEN_H / 2)
#define SCREEN_R      233
