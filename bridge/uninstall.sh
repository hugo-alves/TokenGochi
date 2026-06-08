#!/bin/sh
# uninstall.sh — stop and remove the launchd agent.
set -e

PLIST="$HOME/Library/LaunchAgents/com.tokengochi.bridge.plist"

if [ -f "$PLIST" ]; then
  launchctl unload "$PLIST" 2>/dev/null || true
  rm "$PLIST"
  echo "removed: $PLIST"
else
  echo "not installed: $PLIST"
fi
