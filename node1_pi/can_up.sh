#!/bin/sh
# Bring up both MCP2515 interfaces with automatic bus-off recovery.
# Usage: ./can_up.sh [bitrate]   (default 500000)
#
# Interfaces are found by chip select (bus A = spi0.0 / CE0, bus B = spi0.1 /
# CE1), since can0/can1 can swap between boots. If one won't come up (e.g.
# "RTNETLINK answers: Connection timed out": the MCP2515 ignored the driver's
# mode change), its driver is unbound and rebound. The re-probe sends the chip
# an SPI RESET, which is what a reboot does and ip link down/up does not.
BITRATE=${1:-500000}
DRV=/sys/bus/spi/drivers/mcp251x
rc=0

ifname() { ls "/sys/bus/spi/devices/$1/net" 2>/dev/null | head -1; }
log()    { sudo dmesg | grep -i mcp251 | tail -3 | sed 's/^/   dmesg: /' >&2; }
up()     { sudo ip link set "$1" down 2>/dev/null || true
           sudo ip link set "$1" up type can bitrate "$BITRATE" restart-ms 100; }

for SPI in spi0.0 spi0.1; do
    case $SPI in spi0.0) BUS="bus A (CE0)" ;; *) BUS="bus B (CE1)" ;; esac
    IF=$(ifname $SPI)
    if [ -z "$IF" ] || ! up "$IF"; then
        echo "!! $BUS ${IF:-(no interface)} would not come up; re-probing $SPI" >&2
        log
        [ -n "$IF" ] && echo "$SPI" | sudo tee $DRV/unbind >/dev/null
        echo "$SPI" | sudo tee $DRV/bind >/dev/null
        sleep 0.5
        IF=$(ifname $SPI)
        if [ -z "$IF" ] || ! up "$IF"; then
            echo "!! $BUS still down after the re-probe: check that module's SPI wiring and power" >&2
            log
            rc=1
            continue
        fi
        echo "   $BUS back after re-probe. Restart ./cockpit: it still holds the old interface" >&2
    fi
    echo "== $IF = $BUS"
    ip -details link show "$IF" | grep -E "state|bitrate" | sed 's/^ */  /'
done
exit $rc
