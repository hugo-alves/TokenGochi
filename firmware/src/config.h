#pragma once
// Build-time configuration (NOT secrets). Edit and rebuild.

#ifndef PROXY_URL
// Mac LAN IP + bridge port. Find yours with `ipconfig getifaddr en0`.
#define PROXY_URL "http://192.168.4.132:8787"
#endif

#ifndef POLL_INTERVAL_MS
#define POLL_INTERVAL_MS 30000   // how often to GET /pet/state
#endif

#ifndef HTTP_TIMEOUT_MS
#define HTTP_TIMEOUT_MS 8000     // HTTPClient.setTimeout
#endif

// Round AMOLED is 466x466 with a ~233 px visible radius. We clip drawing
// to the inscribed circle so nothing leaks into the chassis corners.
#define SCREEN_W      466
#define SCREEN_H      466
#define SCREEN_CX     (SCREEN_W / 2)
#define SCREEN_CY     (SCREEN_H / 2)
#define SCREEN_R      233
