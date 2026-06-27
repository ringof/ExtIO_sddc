#!/usr/bin/env bash
#
# usb_trace.sh — host-side USB lifecycle tracer for the RX888 (VID 04b4).
#
# Watches USB add/remove/reset/bind events and PID transitions
# (0x00f3 DFU <-> 0x00f1 loaded <-> gone) in real time, timestamped, WITHOUT
# claiming the device — so it is safe to run alongside radiod. Use it to see
# exactly what the firmware/device does (e.g. through a driver-initiated USB
# reset) while you launch radiod by hand in the container.
#
# Usage:
#   Terminal 1 (host):   tests/usb_trace.sh            # leave it running
#   Terminal 2 (host):   docker run --rm -it --name ka9q-dbg --privileged --network host \
#                          -v /dev/bus/usb:/dev/bus/usb -v /run/udev:/run/udev:ro \
#                          -v "$(pwd)/SDDC_FX3:/firmware" \
#                          -v "$(pwd)/wisdom:/var/lib/ka9q-radio" \
#                          ka9q-radio bash
#                        (inside)  radiod /etc/radio/radiod@rx888-test.conf
#
# Three timestamped streams, easy to correlate with radiod's own log:
#   UDEV   - udev add/remove/bind/unbind events (no root needed)
#   PID    - 04b4 PID transitions, printed only when they change
#   DMESG  - kernel USB log: resets, new-device, disconnect (needs root)
#
# Env: VID (default 04b4), POLL (PID poll interval s, default 0.3).

set -u
VID="${VID:-04b4}"
POLL="${POLL:-0.3}"
stamp() { date '+%H:%M:%S.%3N'; }

echo "# usb_trace VID=$VID  poll=${POLL}s  (Ctrl-C to stop)"
echo "$(stamp) START lsusb: $(lsusb -d "$VID": 2>&1 | paste -sd'; ' -)"

pids=()

# 1) udev events — no root required.
if command -v udevadm >/dev/null 2>&1; then
    stdbuf -oL udevadm monitor --udev --subsystem-match=usb 2>/dev/null \
        | while IFS= read -r l; do [ -n "$l" ] && echo "$(stamp) UDEV  $l"; done &
    pids+=($!)
else
    echo "# (udevadm not found — UDEV stream disabled)"
fi

# 2) PID-state poll — prints only on change, so 00f3<->00f1<->gone is explicit.
(
    prev="__init__"
    while true; do
        cur="$(lsusb -d "$VID": 2>/dev/null | paste -sd' | ' -)"
        [ -z "$cur" ] && cur="<none>"
        if [ "$cur" != "$prev" ]; then echo "$(stamp) PID   $cur"; prev="$cur"; fi
        sleep "$POLL"
    done
) &
pids+=($!)

# 3) Kernel USB log — needs root (sudo). Skipped cleanly if unavailable.
if [ "$(id -u)" = 0 ]; then DMESG="dmesg"
elif sudo -n true 2>/dev/null; then DMESG="sudo dmesg"
else DMESG=""; echo "# (dmesg skipped — run as root or enable passwordless sudo for kernel USB lines)"; fi
if [ -n "$DMESG" ]; then
    # --follow-new prints ONLY messages logged after we start (no backlog
    # dump). Falls back to --follow if the util-linux build lacks it.
    if $DMESG --follow-new --help >/dev/null 2>&1 || $DMESG --help 2>&1 | grep -q -- --follow-new; then
        DMESG_FOLLOW="--follow-new"
    else
        DMESG_FOLLOW="--follow"
        echo "# (dmesg lacks --follow-new; using --follow — expect a one-time backlog dump)"
    fi
    $DMESG $DMESG_FOLLOW 2>/dev/null \
        | grep --line-buffered -iE 'usb|xhci' \
        | while IFS= read -r l; do echo "$(stamp) DMESG $l"; done &
    pids+=($!)
fi

trap 'kill "${pids[@]}" 2>/dev/null' EXIT INT TERM
wait
