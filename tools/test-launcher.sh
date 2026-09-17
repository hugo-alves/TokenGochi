#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$(mktemp -d "${TMPDIR:-/tmp}/tokengochi-launcher.XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT
CXX="${CXX:-c++}"
FLAGS=(-std=c++11 -Wall -Wextra -Werror -pedantic -O1 -g -I"$ROOT/firmware/src")
if [[ "${SANITIZE:-1}" == "1" ]]; then FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"$CXX" "${FLAGS[@]}" -DLAUNCHER_STANDALONE "$ROOT/firmware/test/test_launcher/test_main.cpp" -o "$BUILD/model-tests"
"$BUILD/model-tests"
"$CXX" "${FLAGS[@]}" "$ROOT/tests/render_test.cpp" -o "$BUILD/render-tests"
OUT="${LAUNCHER_PREVIEW_DIR:-$BUILD/frames}"
mkdir -p "$OUT"
"$BUILD/render-tests" "$OUT"
for COMPACT in 0 1; do
    "$CXX" "${FLAGS[@]}" -DTOKENGOCHI_COMPACT_UI="$COMPACT" "$ROOT/tests/integration_test.cpp" -o "$BUILD/integration-$COMPACT"
    "$BUILD/integration-$COMPACT"
done
echo 'Native model, renderer and inserted-function integration tests passed. This is not an ESP32 firmware build.'
