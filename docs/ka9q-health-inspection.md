---
title: ka9q-radio health & subsystem inspection
---

# ka9q-radio health & subsystem inspection

A runbook for looking *directly* at ka9q-radio and every subsystem the test
harness rides on, when the harness alone isn't telling you enough.

The automated harnesses (`tests/ka9q_smoke.sh`, `tests/ka9q_test.sh`) judge
health *indirectly*: "radiod process alive + no fatal log markers + the
spectrum looks like a live thermal floor." That is the right gate for a
go/no-go, but when something is wrong it tells you *that* it's wrong, not
*which* of the ~8 moving parts broke. This doc gives the direct, per-subsystem
checks so you can localize a failure fast.

> **Scope.** This is the host/container side — radiod and friends. For the
> *firmware's* own telemetry (DMA counters, GPIF state, PLL-lock heuristics via
> `GETSTATS`) see [diagnostics side-channel](diagnostics_side_channel.md); for
> wedge recovery see [wedge detection](wedge_detection.md) and
> [GPIF & recovery](gpif-and-recovery.md). For the container/multicast/avahi
> gotchas these checks are built on, see [docker.md](docker.md).

## How to run these checks (read first)

Work **inside** the container, from a persistent interactive shell:

```sh
./docker/ka9q-radio/ka9q.sh console      # docker exec -it … bash
```

Then run the plain commands below (`radiod`, `pgrep`, `control`, `powers`,
`fx3_cmd`, …) in that shell. **Do not** wrap each command in a fresh host-side
`docker exec …`: each one spawns a separate process and that has caused real
bugs here — an unforwarded Ctrl-C, a stray `radiod` claiming the USB device.
(See [docker.md §1](docker.md).)

Two hard rules that bite every newcomer:

- **Don't probe the device with `fx3_cmd` while `radiod` is running.** They
  both claim the USB interface; probing steals the claim and crashes radiod.
  Host-side device probes are valid only when radiod is *stopped* (this is the
  same constraint `ka9q_test.sh` is built around).
- **Don't bounce avahi-daemon.** Restarting it mid-session breaks `.local`
  resolution in ways that look like radiod faults. (See [docker.md §3](docker.md).)

---

## 0. At-a-glance health board

Paste this block into the container shell for a 10-second overview. Each line
is "subsystem → is it alive?"; a red light points you at the numbered section.

```sh
echo "== radiod ==" ;        pgrep -a radiod || echo "  DOWN (§3)"
echo "== USB / FX3 ==" ;     lsusb -d 04b4: || echo "  NO DEVICE (§2)"
echo "== avahi/dbus ==" ;    pgrep -a avahi-daemon >/dev/null && pgrep -a dbus-daemon >/dev/null \
                               && echo "  up" || echo "  DOWN (§6)"
echo "== mDNS hf.local ==" ; avahi-resolve -n hf.local || echo "  UNRESOLVED (§6)"
echo "== ka9q-web ==" ;      ss -ltnp 2>/dev/null | grep -q ':8081' && echo "  listening :8081" \
                               || echo "  not listening (§8)"
echo "== mcast joins ==" ;   ip maddr show | grep -A2 -E 'lo$' | grep -c '239\.' | sed 's/^/  groups: /'
```

