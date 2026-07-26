#!/bin/sh
# install-ingest.sh — install token-ingest as a launchd agent.
set -e
umask 077

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLIST_SRC="$SCRIPT_DIR/com.tokengochi.ingest.plist"
PLIST_DST="$HOME/Library/LaunchAgents/com.tokengochi.ingest.plist"
ENV_FILE="$SCRIPT_DIR/.env"
LOG_FILE="$SCRIPT_DIR/ingest.log"

NODE_BIN="$(command -v node || true)"
if [ -z "$NODE_BIN" ]; then
  echo "error: node not found in PATH." >&2
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
  echo "error: copy .env.example to .env and set CLOUDFLARE_WORKER_URL / INGEST_TOKEN" >&2
  exit 1
fi

WORKER_URL="$(sed -n 's/^CLOUDFLARE_WORKER_URL=//p' "$ENV_FILE" | head -n 1 | tr -d '\r')"
INGEST_TOKEN="$(sed -n 's/^INGEST_TOKEN=//p' "$ENV_FILE" | head -n 1 | tr -d '\r')"
if [ -z "$WORKER_URL" ] || [ -z "$INGEST_TOKEN" ]; then
  echo "error: CLOUDFLARE_WORKER_URL and INGEST_TOKEN must be set in $ENV_FILE" >&2
  exit 1
fi
chmod 600 "$ENV_FILE"

mkdir -p "$(dirname "$PLIST_DST")"
awk \
  -v node="$NODE_BIN" \
  -v dir="$SCRIPT_DIR" \
  '{ gsub(/__NODE_BIN__/, node); gsub(/__BRIDGE_DIR__/, dir); print }' \
  "$PLIST_SRC" > "$PLIST_DST"

launchctl unload "$PLIST_DST" 2>/dev/null || true
launchctl load -w "$PLIST_DST"

echo "installed: $PLIST_DST"
echo "node:      $NODE_BIN (v$NODE_VERSION)"
echo "dir:       $SCRIPT_DIR"
echo "log:       $LOG_FILE"
echo
echo "manage it:"
echo "  launchctl list | grep tokengochi     # status + pid"
echo "  tail -f $LOG_FILE                    # follow logs"
echo "  $SCRIPT_DIR/uninstall-ingest.sh       # remove"
