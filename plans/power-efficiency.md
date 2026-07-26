# Power Efficiency Plan — TokenGochi Firmware (M5Stack StopWatch C152)

## Overview

The TokenGochi firmware (ESP32-S3, `firmware/src/`) currently runs every major
power domain — WiFi radio, CPU, AMOLED display, audio codec — in its most
expensive configuration. The device is a wearable-form-factor virtual pet that
polls a backend every 30 s and is otherwise idle most of the time, so the idle
power profile dominates battery life. This plan ranks the changes by impact.

Power consumers on this hardware:

- ESP32-S3 WiFi radio (active RX ≈ 80–100 mA when modem sleep is off)
- ESP32-S3 CPU @ 240 MHz default
- CO5300 round AMOLED, 466×466 (power scales with lit pixels)
- ES8311 audio codec + MEMS mic
- CST820B touch controller, vibration motor (negligible at idle)

## Phase 1 — Quick wins (no UX change)

### 1.1 Enable WiFi modem sleep

`firmware/src/main.cpp:197` calls `WiFi.setSleep(false)`, keeping the radio in
continuous active receive. Change to:

```cpp
WiFi.setSleep(true);  // WIFI_PS_MIN_MODEM
```

Expected: idle radio draw drops from ~80–100 mA to ~15–25 mA. Cost: a few
hundred ms extra latency on the first packet of each HTTP request — invisible
at a 30 s poll cadence.

Acceptance: HTTP polling and `/transcribe` uploads still succeed reliably;
measure battery current before/after.

### 1.2 Drop CPU frequency to 80 MHz

The main loop (`firmware/src/main.cpp:710`) wakes every 20 ms to poll buttons.
Nothing requires 240 MHz; heaviest work is TLS handshakes and WAV muxing.

```cpp
setCpuFrequencyMhz(80);  // in setup(); 80 MHz is the WiFi minimum
```

Expected: ~20–30 mA savings. Acceptance: recording, TLS polling, and UI redraw
latency remain acceptable.

## Phase 2 — Display power management

### 2.1 Idle dim and screen-off

No `setBrightness` call exists anywhere; the AMOLED runs at the M5Unified
default forever. Add a display power state machine:

- After ~30 s without button/touch input: `M5.Display.setBrightness(40)`.
- After ~3 min: `M5.Display.sleep()` (CO5300 has a hardware sleep mode).
- Wake on KEYA/KEYB press or touch INT (G13); restore brightness.
- Background polling continues while the screen is off.
- Pause the blink animation (`firmware/src/main.cpp:793`) while dimmed/off so
  the QSPI display pipeline goes fully quiet.

### 2.2 Loop cadence while screen off

With the screen off, relax the main loop `delay(20)` to `delay(100)` or more.
M5Unified button debouncing tolerates this.

## Phase 3 — Network efficiency

### 3.1 Avoid a TLS handshake on every poll

`firmware/src/net.cpp:20` (`beginRequest`) reuses a static `WiFiClientSecure`
but each request constructs a fresh `HTTPClient` and calls `http.end()`,
forcing a full TLS renegotiation with the Cloudflare worker every 30 s
(~2–4 s of combined radio + crypto burst per poll).

- Make the `HTTPClient` persistent and call `http.setReuse(true)` so
  keep-alive holds the connection between polls.
- Verify Cloudflare's edge honors keep-alive across a 30 s gap; if not,
  prefer 3.2.

### 3.2 Adaptive poll interval

`POLL_INTERVAL_MS` is fixed at 30 s (`firmware/src/config.h:11`). Back off
when nothing is happening:

- 30 s while the user recently interacted or pet state is changing.
- 2–5 min when idle (mood changes slowly; staleness is invisible).

### 3.3 Reconnect scan backoff

The WiFi watchdog (`firmware/src/main.cpp:720`) runs a full blocking scan every
10 s while disconnected. Scans are high-power radio events. Use exponential
backoff: 10 s → 30 s → 2 min cap.

## Phase 4 — Peripheral housekeeping

### 4.1 Audio codec idle drain

`audio::init()` (`firmware/src/audio.cpp:105`) starts `M5.Speaker` at boot and
it stays enabled except while recording. Chirps are rare; call
`M5.Speaker.begin()` on demand before a chirp and `M5.Speaker.end()` after.

## Future / stretch

- Automatic light sleep via `esp_pm_configure` + tickless idle so the chip
  sleeps inside `delay()` while keeping WiFi associated.
- Deep sleep with EXT/GPIO button wake for a true watch standby mode (UX
  trade-off: WiFi reconnect on wake).

## Measurement & verification

- Use `M5.Power.getBatteryCurrent()` (AXP power IC) to log average current
  over ~60 s in each configuration; record before/after for every phase.
- Regression checks per phase: poll succeeds, `/transcribe` upload succeeds,
  buttons/touch responsive, chirp + vibration still work.

## Acceptance criteria

- Idle current reduced by >50% after Phase 1+2 (measured via battery current).
- No functional regressions in the README button/touch matrix.
- Screen wakes within ~200 ms of button/touch.
