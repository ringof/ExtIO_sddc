#!/bin/bash
#
# ka9q.sh — helper for the ka9q-radio Docker image.
#
# Subcommands:
#   start [--vhf]          Launch the container detached. With --vhf, radiod
#                          runs the VHF/FM config (rx888-vhf-fm.conf: a WBFM
#                          receiver at the 4.57 MHz R828D IF) instead of the
#                          default HF test config. Then tune the front end with
#                          ../../vhf_fm_tune.sh and listen to fm-pcm.local.
#   console                Drop into a bash shell inside the running container.
#   monitor [stream-name]  Run ka9q's `monitor` inside the container with
#                          host-side ALSA playback.  Default: wwv-pcm.local
#   listen  [stream-name]  Fallback: play with cvlc on the host (often fails
#                          because ka9q uses dynamic RTP payload types VLC
#                          can't decode without an SDP file).  Default:
#                          wwv-pcm.local
#   stop                   Stop the container.
#   help                   Show this usage.
#
# Typical workflow:
#   ./ka9q.sh start                    # terminal A
#   ./ka9q.sh monitor                  # terminal B  (audio out via ka9q monitor)
#   ./ka9q.sh console                  # terminal C
#     # inside the container:
#     control hf.local                 # curses tuner UI
#

set -euo pipefail

# Env-overridable so callers (e.g. tests/validate.sh) can drive a
# differently-named container or image without modifying this script.
CONTAINER_NAME="${CONTAINER_NAME:-ka9q-radio}"
IMAGE_NAME="${IMAGE_NAME:-ka9q-radio}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# This script lives in docker/ka9q-radio/; project root is two levels up.
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Directory bind-mounted to /firmware (must contain SDDC_FX3.img). Defaults to
# the in-repo SDDC_FX3/ tree; override to point at firmware built/kept outside
# the repo, e.g.  FIRMWARE_DIR=/abs/path/to/firmware-dir ./ka9q.sh start
FIRMWARE_DIR="${FIRMWARE_DIR:-$PROJECT_ROOT/SDDC_FX3}"

# FFTW planning rigor. Default "estimate" for an instant cold boot on this
# test/eval image; set FFTW_RIGOR=measure|patient for long-term operation.
FFTW_RIGOR="${FFTW_RIGOR:-estimate}"

usage() {
    sed -n '3,/^$/s/^# \?//p' "$0"
}

container_running() {
    docker ps --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"
}

cmd_start() {
    # Default: use the image's baked CMD (radiod @ rx888-test.conf). With --vhf
    # (or --conf NAME), override the CMD and bind-mount the local config so it
    # takes effect without an image rebuild.
    local conf_name=""
    while [ $# -gt 0 ]; do
        case "$1" in
            --vhf|--fm) conf_name="rx888-vhf-fm" ;;
            --conf)     shift; conf_name="${1:-}" ;;
            *) echo "unknown start option: $1" >&2; return 2 ;;
        esac
        shift
    done

    local conf_mount=() cmd_override=()
    if [ -n "$conf_name" ]; then
        local local_conf="$SCRIPT_DIR/${conf_name}.conf"
        if [ ! -f "$local_conf" ]; then
            echo "config not found: $local_conf" >&2
            return 2
        fi
        local in_container="/etc/radio/radiod@${conf_name}.conf"
        conf_mount=(-v "$local_conf:$in_container:ro")
        cmd_override=(radiod "$in_container")
        echo "Using config: $conf_name (radiod $in_container)"
    fi

    if container_running; then
        echo "Container '$CONTAINER_NAME' is already running."
        return 0
    fi
    if [ ! -f "$FIRMWARE_DIR/SDDC_FX3.img" ]; then
        echo "WARNING: $FIRMWARE_DIR/SDDC_FX3.img not found."
        echo "         Set FIRMWARE_DIR=/abs/path/to/firmware-dir to point at"
        echo "         external firmware. Upload will fail unless the device is"
        echo "         already loaded (PID 0x00F1)."
    fi
    mkdir -p "$PROJECT_ROOT/wisdom"
    # /dev/snd + audio group give the in-container `monitor` access to host
    # ALSA so audio actually plays.  Harmless on hosts without sound — the
    # device simply isn't bound and `monitor` falls back to silent operation.
    local snd_args=()
    if [ -e /dev/snd ]; then
        snd_args+=(--device /dev/snd --group-add audio)
    fi
    # --network host is REQUIRED for radiod's in-container cold start. After it
    # uploads firmware the FX3 re-enumerates (00f3->00f1); that re-acquire is a
    # USB *hotplug* event, and hotplug is delivered over a netlink socket that is
    # network-namespace-scoped — a bridge container never hears it, so libusb's
    # device list stays stale and radiod fails with "device could not be found"
    # (docs/ka9q-compat-audit.md §1). Host netns is the only way libusb sees the
    # re-enumeration. Multicast stays deterministic by keeping everything on
    # loopback: radiod defaults to lo and the harness consumers pin `,lo`/`-I lo`.
    # The multi-homed hazard only bites UN-pinned consumers; ours pin. Under host
    # net ka9q-web binds host :8081 directly (no -p publish).
    docker run --rm -d --name "$CONTAINER_NAME" --privileged \
        --network host \
        -v /dev/bus/usb:/dev/bus/usb \
        -v /run/udev:/run/udev:ro \
        -v "$FIRMWARE_DIR:/firmware" \
        -v "$PROJECT_ROOT/wisdom:/var/lib/ka9q-radio" \
        "${snd_args[@]}" \
        "${conf_mount[@]}" \
        -e FFTW_RIGOR="$FFTW_RIGOR" \
        "$IMAGE_NAME" "${cmd_override[@]}" >/dev/null
    echo "Container '$CONTAINER_NAME' started."
    echo "Follow logs:  docker logs -f $CONTAINER_NAME"
}

