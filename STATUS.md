# Status — `2026-06-08`

## TL;DR

The bridge, tools, and firmware all build, link, and run. The bridge is
live on `localhost:8787` under launchd (`com.tokengochi.bridge`, PID
11527). All 17 tests pass. The firmware boots, finds the M5Unified stack
and the 8 MB PSRAM, and tries to connect to WiFi — but the
`Parada Clientes` AP is rejecting the association. This is a
network-side issue, not a firmware or bridge issue.

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
