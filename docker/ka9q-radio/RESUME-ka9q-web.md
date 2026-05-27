# Resuming the ka9q-web spectrum setup

Quick guide to stand the ka9q-web + RX888 spectrum stack back up.
Branch: `claude/shdn-when-stopped`.

## Stand it up

```bash
cd <repo root>            # e.g. ~/code/sdr/ExtIO_sddc
git pull
docker build -t ka9q-radio docker/ka9q-radio/    # ka9q-radio 42273761 + ka9q-web + hack_no_usb_reset
docker rm -f ka9q-radio; ./ka9q.sh start
docker logs ka9q-radio | tail -20                # expect "rx888 running", "3 channels started", NO "reset failed"
docker exec -d ka9q-radio ka9q-web -m hf.local -p 8081 -n rx888-test
# browse http://localhost:8081  -> waterfall (LOW until an antenna is connected)
```

## Key facts (so they don't get re-derived)

- **`hack_no_usb_reset = yes`** in `rx888-test.conf` is REQUIRED. ka9q-radio
  42273761's rx888 driver does a `libusb_reset_device()` during init; this
  firmware (correctly, by design) reboots to the bootloader (DFU) on a USB
  reset, so the driver's reset drops the device to DFU and fails
  `rx888_usb_init` (`reset failed, -5`). The knob — added by WA2N (Scott
  Newell), the ka9q-web author — skips the reset. Verified on the wire
  (`tests/usb_trace.sh`) that the firmware itself does the right thing
  (reset -> DFU -> clean re-enumerate); the failure was purely the driver's
  optional reset, NOT a firmware defect.
- **ka9q-radio `42273761` + ka9q-web `b63c991` is the proven pair** (the
  W1EUJ / palomar-sdr.com deployment) and they MUST move together. Newer
  ka9q-radio (e.g. d555f185) changed `Metadata_dest_socket` from
  `struct sockaddr` to `sockaddr_storage`, which this ka9q-web commit does
  not handle -> compile error.
- **Spectrum "really low" = no antenna** (noise floor ~-110 dB), not a bug.
  Connect an antenna or raise `gain` in `rx888-test.conf` to see signals.
- **`powers` (CLI) uses `SPECT_DEMOD`** -> the half-flat/untuned output we
  saw; **ka9q-web uses `SPECT2_DEMOD`** -> the real spectrum. Use ka9q-web
  for the waterfall, not `powers`.
- Device at `00f3` (DFU) is normal — radiod uploads firmware on start.

## Diagnostics

```bash
# Host-side USB lifecycle (no device claim — safe alongside radiod):
sudo tests/usb_trace.sh
# Manual radiod in the foreground to watch its log:
docker run --rm -it --name ka9q-dbg --privileged \
  -v /dev/bus/usb:/dev/bus/usb -v /run/udev:/run/udev:ro \
  -v "$(pwd)/SDDC_FX3:/firmware" -v "$(pwd)/wisdom:/var/lib/ka9q-radio" \
  -p 127.0.0.1:8081:8081 ka9q-radio bash
#   inside: radiod /etc/radio/radiod@rx888-test.conf
# Manual ka9q-web in the foreground to watch its log:
docker exec -it ka9q-radio ka9q-web -m hf.local -p 8081 -n rx888-test
```

## Open thread

ka9q-web renders the spectrum but it's low with no antenna — confirm
signals with an antenna connected. The `powers`/`SPECT_DEMOD` half-flat
output is a separate, lower-priority curiosity (the SPECT vs SPECT2 path).

NOTE: the docker/ka9q-web integration here is exploratory validation built
on top of the PR's core firmware change (issue #131, ADC SHDN standby).
Decide what belongs in the merged PR vs. a separate change before merging.
