#!/bin/sh
# install.sh — install the Token Tamagotchi bridge as a launchd agent.
# Writes ~/Library/LaunchAgents/com.tokengochi.bridge.plist with absolute
# paths baked in, then `launchctl load -w`s it. Idempotent: re-running just
# reloads the existing plist.
set -e
umask 077

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLIST_SRC="$SCRIPT_DIR/com.tokengochi.bridge.plist"
PLIST_DST="$HOME/Library/LaunchAgents/com.tokengochi.bridge.plist"
LOG_FILE="$SCRIPT_DIR/bridge.log"
ENV_FILE="$SCRIPT_DIR/.env"

# --- preflight --------------------------------------------------------------
NODE_BIN="$(command -v node || true)"
if [ -z "$NODE_BIN" ]; then
  echo "error: 'node' not found in PATH. Install Node 18+ first." >&2
  exit 1
fi

NODE_VERSION="$("$NODE_BIN" --version | sed 's/^v//')"
NODE_MAJOR="$(echo "$NODE_VERSION" | cut -d. -f1)"
if [ "$NODE_MAJOR" -lt 18 ]; then
  echo "error: Node 18+ required (found v$NODE_VERSION)" >&2
  exit 1
fi

if [ ! -f "$PLIST_SRC" ]; then
  echo "error: missing $PLIST_SRC" >&2
  exit 1
fi

if [ ! -f "$ENV_FILE" ]; then
  echo "error: missing $ENV_FILE" >&2
  echo "copy .env.example to .env and set a random DEVICE_TOKEN first" >&2
  exit 1
fi

DEVICE_TOKEN="$(sed -n 's/^DEVICE_TOKEN=//p' "$ENV_FILE" | head -n 1 | tr -d '\r')"
if [ "${#DEVICE_TOKEN}" -lt 32 ] || [ "$DEVICE_TOKEN" = "replace-with-a-random-device-token" ]; then
  echo "error: DEVICE_TOKEN must be a non-placeholder secret of at least 32 characters" >&2
  exit 1
fi
chmod 600 "$ENV_FILE"

# --- bake absolute paths into the plist -------------------------------------
mkdir -p "$(dirname "$PLIST_DST")"
awk \
  -v node="$NODE_BIN" \
  -v dir="$SCRIPT_DIR" \
  '{ gsub(/__NODE_BIN__/, node); gsub(/__BRIDGE_DIR__/, dir); print }' \
  "$PLIST_SRC" > "$PLIST_DST"

# --- unload any previous copy, then load fresh -------------------------------
launchctl unload "$PLIST_DST" 2>/dev/null || true
launchctl load -w "$PLIST_DST"

# --- friendly status --------------------------------------------------------
echo "installed: $PLIST_DST"
echo "node:      $NODE_BIN (v$NODE_VERSION)"
echo "dir:       $SCRIPT_DIR"
echo "log:       $LOG_FILE"
echo

echo "manage it:"
echo "  launchctl list | grep tokengochi    # status + pid"
echo "  tail -f $LOG_FILE                    # follow logs"
echo "  $SCRIPT_DIR/uninstall.sh             # remove"
