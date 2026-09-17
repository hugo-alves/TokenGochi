# TokenGochi launcher

## Scope

A launcher-first implementation, not a completed five-app firmware release.
Pet routes back to the existing firmware experience. Steady is a visibly labelled
navigation preview, with no game logic. Vibe, Sonifier, and Tap Tap are dimmed,
marked SOON, and do not open a fabricated app. Touching them explains that they
are not built. The existing boot destination remains Pet.

The artwork is derived from the supplied reference and laid out at 466 × 466
inside the round display. The larger Pet tile, pixel-art labels, irregular rims,
colours, black background, and subdued central pet are preserved. Keyboard/button
focus is indicated with small pixel brackets. Touch regions are separate,
non-overlapping ellipses; framebuffer corners cannot receive app launches.

## Integration

`main.cpp` gains `Mode::LAUNCHER` and includes `app_launcher_runtime.inc` before
`setup()`. The include is a deliberately small seam into the existing file's
private state and services; it is not a second independently compiled runtime.
The same include is compiled by the host integration harness against mocks.
Existing pet, recording, transcript, reset, and settings implementation bodies
are retained apart from the narrowly specified entry/exit and power hooks.

The firmware consists of a fixed manifest, a hardware-independent state model,
a templated renderer, an M5 drawing/peripheral adapter, and the runtime seam.
The model and renderer use no heap allocations. The image data is generated,
paletted RGB565 with row run-length encoding in constant storage. Firmware draws
into `ui::target()` and calls `ui::flush()`; it does not allocate another display
sprite or bypass the existing TGSHOT mirror. Check `launcher-assets.json` for
actual generated data sizes; these are not measurements of the final firmware.

The launcher/Steady preview own no active microphone, speaker, vibration, or
network operation. Entry and exit quiet the idle peripherals; active recording
is not an allowed entry state. The synchronous reconnect watchdog is skipped
while the launcher is active, and wake-triggered reconnect requests are deferred
until Pet resumes. Normal auto-dim/display sleep applies. A disconnected radio
can be powered off while the launcher is passively asleep, even with an overdue
pet poll. Pet state is not refreshed while the launcher is open. Initial boot and
other existing screens can still block on Wi-Fi; this change does not refactor
all network I/O.

The rectangular M5StickC target cannot enter the new launcher. Its prior boot
and input routes remain selected. Its complete firmware build still needs checking.

## Controls

| Context | Input | Result |
| --- | --- | --- |
| Pet, voice idle, or stats | Hold A for at least 800 ms, then release | Open launcher |
| Launcher | Tap Pet / short A with Pet focused | Return to existing Pet |
| Launcher | Short B | Cycle Pet and Steady preview; skip unavailable apps |
| Launcher | Tap Steady / short A with Steady focused | Open labelled preview |
| Steady preview | Short A/B, hold A, or visible Back button | Return to grid |
| Launcher grid | Hold A, then release | Return to Pet |
| Safe entry screens, including launcher | Hold A+B | Existing settings chord |
| Sleeping display | First button/touch gesture | Wake only; consume its release |

Single-A holds are resolved on release so an A+B chord observed at any point
wins. The gesture that opens a screen cannot also activate an item in it. The
new shortcut is disabled during recording, transcription, reset confirmation,
transcript display/history, errors, and settings. Short-A history, short-B voice,
and B-hold stats remain owned by the existing Pet logic.

For device captures, send the exact serial line `TGLAUNCHER` followed by newline
from a safe entry state. Then use the existing capture tool. It does not force
entry from recording, confirmation, or the compact target.

## Local checks

From the repository root:

```sh
tools/test-launcher.sh
cd firmware
../tools/pio test -e native
../tools/pio run -e m5stack-stopwatch
../tools/pio run -e m5stickc-plus2
```

The standalone host runner uses C++11, strict warnings, and AddressSanitizer /
UndefinedBehaviorSanitizer by default. Use `SANITIZE=0` only on a host where those
sanitizers are unavailable. The Unity model suite is also discoverable by the
existing native PlatformIO test environment and needs no change to its source
filter because the state model is header-only.

These are distinct checks: host tests are not an ESP32 build, a firmware build
is not a flash, and a flash alone does not verify touch, power, or audio behavior.

## Required physical verification

Use the existing local credentials and configuration; this bundle provides no
credentials and does not modify secrets or backend deployment settings.

```sh
cd firmware
../tools/pio run -e m5stack-stopwatch -t upload
cd ..
node tools/capture-device-screen.mjs
```

Capture and inspect Pet, the grid with both focus states, Steady preview, an
unavailable-app notice, and any low-battery overlay. Confirm long-A and A+B
precedence, return-to-Pet behavior, unchanged history/voice/stats, refusal to enter
from recording/reset confirmation, wake-without-activation, idle dim/sleep,
Wi-Fi loss/recovery, and silence on exit. Exercise physical touch near tile
boundaries, not only serial navigation. Repeat sleep checks without a USB serial
host, as that affects the existing power policy.

## Artwork maintenance

Regenerate checked-in assets with:

```sh
python3 tools/generate-launcher-assets.py /path/to/the-original-reference.png
```

The generator uses Pillow as a development-only dependency and expects the
original 1254 × 1254 image. Firmware has no new runtime library dependency. The
source package contains the original under `artwork/reference.png`.

## Verification on 2026-09-17

The integration was applied to the full checkout at base commit
`55636b3b47aed0f86931bffe135d567964eaf9e8`.

Verified: `bash tools/test-launcher.sh` passed model tests (563,898 assertions),
renderer/asset checks, and both round and compact runtime integration harnesses.
The StopWatch firmware build passed: 67,500 bytes static RAM and 1,485,489 bytes
application flash reported by PlatformIO.

The resulting 1,485,904-byte image was flashed to the connected ESP32-S3 at
`0x10000` using esptool with `--before no_reset --after hard_reset write_flash`.
Esptool reported `Hash of data verified`. The device-specific backend URL was
supplied as a build-time override; the repository example remains unchanged.

Not verified: live launcher rendering, physical input/audio/power behavior, and
the complete M5StickC firmware build. Serial launcher confirmation received no
response, and the round-masked screen capture attempt timed out waiting for the
TGSHOT header. Successful flashing does not establish successful UI operation.
