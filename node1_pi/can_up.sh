#!/bin/sh
# Reset both MCP2515s and bring both interfaces up with bus-off recovery.
# Usage: ./can_up.sh [bitrate]   (default 500000)
#
# Both modules are re-probed first (driver unbind/bind). The probe sends each
# chip an SPI RESET, so every start is as clean as a reboot; plain
# ip link down/up does not reset the chip, and a chip stuck after a jumper pull
# stays stuck through it. Re-probing gives new interfaces (new ifindex, maybe
# new can0/can1 names), so start or restart ./cockpit AFTER this script.
# Interfaces are found by chip select: bus A = spi0.0 (CE0), bus B = spi0.1 (CE1).
BITRATE=${1:-500000}
DRV=/sys/bus/spi/drivers/mcp251x
rc=0

ifname() { ls "/sys/bus/spi/devices/$1/net" 2>/dev/null | head -1; }
log()    { sudo dmesg | grep -i mcp251 | tail -3 | sed 's/^/   dmesg: /' >&2; }

if pgrep -x cockpit >/dev/null; then
    echo "note: cockpit is running; restart it after this (its sockets point at the old interfaces)" >&2
fi

for SPI in spi0.0 spi0.1; do
    [ -e "$DRV/$SPI" ] && echo "$SPI" | sudo tee $DRV/unbind >/dev/null
done
sleep 0.2
for SPI in spi0.0 spi0.1; do
    echo "$SPI" | sudo tee $DRV/bind >/dev/null 2>&1
done
sleep 0.5

for SPI in spi0.0 spi0.1; do
    case $SPI in spi0.0) BUS="bus A (CE0)" ;; *) BUS="bus B (CE1)" ;; esac
    IF=$(ifname $SPI)
    if [ -z "$IF" ]; then
        echo "!! $BUS: no interface after reset (probe failed): check that module's SPI wiring and power" >&2
        log; rc=1; continue
    fi
    if ! sudo ip link set "$IF" up type can bitrate "$BITRATE" restart-ms 100; then
        echo "!! $BUS $IF would not come up" >&2
        log; rc=1; continue
    fi
    echo "== $IF = $BUS"
    ip -details link show "$IF" | grep -E "state|bitrate" | sed 's/^ */  /'
done
exit $rc
