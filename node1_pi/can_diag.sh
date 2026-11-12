#!/bin/bash
# Timeline of both MCP2515 interfaces while you pull and restore a CAN jumper.
#
#   sudo ./can_diag.sh [seconds] > diag.txt     (default 40)
#
# Leave the cockpit running. At ~10 s pull the Pi's bus B jumper, at ~25 s put
# it back, then let it finish. Prints every 0.5 s, per bus: link state, CAN
# state + error counters, rx/tx packet deltas, interrupts taken (delta), the
# INT pin level, and the CPU used by that chip's kernel IRQ thread. At the end:
# kernel messages and an ip link down/up attempt on any bus that stopped.
DUR=${1:-40}
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }

declare -A IF IRQ PIN ITH
for d in /sys/class/net/can*; do
    n=$(basename "$d")
    case "$(basename "$(readlink "$d/device")")" in
        spi0.0) IF[A]=$n ;;
        spi0.1) IF[B]=$n ;;
    esac
done
PIN[A]=25; PIN[B]=24   # INT pins from config.txt (interrupt=25 / interrupt=24)

echo "== $(date)  kernel $(uname -r)  $(tr -d '\0' </proc/device-tree/model)"
grep -E '^[^#]*(dtparam=spi|mcp2515|spi)' /boot/firmware/config.txt
for b in A B; do
    i=${IF[$b]}
    [ -n "$i" ] || { echo "bus $b: no interface on spi0.$([ $b = A ] && echo 0 || echo 1)"; continue; }
    IRQ[$b]=$(awk -v s="spi0.$([ $b = A ] && echo 0 || echo 1)" '$NF==s {sub(":","",$1); print $1}' /proc/interrupts)
    ITH[$b]=$(ps -eo pid=,comm= | awk -v p="irq/${IRQ[$b]}-" 'index($2,p)==1 {print $1; exit}')
    echo "bus $b = $i (spi0.$([ $b = A ] && echo 0 || echo 1)), INT GPIO${PIN[$b]}, irq ${IRQ[$b]}" \
         "type $(cat /sys/kernel/irq/${IRQ[$b]}/type 2>/dev/null || echo ?), irq thread pid ${ITH[$b]:-?}"
    ip -details -statistics link show "$i" | sed 's/^/    /'
done
echo

NCPU=$(head -1 /proc/interrupts | wc -w)
irqs()  { awk -v n="$1" -v c="$NCPU" '$1==n":" {s=0; for(k=2;k<=c+1;k++) s+=$k; print s}' /proc/interrupts; }
stat()  { cat "/sys/class/net/$1/statistics/$2" 2>/dev/null || echo 0; }
level() { if command -v pinctrl >/dev/null; then pinctrl get "$1" | grep -oE '\b(hi|lo)\b' | head -1
          else raspi-gpio get "$1" | grep -oE 'level=[01]'; fi; }
ticks() { [ -n "$1" ] && awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }
canst() { ip -details link show "$1" | sed -n 's/.*can .*state \([A-Z-]*\).*berr-counter tx \([0-9]*\) rx \([0-9]*\).*/\1 tec=\2 rec=\3/p'; }

declare -A pr pt pi pc
for b in A B; do i=${IF[$b]}; [ -n "$i" ] || continue
    pr[$b]=$(stat "$i" rx_packets); pt[$b]=$(stat "$i" tx_packets); pi[$b]=$(irqs "${IRQ[$b]}"); pc[$b]=$(ticks "${ITH[$b]}")
done
printf '%-6s| %-62s| %s\n' "t(s)" "bus A: link  can-state  tec rec   +rx  +tx  +irq  INT  irqthr%" "bus B: same"
t0=$(date +%s.%N)
END=$(( $(date +%s) + DUR ))
while [ "$(date +%s)" -lt "$END" ]; do
    sleep 0.5
    line=$(awk -v a="$(date +%s.%N)" -v b="$t0" 'BEGIN {printf "%6.1f", a - b}')
    for b in A B; do i=${IF[$b]}; [ -n "$i" ] || continue
        r=$(stat "$i" rx_packets); t=$(stat "$i" tx_packets); q=$(irqs "${IRQ[$b]}"); c=$(ticks "${ITH[$b]}")
        line+=$(printf '| %-5s %-26s %5d %4d %5d  %-3s %4d%% ' "$(cat /sys/class/net/$i/operstate)" "$(canst "$i")" \
                 $((r - pr[$b])) $((t - pt[$b])) $((q - pi[$b])) "$(level "${PIN[$b]}")" $(( (c - pc[$b]) * 2 )))
        pr[$b]=$r; pt[$b]=$t; pi[$b]=$q; pc[$b]=$c
    done
    echo "$line"
done

echo; echo "== kernel messages (last 5 min)"
dmesg --since "-5min" 2>/dev/null | grep -iE 'mcp251|spi|can[01]|irq|nobody cared|hung' | tail -40 \
    || dmesg | grep -iE 'mcp251|spi|can[01]|irq|nobody cared|hung' | tail -40

echo; echo "== recovery test: ip link down/up on each bus whose rx stopped"
for b in A B; do i=${IF[$b]}; [ -n "$i" ] || continue
    r0=$(stat "$i" rx_packets); sleep 1; r1=$(stat "$i" rx_packets)
    [ "$r1" -gt "$r0" ] && { echo "bus $b ($i): receiving ($((r1 - r0))/s), left alone"; continue; }
    echo "bus $b ($i): no rx in 1 s, bouncing"
    timeout 5 ip link set "$i" down; echo "  down -> exit $?"
    timeout 5 ip link set "$i" up type can bitrate 500000 restart-ms 100; echo "  up   -> exit $?"
    sleep 1; r2=$(stat "$i" rx_packets); sleep 1; r3=$(stat "$i" rx_packets)
    echo "  after bounce: $((r3 - r2)) rx/s, $(canst "$i"), INT GPIO${PIN[$b]} $(level "${PIN[$b]}")"
    dmesg | tail -5 | sed 's/^/  dmesg: /'
done