cmd_console() {
    if ! container_running; then
        echo "Container '$CONTAINER_NAME' is not running.  Start it with: $0 start" >&2
        exit 1
    fi
    exec docker exec -it "$CONTAINER_NAME" bash
}

cmd_monitor() {
    local stream="${1:-wwv-pcm.local}"
    if ! container_running; then
        echo "Container '$CONTAINER_NAME' is not running.  Start it with: $0 start" >&2
        exit 1
    fi
    echo "Running ka9q's monitor inside the container.  Ctrl-C to stop."
    echo "If you hear nothing, check that the container was started with"
    echo "/dev/snd available on the host (re-run '$0 start' on a host with sound)."
    exec docker exec -it "$CONTAINER_NAME" monitor "$stream"
}

cmd_listen() {
    local stream="${1:-wwv-pcm.local}"
    if ! command -v avahi-resolve >/dev/null 2>&1; then
        echo "avahi-resolve not found.  Install avahi-utils on the host" >&2
        echo "  (e.g. sudo apt install avahi-utils)" >&2
        exit 1
    fi
    if ! command -v cvlc >/dev/null 2>&1; then
        echo "cvlc not found.  Install VLC on the host" >&2
        echo "  (e.g. sudo apt install vlc)" >&2
        exit 1
    fi
    local addr
    addr=$(avahi-resolve -n "$stream" 2>/dev/null | awk '{print $2}')
    if [ -z "$addr" ]; then
        echo "Could not resolve '$stream' via avahi-daemon." >&2
        echo "  - Is the container running?  ($0 start)" >&2
        echo "  - Is avahi-daemon running on the host?" >&2
        echo "  - Is the multicast stream actually being published?" >&2
        echo "    (check 'docker logs $CONTAINER_NAME' for 'Established under name')" >&2
        exit 1
    fi
    echo "Resolved $stream -> $addr"
    echo "Playing with cvlc (--network-caching=200).  Ctrl-C to stop."
    echo "If audio doesn't come through, try the GUI: vlc rtp://@$addr:5004"
    exec cvlc --network-caching=200 "rtp://@$addr:5004"
}

cmd_stop() {
    if ! container_running; then
        echo "Container '$CONTAINER_NAME' is not running."
        return 0
    fi
    docker stop "$CONTAINER_NAME" >/dev/null
    echo "Container '$CONTAINER_NAME' stopped."
}

case "${1:-help}" in
    start)          shift; cmd_start "$@" ;;
    console)        shift; cmd_console "$@" ;;
    monitor)        shift; cmd_monitor "$@" ;;
    listen)         shift; cmd_listen "$@" ;;
    stop)           shift; cmd_stop "$@" ;;
    help|-h|--help) usage ;;
    *) usage; exit 2 ;;
esac
