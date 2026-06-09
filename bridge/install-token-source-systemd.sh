#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICE_SRC="$SCRIPT_DIR/tokengochi-token-source.service"
SYSTEMD_USER_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
SERVICE_DST="$SYSTEMD_USER_DIR/tokengochi-token-source.service"
ENV_FILE="$SCRIPT_DIR/.env"
NODE_BIN="$(command -v node || true)"

if [[ -z "$NODE_BIN" ]]; then
  echo "node not found on PATH" >&2
  exit 1
fi

if [[ ! -f "$ENV_FILE" ]]; then
  echo "missing $ENV_FILE" >&2
  echo "copy .env.example to .env and set TOKEN_SOURCE_TOKEN first" >&2
  exit 1
fi

TOKEN_SOURCE_TOKEN="$(sed -n 's/^TOKEN_SOURCE_TOKEN=//p' "$ENV_FILE" | head -n 1 | tr -d '\r')"
if [[ -z "$TOKEN_SOURCE_TOKEN" ]]; then
  echo "TOKEN_SOURCE_TOKEN is not set in $ENV_FILE" >&2
  exit 1
fi

mkdir -p "$SYSTEMD_USER_DIR"
awk \
  -v node="$NODE_BIN" \
  -v dir="$SCRIPT_DIR" \
  -v env_file="$ENV_FILE" \
  '{ gsub(/__NODE_BIN__/, node); gsub(/__BRIDGE_DIR__/, dir); gsub(/__ENV_FILE__/, env_file); print }' \
  "$SERVICE_SRC" > "$SERVICE_DST"

systemctl --user daemon-reload
systemctl --user enable --now tokengochi-token-source.service

echo "installed $SERVICE_DST"
echo "status: systemctl --user status tokengochi-token-source.service"
echo "logs:   journalctl --user -u tokengochi-token-source.service -f"
