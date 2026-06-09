#!/bin/sh
set -e

PLIST_DST="$HOME/Library/LaunchAgents/com.tokengochi.ingest.plist"
launchctl unload "$PLIST_DST" 2>/dev/null || true
rm -f "$PLIST_DST"
echo "removed: $PLIST_DST"
