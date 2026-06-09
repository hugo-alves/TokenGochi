# Display Capture

TokenGochi screenshots come from the firmware-side `TGSHOT` serial command,
not from a camera. The captured framebuffer is `466x466`, but the physical
AMOLED is round.

## Geometry

The visible display area is modeled as:

```txt
width = 466
height = 466
center = (233, 233)
radius = 233
visible pixel if (x - 233)^2 + (y - 233)^2 <= 233^2
```

The largest rectangle guaranteed to fit inside the circle is:

```txt
x = 69..397
y = 69..397
size = 329x329
```

Detailed geometry lives in [`display-geometry.json`](display-geometry.json).

## Capturing

Default captures apply the circular alpha mask:

```sh
node tools/capture-device-screen.mjs
```

For debugging the full square framebuffer:

```sh
node tools/capture-device-screen.mjs --mask none
```

Tracked reference captures live under `docs/assets/screenshots/`. Routine
captures go under `screenshots/`, which is gitignored.
