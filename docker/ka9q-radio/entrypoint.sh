#!/bin/bash
set -e

# Start dbus (required by avahi)
if [ ! -d /run/dbus ]; then
    mkdir -p /run/dbus
fi
dbus-daemon --system --nofork &
sleep 0.5

# Start avahi (required by ka9q-radio for multicast service discovery)
avahi-daemon --no-drop-root --daemonize 2>/dev/null || true

# Check for firmware image
if [ ! -f /firmware/SDDC_FX3.img ]; then
    echo "WARNING: /firmware/SDDC_FX3.img not found."
    echo "Point at your built firmware by bind-mounting the directory that"
    echo "contains SDDC_FX3.img onto /firmware, e.g.:"
    echo "    -v /abs/path/to/firmware-dir:/firmware"
    echo "or mount the file directly if it has a different name:"
    echo "    -v /abs/path/to/your.img:/firmware/SDDC_FX3.img"
    echo "(ka9q.sh: FIRMWARE_DIR=/abs/path/to/firmware-dir ./ka9q.sh start)"
    echo "If the device is already loaded (PID 0x00F1), this is only a warning."
fi

# Check for USB device
if lsusb -d 04b4:00f1 > /dev/null 2>&1; then
    echo "RX888 found (loaded, PID 0x00F1)"
elif lsusb -d 04b4:00f3 > /dev/null 2>&1; then
    echo "RX888 found (bootloader, PID 0x00F3) — radiod will upload firmware"
else
    echo "WARNING: No RX888 detected on USB bus"
    echo "Make sure to run with: --privileged -v /dev/bus/usb:/dev/bus/usb -v /run/udev:/run/udev:ro"
fi

# ADC sample rate — substituted into the radiod config at startup.
# Default 64m8 (64.8 Msps). Set ADC_SAMPRATE=129m6 for full-rate
# (129.6 Msps) if the RX888's thermal headroom allows it.
ADC_SAMPRATE="${ADC_SAMPRATE:-64m8}"
CONF="/etc/radio/radiod@rx888-test.conf"
case "$ADC_SAMPRATE" in
    64m8|64800000)  ADC_SAMPRATE="64m8" ;;
    129m6|129600000) ADC_SAMPRATE="129m6" ;;
    *)
        echo "WARNING: unknown ADC_SAMPRATE='$ADC_SAMPRATE'; using 64m8."
        ADC_SAMPRATE="64m8"
        ;;
esac
sed -i "s/^samprate = 64m8$/samprate = $ADC_SAMPRATE/" "$CONF"
echo "ADC sample rate: $ADC_SAMPRATE"

# FFTW wisdom — skip generation, just use ESTIMATE.  Generating wisdom
# inside a container is impractical (the large FFT sizes take forever
# and the result is CPU-specific anyway).  ESTIMATE is fine for a
# firmware test/eval image.

exec "$@"
