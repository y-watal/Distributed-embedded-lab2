#!/bin/sh
# Bring up both MCP2515 interfaces with automatic bus-off recovery.
# Usage: ./can_up.sh [bitrate]   (default 500000)
set -e
BITRATE=${1:-500000}
for IF in can0 can1; do
    sudo ip link set "$IF" down 2>/dev/null || true
    sudo ip link set "$IF" up type can bitrate "$BITRATE" restart-ms 100
    echo "== $IF"
    ip -details link show "$IF" | grep -E "state|bitrate" | sed 's/^ */  /'
done
