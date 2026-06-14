# ka9q-radio Docker Test Environment

Docker container for testing SDDC_FX3 firmware compatibility with
[ka9q-radio](https://github.com/ka9q/ka9q-radio)'s `radiod` and the
`rx888.so` driver plugin.

## Build

```
docker build -t ka9q-radio docker/ka9q-radio/
```

## Find your USB device

`/dev/bus/usb` is the Linux usbfs tree. To confirm the RX888mk2 is
visible to the host before mounting it into the container:

```
# List all USB devices; look for Cypress FX3
lsusb

# Filter to Cypress IDs:
#   04b4:00f3  -> FX3 boot loader (no firmware loaded)
#   04b4:00f1  -> SDDC firmware running
lsusb -d 04b4:
```

Example output:

```
Bus 002 Device 014: ID 04b4:00f3 Cypress Semiconductor Corp. CYUSB3013/CYUSB3014
```

The corresponding device node is `/dev/bus/usb/<bus>/<device>` —
`/dev/bus/usb/002/014` in the example. Mounting all of `/dev/bus/usb`
into the container (as the `docker run` examples below do) lets the
container see the device even when its bus/device path changes after
firmware upload (PID flips `0x00f3` → `0x00f1`, which usually
re-enumerates).

If `lsusb` shows nothing for `04b4:`, the host isn't seeing the
device — fix that first (cable, USB 3.0 port, power) before bothering
with Docker. macOS and Windows do not expose `/dev/bus/usb`; this
container is Linux-only.

## Run

### With firmware already loaded on the device

```
docker run --rm -it --privileged --network host \
  -v /dev/bus/usb:/dev/bus/usb \
  -v /run/udev:/run/udev:ro \
  ka9q-radio
```

### With firmware upload (radiod handles it)

Bind-mount the directory that contains your built `SDDC_FX3.img` onto
`/firmware`. That directory is **external to the image** — point it wherever
your firmware actually lives; the in-repo `SDDC_FX3/` source tree only has an
`.img` after you build it.

```
docker run --rm -it --privileged --network host \
  -v /dev/bus/usb:/dev/bus/usb \
  -v /run/udev:/run/udev:ro \
  -v /abs/path/to/firmware-dir:/firmware \
  -v $(pwd)/wisdom:/var/lib/ka9q-radio \
  ka9q-radio
```

If your firmware file has a different name, mount the file directly instead:
`-v /abs/path/to/your.img:/firmware/SDDC_FX3.img`. With the `ka9q.sh` helper,
set `FIRMWARE_DIR=/abs/path/to/firmware-dir ./ka9q.sh start` (defaults to the
in-repo `SDDC_FX3/`).

> **`--network host` is required** (not bridge). radiod's cold-start path —
> upload firmware, FX3 re-enumerates `00f3`→`00f1`, re-acquire — depends on a
> USB **hotplug** event, and hotplug is delivered over a **network-namespace-
> scoped** netlink socket that a bridge container never receives, so libusb
> fails with "device could not be found" (see `docs/ka9q-compat-audit.md` §1).
> Host netns is the only way libusb sees the re-enumeration. Multicast stays
> deterministic because everything is kept on **loopback**: radiod defaults to
> `lo` and the harness consumers pin `-I lo` / `,lo` — the multi-homed hazard
> only affects *un-pinned* consumers. Under host networking ka9q-web binds host
> `:8081` directly, so no `-p` publish is needed.

The `/run/udev` bind mount is **required** when radiod uploads the
firmware: after the FX3 re-enumerates from `04b4:00f3` (DFU) to
`04b4:00f1` (loaded), libusb inside the container needs to see the
new device.  libusb's hotplug listener subscribes to systemd-udevd
events; udevd does not run inside the container, so without this
mount libusb returns a stale device list forever and radiod exits
with "Error or device could not be found".  The mount is harmless
(read-only) for the firmware-already-loaded case, so it is shown in
both examples.

The `wisdom` bind mount persists FFTW wisdom across runs.  On first
run the entrypoint generates wisdom for the host CPU (FFTW wisdom is
CPU-specific — it cannot be baked into a portable image).  Subsequent
runs reuse the saved file.

Planning rigor is controlled by the `FFTW_RIGOR` environment variable:

| Value        | First-run time           | Runtime FFT performance |
|--------------|--------------------------|-------------------------|
| `estimate`   | instant (default)        | slowest                 |
| `measure`    | minutes                  | near-optimal            |
| `patient`    | hours (1.62M-point FFT)  | optimal                 |
| `exhaustive` | many hours to days       | marginally > patient    |

The default is **`estimate`** so a cold boot of this test/eval image is
instant — appropriate for firmware-compatibility checks, where optimal
runtime FFT plans don't matter. Override with `-e FFTW_RIGOR=<value>` on
`docker run` (or `FFTW_RIGOR=measure ./ka9q.sh start`), e.g.
`-e FFTW_RIGOR=patient` if you intend to operate the radio long-term and
want the most efficient FFT plans.

### Tuning and listening (helper script)

`ka9q.sh` (in this directory) wraps the typical operating workflow.
The container publishes demodulated audio as RTP/multicast (per
`radiod`'s standard behavior); the helper gives you start / stop /
console / monitor / listen subcommands without having to remember
the long `docker run` invocation.

Host-side requirements:

```
sudo apt install avahi-utils alsa-utils    # for mDNS resolve + ALSA
```

The `start` subcommand auto-detects `/dev/snd` on the host and
adds `--device /dev/snd --group-add audio` to the `docker run`
invocation when present, so the in-container `monitor` can play
audio through the host's sound card.

Typical session (three terminals):

```
# Terminal A — launch the container in the background:
./ka9q.sh start

# Terminal B — listen with ka9q's own monitor (recommended):
./ka9q.sh monitor                     # default stream: wwv-pcm.local
# or:
./ka9q.sh monitor <other-stream.local>

# Terminal C — drop into the container to operate the radio:
./ka9q.sh console
# inside the container:
control hf.local                      # curses tuner UI (recommended)

# Or one-shot from the container shell (requires the channel's SSRC,
# defined in radiod config — easier to just use `control` above
# which shows you all channels):
tune -r hf.local -s <ssrc> -f 14.074m
```

`control`, `tune`, and `monitor` resolve `*.local` names via mDNS;
the runtime image installs `libnss-mdns` so glibc's `getaddrinfo`
asks the in-container avahi-daemon for `.local` lookups.  If
`Temporary failure in name resolution` appears, the image is from
before this was added — rebuild with `docker build --no-cache`.

To shut down: `./ka9q.sh stop`.

### VHF FM-broadcast mode

The `--vhf` flag starts radiod with the VHF/FM config
(`rx888-vhf-fm.conf`) instead of the default HF test config:

```
./ka9q.sh start --vhf
```

This configures a WBFM receiver at the R828D IF centre (4.570 MHz).
Tune the R828D front-end separately with the host-side tuner script,
then listen:

```
python3 vhf/vhf_tune.py 100300000 --persist  # tune R828D to 100.3 MHz FM
./ka9q.sh monitor fm-pcm.local        # listen to demodulated audio
```

See `rx888-vhf-fm.conf` for signal-path details and GPIO caveats.

#### `monitor` keybindings (cheat sheet)

`monitor` is a curses program; press `h` inside it for the full
in-app help screen.  Most-used keys:

| Key | Action |
|---|---|
| `↑` `↓` | previous / next session |
| `PgUp` `PgDn` | page through sessions |
| `m` / `u` | mute / unmute current session (`u` also resets) |
| `M` / `U` | mute / unmute all sessions |
| `+` `-` | per-session gain ±1 dB |
| `←` `→` | per-session pan (stereo) |
| `Shift+←` / `Shift+→` | playout buffer ±1 ms |
| `n` / `f` | notch on / off (current session) |
| `s` / `t` | sort sessions by recent / total activity |
| `r` | reset playout queue (current) |
| `R` | reset all sessions |
| `d` | delete current session |
| `v` | toggle verbose display |
| `h` | help screen |
| `q` or `Q` | quit |

#### Why `monitor`, not VLC

ka9q-radio publishes RTP audio with **dynamic payload types** (PT
96+) and no SDP description that VLC will pick up.  VLC opens the
stream and receives bytes, but cannot decode them — you get
silence.  `monitor` is part of ka9q-radio itself and understands
the stream format natively.

The script does provide a `listen` subcommand that runs `cvlc` on
the host as a fallback, but it usually does not produce audio.
It is kept for users who want to use VLC's GUI for diagnostics or
who have already arranged an SDP file for VLC.  Install VLC
separately if you want it: `sudo apt install vlc`.

### Interactive shell (for debugging)

For day-to-day operation, use the `ka9q.sh` helper script
documented above.  This section is for debugging the image itself
(checking USB visibility, running `radiod` by hand, inspecting
binaries, etc.) — it deliberately omits audio passthrough.

```
docker run --rm -it --privileged --network host \
  -v /dev/bus/usb:/dev/bus/usb \
  -v /run/udev:/run/udev:ro \
  -v /abs/path/to/firmware-dir:/firmware \
  -v $(pwd)/wisdom:/var/lib/ka9q-radio \
  ka9q-radio bash
```

> The `wisdom` mount + the default `FFTW_RIGOR=estimate` keep this debug
> shell's cold boot instant; without the wisdom mount you still pay only the
> instant `estimate` plan. Point `/firmware` at wherever your built
> `SDDC_FX3.img` lives (see [With firmware upload](#with-firmware-upload-radiod-handles-it)).

Then inside the container:

```
# Check USB device
lsusb -d 04b4:

# Run radiod manually (instead of the entrypoint's CMD):
radiod /etc/radio/radiod@rx888-test.conf

# From another terminal — docker exec into the same container — you can
# operate the receiver while radiod runs in the foreground here:
control hf.local                            # curses tuner UI

# Or one-shot tune (need the channel's SSRC from the radiod config):
tune -r hf.local -s <ssrc> -f 14.074m

# `monitor` will run but produce no audio in this debug invocation
# because /dev/snd is not passed through.  Use the helper script's
# `monitor` subcommand for actual audio output.
monitor wwv-pcm.local
```

## Verify it's streaming (no receiver UI)

The proof that the firmware is really running the radio is the power
spectrum itself — you don't need to listen to anything.  With the container
up (`./ka9q.sh start`), run the whole-band smoke test from the repo root:

```
tests/ka9q_smoke.sh
```

It sweeps `0 .. fs/2` via `powers`, renders a PNG, and PASS/FAILs
on whether the floor is a live, textured thermal spectrum (~-130 dB with
natural variance, the ADC DC spike, and the fs/2 alias) versus the
featureless flat line a frozen / shut-down ADC would produce.  See
`tests/README.md` for the calibrated 50 Ω dummy-load reference.

## What this tests

1. **Firmware upload** — ka9q-radio uses its own `ezusb.c` loader
   (same protocol as `rx888_stream -f`)
2. **Si5351 clock programming** — ka9q programs the Si5351 directly
   via `I2CWFX3`.  The firmware reads the Si5351 CLK0 enable state back
   from the chip, so `STARTFX3`'s GPIF preflight check passes with no
   host-side workaround.  (An earlier container patch — 03,
   `STARTADC` before `STARTFX3` — is retired; see `patches/README.md`.)
3. **GPIF streaming** — `STARTFX3` + async bulk transfers at 64.8 MSPS
4. **GPIO control** — `GPIOFX3` for dither, randomizer, HF/VHF select
5. **Attenuator/VGA** — `SETARGFX3` with DAT31_ATT and AD8340_VGA
6. **Stop/restart** — `STOPFX3` + `STARTFX3` cycling

## Known compatibility notes

See `docs/ka9q-compat-audit.md` in the parent repository for the
full analysis.  Summary:

- **ka9q-radio pin** — the container builds ka9q-radio `87567fa` (main),
  whose `rx888.c` does host-side Si5351 clock synthesis (new `si5351.c`
  module), polls the Si5351 for lock, and logs the rx888 firmware version
  via `TESTFX3`.  Paired with ka9q-web `91cbfca`.  See
  `docs/ka9q-compat-audit.md` §12 for the full driver-eval analysis.
- **Zero active container patches — builds vanilla ka9q-radio.**  Every local
  ask has been upstreamed: the `powers` float/double fixes (`01`, `02`) and
  the no-tuner-stdby change (`04`, removed at `87567fa`).  Patch `03`
  (`STARTADC` before `STARTFX3`) remains retired (the firmware reports Si5351
  CLK0 state truthfully, so the GPIF preflight passes with no host workaround).
  See `patches/README.md`.
- **`/run/udev` bind-mount required** at `docker run` time — libusb
  inside the container needs host udev events to see the FX3
  re-enumerate after firmware upload.  Container-side workaround,
  no patch.
- `sleep(1)` after firmware upload (`rx888.c:700`) — fragile in
  principle, sufficient on observed hardware with the udev mount.
  Documented in audit §1, no patch.
- `TUNERSTDBY` (0xB8) calls STALL on this firmware (no tuner) and could
  intermittently wedge radiod's restart bring-up — removed on the HF path by
  active patch `04-no-tuner-stdby` (audit §2, `patches/README.md`).
- GPIO LED bit-mapping differences (cosmetic).
- Missing `libusb_clear_halt()` in ka9q (xHCI fix needed upstream).

## Requirements

- Docker with `--privileged` support
- RX888mk2 connected via USB 3.0
- Host kernel with xHCI/USB 3.0 support (any modern Linux)