| Light | Means | Go to |
|-------|-------|-------|
| radiod DOWN | core not running / crashed | [§3](#3-radiod-core) |
| NO DEVICE | FX3 not on the bus, or at the wrong PID | [§2](#2-usb--fx3-firmware) |
| avahi/dbus DOWN | discovery substrate gone | [§6](#6-dbus--avahi-mdns) |
| hf.local UNRESOLVED | radiod not advertising, or resolver path wrong | [§6](#6-dbus--avahi-mdns) |
| :8081 not listening | web viewer down (non-fatal for smoke test) | [§8](#8-ka9q-web--libonion) |
| 0 mcast groups | no data-plane joins | [§7](#7-multicast--rtp-data-plane) |

---

## 1. The chain, in dependency order

```
dbus → avahi ──┐
               ▼
USB/FX3 ── radiod ── rx888.so ── Si5351 (host-programmed) ── GPIF stream
               │                                                  │
               └── multicast/RTP ── control / powers / monitor / ka9q-web
FFTW wisdom ───┘ (affects start latency only)
```

Read failures from the left: a dead avahi or a missing device explains a
"radiod won't start," but a dead ka9q-web never explains a missing spectrum.

---

## 2. USB / FX3 firmware

**What it is.** The RX888mk2's Cypress FX3, running SDDC firmware. Everything
else is downstream of the device being on the bus at the right PID and speed.

**Fast check:**

```sh
lsusb -d 04b4:
#   04b4:00f3  → bootloader / DFU (no firmware) — radiod will upload
#   04b4:00f1  → SDDC firmware running
```

**Deeper:**

```sh
# SuperSpeed (5 Gb/s) negotiated? HF at 64.8 Msps needs USB3; a USB2 fallback
# (480 Mb/s) silently starves the stream.
lsusb -t                                  # tree + per-device speed
for d in /sys/bus/usb/devices/*/idVendorId; do :; done   # (idVendor/idProduct/speed live here)
cat /sys/bus/usb/devices/*/speed 2>/dev/null  # "5000" = SS, "480" = HS

# Re-enumeration / reset storms (the §10/DFU-flip signature):
dmesg | grep -iE 'usb|xhci' | tail -40
```

**Direct firmware counters — only when radiod is NOT running:**

```sh
# (radiod stopped; the device is host-claimable again)
fx3_cmd test              # probe: model/version, returns 4-byte TESTFX3 reply
fx3_cmd stats             # GETSTATS counters (DMA count, GPIF state, …)
```

See [tests/README.md](../tests/README.md) for the full `fx3_cmd` command set and
[diagnostics_side_channel.md](diagnostics_side_channel.md) for what each
`GETSTATS` field means.

**Failure signatures:**

| Symptom | Likely cause |
|---|---|
| stuck at `00f3`, never flips to `00f1` | firmware upload failing — check `/firmware/SDDC_FX3.img` mount + `/run/udev` bind-mount |
| flips `00f1`→`00f3` during init | a USB reset is hitting the device → DFU. With the driver-eval pin this should not happen (`reset = no`); if it does, see compat-audit §1/§10 |
| device present but speed = 480 | USB2 path — reseat into a USB3 port/cable; stream will under-run |
| `lsusb` empty for `04b4:` | host not seeing the device at all — fix before touching the container |

---

## 3. radiod core

**What it is.** The multichannel SDR engine. It loads the `rx888.so` plugin,
programs the front end, runs the FFT/filter graph, and publishes status + RTP
on multicast.

**Fast check:** `pgrep -a radiod` (alive?).

**Deeper — watch the bring-up live.** Stop the entrypoint's radiod and run it
in the foreground with verbose logging so you can read the sequence:

```sh
pkill -INT radiod ; sleep 2          # clean stop (SIGINT, not -9)
radiod -v /etc/radio/radiod@rx888-test.conf
```

Bring-up markers to expect, in order:

```
... Dynamically loading rx888 hardware driver from .../rx888.so   ← plugin found+loaded
... RX888 Si5351 PLL: vco = …                                     ← NEW host-side synthesis (§4/§5)
... RX888 Si5351 output divider: samprate = …                    ← NEW host-side synthesis
... rx888 running                                                 ← streaming threads up; data plane live
```

If it dies or hangs, attach a debugger (still inside the container):

```sh
gdb -p "$(pgrep radiod)" -batch -ex 'thread apply all bt'   # all-thread backtrace
```

**Failure signatures (the two we care about most for the driver-eval):**

| Backtrace top | Meaning | Reference |
|---|---|---|
| `command_send` ← `rx888_setup` (SIGSEGV in libusb) | invalid device handle used before/at setup | compat-audit §10 — **revert the pin if this recurs** |
| stalled in setup, no streaming threads, last log = `TUNERSTDBY`/`STARTFX3` | restart bring-up wedge | compat-audit §11; patch `04` removes the HF-path `TUNERSTDBY` |
| exits with "device could not be found" | libusb hotplug stale — missing `/run/udev` mount | docker.md / README |

Use `-vv` for even louder logging if `-v` isn't enough.

---

## 4. rx888.so plugin + GPIF streaming

**What it is.** The hardware driver radiod dlopen's: it programs the front end
and pumps the bulk-IN GPIF stream into radiod's buffers. **At the driver-eval
pin it also synthesizes the Si5351 clock host-side** (see §5).

**Plugin loaded?** The `Dynamically loading rx888 …` line in §3. No line → the
`.so` isn't where radiod looks (`/usr/local/lib/ka9q-radio/rx888.so`) or failed
to dlopen (run `ldd` on it).

**Is the data plane actually moving** (not just "process alive")?

- *Without touching the device, while radiod runs:* the spectrum is the proof —
  a textured ~-130 dB floor with the f=0 DC spike and the fs/2 (32.4 MHz)
  Nyquist alias means real samples are flowing. `tests/ka9q_smoke.sh` automates
  exactly this gate; for a manual single tile:

  ```sh
  powers -r hf.local -f 10000000 -w 1000 -c 1     # quick spectrum around 10 MHz
  ```

- *With radiod stopped:* `fx3_cmd stats` twice, a second apart — the DMA count
  must advance while streaming and freeze after `STOPFX3`. (Device-claim rule,
  §2.)

**Failure signatures:** flat featureless floor (no variance, no DC spike, no
fs/2 alias) → ADC frozen / not clocked → suspect Si5351 (§5) or a stopped GPIF.
"readiness ≠ data-plane": radiod can report ready and still emit nothing — see
[docker.md §8](docker.md).

---

## 5. Si5351 clock (now host-programmed)

**What it is — and why it's new.** On the driver-eval pin, `rx888.c` computes
the Si5351 PLL/MultiSynth solution **on the host** (`si5351_solve` in the new
`si5351.c`) and writes the registers over `I2CWFX3`. Previously more of this
lived in the firmware. A wrong solution = wrong sample clock = a dead or
aliased spectrum, so this is the prime suspect for the driver-eval.

**Direct checks:**

```sh
# 1) Trust but verify the host's math: radiod -v prints the solution it sent.
#    For 64.8 Msps from a 27 MHz reference, the vco/divider numbers must be
#    self-consistent (vco = ref * (a + b/c); samprate = vco / divider).
radiod -v /etc/radio/radiod@rx888-test.conf 2>&1 | grep -i 'Si5351'

# 2) Read the chip back over I2C (radiod stopped; device claimable):
fx3_cmd i2cr 0xC0 0  1     # status reg 0 — bit7 = PLL/LOL loss-of-lock-ish
fx3_cmd i2cr 0xC0 16 1     # CLK0_CONTROL — bit7 = CLK0 powered/enabled
#   (CLK0_CONTROL readback is what the firmware's GpifPreflightCheck reads;
#    see compat-audit and the "Querying Si5351 CLK0 state" entry in index.md.)
```

**Failure signatures:** no fs/2 alias spike + flat floor → ADC not getting a
valid clock → re-check the logged solution and the CLK0/status readback.
Loss-of-lock in the status register, or CLK0 not enabled, points at the
host-side programming sequence (the new code path under evaluation). Cross-ref
[diagnostics_side_channel.md §4 (PLL lock)](diagnostics_side_channel.md).

---

## 6. dbus + avahi (mDNS)

**What it is.** radiod advertises and clients resolve `*.local` names (e.g.
`hf.local`, `wwv-pcm.local`) via mDNS. avahi-daemon needs dbus. The entrypoint
starts both; if either dies, every `.local` lookup fails and it *looks* like
radiod is broken.

**Direct checks:**

```sh
pgrep -a dbus-daemon ; pgrep -a avahi-daemon     # both must be up
avahi-resolve -n hf.local                        # name → multicast address
avahi-browse -atr                                # list ALL advertised services
#   (ka9q advertises control + stream services, e.g. _ka9q-ctl._udp / _rtp._udp;
#    use -a to see whatever this build actually registers, don't assume a type.)
```

**Two traps (both in [docker.md](docker.md)):**

- **§4 — two resolver paths disagree.** glibc NSS (`libnss-mdns`) and avahi's
  own tools can give different answers. `getent hosts hf.local` (NSS path) vs
  `avahi-resolve -n hf.local` (avahi path); a mismatch is the bug, not the
  radio.
- **§3 — do not bounce avahi.** Restarting it mid-session is itself a failure
  mode here.

**Failure signature:** `Temporary failure in name resolution` from
`control`/`tune`/`monitor` → mDNS/NSS path, not radiod. If the image predates
`libnss-mdns`, rebuild `--no-cache` (README notes this).

---

## 7. Multicast / RTP data plane

**What it is.** radiod publishes its status stream and RTP audio on IPv4
multicast (239.x). `control`/`tune` speak the status protocol; `monitor` plays
RTP; `powers` requests spectra. The container runs `--network host` (required
for USB hotplug / cold start — §2 and compat-audit §1), and multicast is kept
deterministic by pinning everything to loopback (`lo`).

**Direct checks:**

```sh
control hf.local                 # curses status UI — channels, tune, levels
ip maddr show                    # confirm 239.x group joins (on lo)
tcpdump -ni lo 'udp and multicast' -c 20   # are status/RTP datagrams flowing?
monitor wwv-pcm.local            # audio (needs /dev/snd passthrough for sound)
```

**The interface-pinning trap ([docker.md §2](docker.md)).** Under host
networking an un-pinned consumer joins the group on the kernel's default-route
interface while radiod sends on `lo` — they never meet, and you get silence
with everything "up." `powers`/`hf_sweep.sh`/`ka9q_smoke.sh` default to `-I lo`
for exactly this; if a manual `control`/`powers` sees nothing, suspect
interface selection first — and if radiod logged an interface other than `lo`,
pin the consumer to match it.

**Failure signature:** `control` shows no channels / `powers` returns nothing,
but radiod is alive and advertising → data-plane join mismatch (pin the iface),
or "readiness ≠ data-plane" timing on a fresh/just-retuned channel
([docker.md §8](docker.md); `hf_sweep.sh` discards the first integration for
this reason).

---

## 8. ka9q-web + libonion

**What it is.** The web spectrum/waterfall viewer (links libonion). Convenient,
but **not required** for the firmware smoke test (`powers` is). A dead ka9q-web
never explains a missing spectrum.

**Direct checks:**

```sh
pgrep -a ka9q-web
ss -ltnp | grep ':8081'                 # listening?
curl -sI http://localhost:8081/         # HTTP reachable? (container publishes 8081)
ldd "$(command -v ka9q-web)" | grep -i onion   # libonion linked/found?
```

Start it by hand if needed:
`ka9q-web -m hf.local -p 8081 -n rx888-test` then browse `http://localhost:8081`.

**Failure signature:** won't start / segfaults on launch after a ka9q-radio SHA
bump → the ka9q-web↔ka9q-radio object/header pairing (it links radiod's
`multicast.o`/`status.o`/`misc.o`/`decode_status.o`/`rtp.o`). This is the
known moving part of the driver-eval bump — see compat-audit §12.

---

## 9. FFTW wisdom (start latency, not a failure)

**What it is.** radiod plans large FFTs at startup; cached "wisdom" makes that
fast. Wisdom is CPU-specific and can't be baked into the image, so the
entrypoint generates it on first run and persists it (bind-mount
`/var/lib/ka9q-radio`).

**Direct checks:**

```sh
ls -l /var/lib/ka9q-radio/wisdom        # present + non-empty?
echo "$FFTW_RIGOR"                       # default estimate (instant); measure|patient|exhaustive
```

**Signature:** with the default `FFTW_RIGOR=estimate` cold boot is instant; a
*slow first start* (minutes, or hours at `patient`) means someone set a higher
rigor — that's wisdom generation, not a hang. Missing/!-matched wisdom → radiod
falls back to `FFTW_ESTIMATE` (works, slower runtime) — never a hard failure.
Sizes for the default config: `rof1620000 cob240` (see `entrypoint.sh`).

---

## 10. Decision tree — harness FAIL → first check

```
ka9q_smoke.sh / ka9q_test.sh FAILs
│
├─ radiod not running?           → §3 (foreground -v, gdb backtrace)
│   ├─ SIGSEGV in command_send   → compat-audit §10  ⇒ REVERT the pin
│   └─ stall after TUNERSTDBY     → compat-audit §11 / patch 04
│
├─ radiod up, no spectrum?
│   ├─ flat/dead floor            → §5 Si5351 (logged solution + CLK0 readback)
│   │                               then §4 GPIF / §2 USB speed
│   └─ control/powers see nothing → §7 iface pinning, then §6 mDNS
│
├─ "Temporary failure in name resolution"  → §6 dbus/avahi/NSS
├─ device missing / DFU-flipping → §2 (PID, dmesg resets, reset=no)
└─ web viewer only               → §8 (non-fatal for the smoke gate)
```

**Driver-eval watch-items** (this branch's whole point — see compat-audit §12):
1. §3 — no §10 `command_send` segfault; reaches `rx888 running`.
2. §3/§7 — re-run the `ka9q_test.sh` restart soak; no §11 stall.
3. §5 — host-side Si5351 solution logs sane and the fs/2 alias is present.
