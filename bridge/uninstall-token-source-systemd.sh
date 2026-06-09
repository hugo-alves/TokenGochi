#!/usr/bin/env bash
set -euo pipefail

SYSTEMD_USER_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
SERVICE_DST="$SYSTEMD_USER_DIR/tokengochi-token-source.service"

systemctl --user disable --now tokengochi-token-source.service 2>/dev/null || true
rm -f "$SERVICE_DST"
systemctl --user daemon-reload

echo "removed tokengochi-token-source.service"
