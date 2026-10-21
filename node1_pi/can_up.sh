#!/bin/sh
# Bring up both MCP2515 interfaces with automatic bus-off recovery.
# Usage: ./can_up.sh [bitrate]   (default 500000)
set -e
BITRATE=${1:-500000}
for IF in can0 can1; do
    sudo ip link set "$IF" down 2>/dev/null || true
    sudo ip link set "$IF" up type can bitrate "$BITRATE" restart-ms 100
    DEV=$(basename "$(readlink /sys/class/net/$IF/device)" 2>/dev/null)
    case "$DEV" in
        spi0.0) BUS="bus A (CE0)" ;;
        spi0.1) BUS="bus B (CE1)" ;;
        *) BUS="?" ;;
    esac
    echo "== $IF = $BUS"
    ip -details link show "$IF" | grep -E "state|bitrate" | sed 's/^ */  /'
done
