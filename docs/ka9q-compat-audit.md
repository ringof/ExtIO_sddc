---
title: ka9q-radio compat audit
nav_order: 10
permalink: /ka9q-compat-audit/
description: Static audit of ka9q-radio against the RX888mk2 FX3 firmware - vendor-command compatibility, STARTADC/STARTFX3 ordering (fixed in v0.1.0), VHF/HF coverage.
---

# ka9q-radio ↔ SDDC_FX3 Compatibility Audit

Status: **container-side patches retired in v0.1.0** — the only host-side workaround (patch 03) is no longer needed; see §8 for the firmware fix.

This document captures a static compatibility audit of ka9q-radio's
`rx888.so` plugin against the SDDC_FX3 firmware in this repository.
Every claim cites a specific source file; behavior is named by
function so the analysis stays valid across line shifts.

## Reproducer

- Firmware: `SDDC_FX3/SDDC_FX3.img` (current tree).
- ka9q-radio: pinned in `docker/ka9q-radio/Dockerfile` via
  `ARG KA9Q_RADIO_SHA`. As of writing:
  `f78cff9cc8c7fa33ea49aac9176d9263c535a332`.
- Container build/run:

  ```
  docker build -t ka9q-radio docker/ka9q-radio/
  docker run --rm -it --privileged \
    -v /dev/bus/usb:/dev/bus/usb \
    -v $(pwd)/SDDC_FX3:/firmware \
    --network host \
    ka9q-radio
  ```

- Failure observed (before container patches):

  ```
  loading rx888 firmware file /firmware/SDDC_FX3.img, done
  Error or device could not be found
  rx888_usb_init() failed
  device setup returned -1
  ```

## Vendor-command compatibility matrix

| ka9q sends | bRequest | SDDC supports | Notes |
|---|---|---|---|
| STARTFX3 | 0xAA | ✅ | |
| STOPFX3 | 0xAB | ✅ | |
| TESTFX3 | 0xAC | ✅ | Defined in `rx888.h` but never sent by ka9q — see §9. |
| GPIOFX3 | 0xAD | ✅ | LED bit positions differ — see §4. |
| I2CWFX3 | 0xAE | ✅ | |
| I2CRFX3 | 0xAF | ✅ | |
| RESETFX3 | 0xB1 | ✅ | |
| STARTADC | 0xB2 | ✅ | Both sides sleep ~1 s; see §7. |
| TUNERINIT | 0xB4 | ❌ STALL | Only inside `#if 0` in `rx888.c`. |
| TUNERTUNE | 0xB5 | ❌ STALL | Only inside `#if 0`. |
| SETARGFX3 wIndex 1 (R82XX_ATTENUATOR) | 0xB6 | ❌ STALL | VHF only — see §3. |
| SETARGFX3 wIndex 2 (R82XX_VGA)        | 0xB6 | ❌ STALL | VHF only. |
| SETARGFX3 wIndex 10 (DAT31_ATT)       | 0xB6 | ✅ | HF path. |
| SETARGFX3 wIndex 11 (AD8340_VGA)      | 0xB6 | ✅ | HF path. |
| TUNERSTDBY | 0xB8 | ❌ STALL | **Called on the HF path** — see §2. |
| READINFODEBUG | 0xBA | ✅ | |

Sources: ka9q `src/rx888.c` and `src/rx888.h` at the pinned SHA;
SDDC firmware `docs/architecture.md` §"Command table" and
`docs/LICENSE_ANALYSIS.md` §"What was removed".

## Findings

### 1. `sleep(1)` after firmware upload *(ka9q-side, documented; no patch)*

After firmware upload, `rx888_usb_init()` in ka9q's `src/rx888.c`
sleeps once for one second, then performs a **single** scan for
`0x04b4:0x00f1` and returns "Error or device could not be found" if
absent.  The author's own comment on the `sleep(1)` flags the value
as a guess.

Original misdiagnosis: this was *thought* to be the cause of the
container's "Error or device could not be found" failure.  It was
not.  The actual cause was that libusb 1.0.26's hotplug listener
subscribes to systemd-udevd's filtered netlink events, and udevd
does not run inside the container, so libusb's cached device list
never updates and `libusb_get_device_list()` keeps returning a stale
list — even after a 10 s polling loop.  `LIBUSB_DEBUG=4` confirmed
the polling loop ran all 50 iterations seeing the same 8 cached
devices, never observing the re-enumerated `04b4:00f1`, even though
the host kernel had it fully enumerated (per `usbmon` and host-side
`lsusb`).

Fix (as understood then): at `docker run` time, bind-mount
`/run/udev:/run/udev:ro` so libusb's udev backend can see host udev
events.  With that mount in place, the kernel finishes SuperSpeed
enumeration ~430 ms after the last firmware byte is acked (per
`usbmon`), and the upstream `sleep(1)` + single rescan succeeds
reliably.  Verified working on the project's reference hardware.

A polling-with-timeout pattern in place of the fixed `sleep(1)`
would be more robust on pathologically slow hosts, but it is not
required for SDDC streaming to work, so we do not patch it — the
cost of a not-strictly-necessary upstream ask outweighs the benefit
on hypothetical hardware.  (The audit's own polling experiment —
50 iterations over a stale list — is itself evidence that *more
polling is not the fix*; see the mechanism below.)

#### Update (2026-06, driver-eval bench): the `/run/udev` mount is necessary but **not sufficient** — hotplug delivery is network-namespace-scoped

Re-running cold start on a second host (`bunsen`, multi-homed, while
evaluating the `21d51fac` driver) reproduced "Error or device could
not be found" **with `/run/udev` correctly mounted and populated**
(verified: host `systemd-udevd` running, container `/run/udev`
mirroring the host's `data/`/`control`).  The same run under
`--network host` succeeds.  Hot start (device already at `00f1` when
radiod calls `libusb_init`) works in either case.  That isolates the
variable to the **network namespace**, and the mechanism is:

libusb's Linux backend needs **two distinct things, over two distinct
paths**:

1. **Device files** — `/dev/bus/usb`, `/sys`, and the udev database in
   `/run/udev/data` — to *enumerate* and read attributes.  These are
   filesystem objects; `--privileged` + the `/run/udev` bind-mount
   satisfy this inside any container.
2. **Hotplug events** — "a device just appeared / re-enumerated" —
   delivered over a **netlink socket** (`NETLINK_KOBJECT_UEVENT`) that
   the kernel and `systemd-udevd` broadcast on.  **Netlink delivery is
   per-network-namespace.**  A bridge-networked container has its own
   netns, so those uevents (broadcast in the host's netns) never reach
   libusb's listener inside the container.  libusb's device list is
   maintained by that event thread, so with no events it stays frozen —
   exactly the "50 iterations, same 8 cached devices" symptom above.

So the `/run/udev` mount fixes path (1), **not** path (2).  `--network
host` puts libusb's netlink socket back in the namespace where the
uevents are broadcast, which is why it — and only it — makes cold start
work on `bunsen`.

This **refines** (does not simply overturn) the reference-hardware
result: the variable not controlled for in the original test was the
container's netns.  The most likely reconciliation is that the
reference run shared the host netns (or its kernel delivered uevents
across netns); the open item is to confirm the reference container's
networking.  Until then, treat the rule as: **the `/run/udev` mount is
required for enumeration, and a shared (host) network namespace is
required for hotplug-driven re-acquire after the firmware-upload
re-enumeration.**

Practical consequences:

- **Hot start** (device present at `libusb_init`) needs neither host
  netns nor hotplug — the one-time enumeration scan finds it.  This is
  why pre-loading firmware sidesteps the whole problem.
- **Cold start** (radiod uploads firmware → FX3 drops to `00f3` →
  returns as `00f1`) *is* a hotplug event, so a harness that must
  exercise it needs the container in the host network namespace.
- This is not a `sleep(1)` problem and not fixable by more polling — the
  list never refreshes without events.  It is also not an
  rx888-driver/firmware issue: it is a libusb-in-a-namespaced-container
  property.  **Decision (driver-eval branch): the harness moved to
  `--network host` uniformly** so cold start works everywhere; multicast
  determinism is preserved by pinning everything to loopback (radiod
  defaults to `lo`, consumers pin `-I lo`/`,lo`).  See §2 for the
  interface-pinning details.

### 2. TUNERSTDBY (0xB8) on the HF path *(RESOLVED upstream at `87567fa`)*

ka9q's `rx888_set_hf_mode()` and `rx888_start_rx()` both fired
`command_send(...,TUNERSTDBY,0)`.  SDDC firmware removed all R82xx
commands (see `docs/LICENSE_ANALYSIS.md`), so 0xB8 returned a clean
USB STALL.  Initially judged cosmetic (two STALLed control transfers
per session); the `ka9q_test.sh` restart soak later reclassified it as
**blocking** — radiod intermittently wedged at the 0xB8 STALL on a
restart (gdb: `main` in `rx888_setup`, no streaming threads).  That
became container patch `04-no-tuner-stdby`.

**Upstreamed at `87567fa`:** Phil removed the `rx888_start_rx` send
outright and wrapped the `rx888_set_hf_mode` one in
`#if 0 // not reimplemented yet in firmware`.  Patch `04` is therefore
retired (`.disabled`) — with `01`/`02` already upstream, the container
now builds **vanilla ka9q-radio with zero local patches**.

### 3. R82xx VHF path *(deferred, out of scope)*

ka9q's `rx888.c` issues `R82XX_ATTENUATOR` and `R82XX_VGA` on the
VHF init path, plus `TUNERINIT` and `TUNERTUNE` inside an `#if 0`
block.  VHF will not work end-to-end because SDDC removed every
R82xx code path; ka9q itself flags VHF as broken in a top-of-file
comment ("VHF tuner does not work yet -- KA9Q, 17 Aug 2024").
HF is unaffected.

### 4. GPIO LED bit-position mismatch *(cosmetic)*

| Bit | ka9q (`rx888.h`) | SDDC firmware (`docs/architecture.md` §"GPIOFX3 bitmask protocol") |
|-----|-------|----------|
| 10  | `LED_YELLOW` | unused |
| 11  | `LED_RED`    | `LED_BLUE` |
| 12  | `LED_BLUE`   | unused |

Driving `LED_BLUE` from ka9q is a no-op on hardware; driving
`LED_RED` lights the blue LED. No streaming impact. Either side
could converge to the other's mapping; deferred.

### 5. SuperSpeed gate *(host topology, not SDDC)*

ka9q's `rx888.c` rejects anything below `LIBUSB_SPEED_SUPER`
silently inside its device scan loop, then exits with the same
"device could not be found" string.  If the device is plugged into
a USB-2 port or behind a hub that downgrades, ka9q reports
identical-looking output.  Worth flagging in user docs.

### 6. PID `0x00F1` matches ✅

ka9q's `Loaded_product_id = 0x00f1` in `rx888.c` matches SDDC
firmware's advertised PID (see `docs/architecture.md` §"USB
descriptors").

### 7. STARTADC settling

SDDC firmware's `STARTADC` handler polls `si5351_pll_locked()` for
up to ~100 ms after programming the PLL, returning as soon as PLL A
reports lock (see `docs/architecture.md` §"Clock synthesis").  ka9q
also sleeps ~1 s host-side around its direct Si5351 programming in
`rx888_set_samprate()`.  The firmware-side wait used to be a fixed
~1 s sleep before commit `13b2091`; together with ka9q's host-side
sleep the round trip was ~2 s.  As of v0.1.0 the firmware contributes
≤100 ms and the round trip is dominated by ka9q's host-side wait.

### 8. Show-stopper: missing `STARTADC` before `STARTFX3` *(resolved firmware-side in v0.1.0)*

> **Resolved in firmware** (commit
> [`13b2091`](https://github.com/ringof/rx888-firmware/commit/13b2091),
> released in **v0.1.0**).  `si5351_clk0_enabled()` now reads CLK0_CONTROL
> register 16 bit 7 over I2C instead of consulting a stale
> `glAdcClockEnabled` host-cache flag.  `GpifPreflightCheck()` therefore
> sees the live chip state, ka9q-radio's direct Si5351 programming
> path passes preflight without `STARTADC`, and patch 03 has been
> retired (kept in-tree as `.patch.disabled` for archaeology — see
> `docker/ka9q-radio/patches/README.md`).  The analysis below is
> preserved as a record of the original failure mode.


ka9q programs the Si5351 directly via raw `I2CWFX3` writes (in
`rx888_set_samprate()`) and intentionally never calls `STARTADC`
(the `docker/ka9q-radio/README.md` originally documented this as a
feature: "bypasses firmware STARTADC").  Against stock RX888
firmware where `STARTADC` only does Si5351 setup, this works.

Pre-v0.1.0 SDDC firmware tracked ADC-clock readiness via a separate
global flag `glAdcClockEnabled` in `SDDC_FX3/driver/Si5351.c`, set
`CyTrue` *only* inside `si5351aSetFrequencyA(freq>0)`, called *only*
from the `STARTADC` handler in `SDDC_FX3/USBHandler.c`.  The
`STARTFX3` handler then ran `GpifPreflightCheck()` in
`SDDC_FX3/StartStopApplication.c` before starting the GPIF state
machine:

    if (!si5351_clk0_enabled())  return CyFalse;  // glAdcClockEnabled
    if (!si5351_pll_locked())    return CyFalse;

`si5351_clk0_enabled()` returned the bare flag — it did not query
the chip — so even though ka9q had correctly programmed Si5351 via
I2C and the PLL was physically locked, the flag remained `CyFalse`.
`STARTFX3` stalled EP0, GPIF never started, no bulk data flowed,
and radiod timed out:

    rx888 running
    No rx888 data for 5 seconds, quitting

By comparison, `rx888_stream` (the test harness) sent the commands
in the order SDDC required:

    DAT31_ATT → AD8340_VGA → STARTADC(samprate) → STARTFX3 → ...

The original container-side fix was to insert
`command_send(...,STARTADC,samprate)` before `STARTFX3` in
`rx888_start_rx()`: STARTADC reprograms Si5351 to the same
frequency ka9q already wrote (harmless duplicate), sets
`glAdcClockEnabled = CyTrue`, polls PLL lock (already locked,
returns immediately), and `STARTFX3`'s preflight then passes.

That fix shipped as `docker/ka9q-radio/patches/03-startadc-before-startfx3.patch`
through commit `13b2091`, at which point the firmware-side fix in
§8's resolution banner above made the host-side patch unnecessary.
The disabled patch file remains in-tree as
`03-startadc-before-startfx3.patch.disabled` for archaeology; see
`docker/ka9q-radio/patches/README.md`.

### 9. TESTFX3 (0xAC) — now exercised at `87567fa` *(firmware version logged)*

Historically ka9q's `rx888.c` never issued `TESTFX3`: the plugin read
no model/version word, so SDDC firmware's `FIRMWARE_VER_MAJOR` /
`FIRMWARE_VER_MINOR` could change between releases (e.g. the v0.1.0 bump
from 2.2 to 2.3) without any host-side coordination.

**As of `87567fa`** the driver sends `TESTFX3` at startup and logs the
reply (`control_recv(...,TESTFX3,...)` → 4 bytes `[hw model, fw major,
fw minor, req count]`), printing `RX888 hardware 0x.., firmware u.u`.
It's read-and-log only — there is still no version *comparison* or
gating — so no host coordination is required, but it does mean radiod
now surfaces the firmware version, which is a useful cross-check that
the loaded `.img` is the one you expect.

### 10. `6a5094ac` rx888 driver segfaults in `command_send` *(ka9q-side regression; blocks the bump)*

We attempted to bump the container from `42273761` to `6a5094ac`
(2026-05-02) to pick up the change that flips the rx888 driver's
init-time USB reset to default **off** (config key `reset`, default
false) — which would have let us drop the `hack_no_usb_reset = yes`
workaround. The bump had to be reverted: at `6a5094ac` `radiod`
**segfaults during rx888 setup**, before it ever finds the device.

It prints up through:

```
Dynamically loading rx888 hardware driver from /usr/local/lib/ka9q-radio/rx888.so
Segmentation fault (core dumped)
```

`gdb` backtrace:

```
Program received signal SIGSEGV, Segmentation fault.
#0  0x00007ffff7d00855 in ?? ()         /lib/x86_64-linux-gnu/libusb-1.0.so.0
#1  command_send ()                      /usr/local/lib/ka9q-radio/rx888.so
#2  rx888_setup ()                       /usr/local/lib/ka9q-radio/rx888.so
#3  loadconfig ()                        radiod
#4  main ()                              radiod
```

`rx888_setup()` calls `command_send()`, which calls into libusb and
crashes there. A SIGSEGV *inside* libusb on a control transfer is the
signature of a NULL/invalid device handle being handed to
`libusb_control_transfer` — i.e. at this SHA the setup path reaches
`command_send()` before it holds a valid handle (or after one was
invalidated).

Reproduced on **both** device states — with the FX3 in the bootloader
(`04b4:00f3`) *and* with firmware already loaded and the device healthy
at `04b4:00f1` — so it is **not** the firmware-upload path and **not**
fixable by pre-loading firmware. It is an unconditional crash in
`6a5094ac`'s rx888 init.

Status: was **reverted to `42273761` + `hack_no_usb_reset = yes`** (the
proven pair with ka9q-web `b63c991`). This was a candidate upstream
report to KA9Q; the backtrace above is the concrete evidence.

**Update (driver-eval bump to `21d51fac`, see §12):** a later SHA is now
under evaluation. At `21d51fac`, `rx888_usb_init()` acquires and NULL-checks
the device handle *before* any control transfer, and the init-time USB reset
defaults OFF — the structural conditions that produced this crash at
`6a5094ac` are addressed. Whether the crash is gone in practice is a bench
question (the build/run is decisive); if `radiod` segfaults in `rx888_setup`
again, revert to `42273761`.

### 11. radiod restart stall *(ka9q-side; firmware exonerated)*

Under the `ka9q_test.sh` multi-cycle soak, `radiod` intermittently stalls when
restarted in the same container: it finds the device at `00f1`, programs the
Si5351, sets gain, sends `TUNERSTDBY` (0xB8) — then hangs *before* `rx888
running`, never starting the stream. (Distinct from the cycle-1 "no spectrum",
which is the avahi/mDNS and `powers` dynamic-channel timing on restart.)

**The firmware is not the cause.** `tests/fx3_cmd resetup_cycle` reproduces a
host's full re-setup restart with no SDR app in the loop — per cycle: park ADC
in SHDN standby (`STOPFX3`, #131), dwell, then re-init clock + GPIO + atten/VGA
+ `TUNERSTDBY` + `STARTFX3` (firmware wakes the ADC, settles, starts the GPIF)
+ a primed bulk read. It **passes every time** across:

- the vendor-command sequence + 5 ms inter-command timing,
- ADC wake-on-restart timing — `RESETUP_STANDBY_MS` = 0 / 40 / 200 ms (rules
  out `ADC_WAKEUP_SETTLE_MS` being too short after a long standby),
- `RESETUP_REOPEN=1` — a brand-new libusb handle each cycle
  (open → claim → stream → stop → release → close), mimicking radiod restarting
  as a fresh process.

So the firmware streams reliably across the full restart surface; the stall is
in `radiod`'s own per-restart USB/URB handling (it hangs *after* claiming the
device, in its streaming-transfer setup). Out of scope for the firmware; raise
upstream with the `resetup_cycle` evidence. Note also that #131's actual claim
— device returns to a usable, ADC-parked idle between sessions — is covered
without ka9q-radio at all by `fw_test`/`soak` (fx3_cmd-side) and the green
`ka9q_smoke` real-output gate.

### 12. Driver-eval: host-side Si5351 synthesis — pinned `87567fa` *(validated on hardware; zero patches)*

KA9Q asked us to bench the new `rx888.c` revision on ka9q-radio `main`. The
container's `KA9Q_RADIO_SHA` was bumped `42273761` → `21d51fac` → **`87567fa`**
(and ka9q-web `b63c991` → `91cbfca`, newest-with-newest) on branch
`Claude/ka9q-driver-eval`.  `21d51fac` was the first host-side-Si5351 revision
we validated; `87567fa` is the follow-up that landed after, incorporating the
fixes below — including the upstreaming of our last local patch.

**What `87567fa` adds over `21d51fac`** (all observed in the `rx888.c` diff):

- **Both `TUNERSTDBY` sends removed** (one deleted, one `#if 0`'d) — this *is*
  patch `04`, now upstream. With `01`/`02` already upstream, the container
  builds **vanilla ka9q-radio, zero local patches**.
- **~10 microsleeps removed** (the `usleep(5000)` inter-command delays, a
  `usleep(100000)`, and a `usleep(1000000)`).
- **Si5351 lock poll** replaces a blind post-clock sleep — polls reg 0 bit5
  (LOL_A) and reg 16 bit7 (CLK0_PDN) for ~50 ms until locked + running, warns
  `RX888 ADC clock not locked/running` otherwise. Reads CLK0 state back,
  dovetailing with this firmware's GPIF preflight.
- **Firmware version read + logged** via `TESTFX3` (see §9).
- The post-upload `sleep(1); // how long should this be?` is **unchanged** —
  the cold-start nit (audit §1) still stands.

**What the new driver changes.** The headline is that Si5351 clock synthesis
moves **host-side**: `rx888.c` now `#include`s a new `si5351.{c,h}` module and
calls a pure-integer PLL solver (`si5351_solve`, `si5351_get_pll_pvals`,
`si5351_get_ms_pvals`), then writes the PLL / MultiSynth registers itself over
`I2CWFX3`. Previously the firmware owned more of that. (`si5351.c` is
self-contained — libc + its own header — and compiles clean standalone.)

**Compatibility analysis (static, pre-build):**

- *Not a file-swap onto the old pin.* `42273761` has no `si5351.{c,h}`, so the
  new `rx888.c` only builds inside its own tree — hence the whole-SHA bump
  rather than a graft. The `SI5351_*` register macros were already in
  `rx888.h` (unchanged); `rx888.h`'s only delta (`DEFAULT_IMAGE_FILE`) is
  unreferenced by `rx888.c`.
- *Plugin ABI.* The exported interface is a **superset** of the old one (adds
  `rx888_shutdown`), so it stays loadable by the same `radiod` model.
- *Patches.* **None.** `01`/`02` (powers float/double) and `04` (no-tuner-stdby,
  upstreamed at `87567fa`) are all retired to `*.disabled`; the container builds
  vanilla ka9q-radio.
- *Config.* `hack_no_usb_reset` is gone, replaced by `reset` (default false);
  `rx888-test.conf` now sets `reset = no` explicitly.
- *ka9q-web.* Its linked headers barely moved (`multicast.h` unchanged,
  `status.h` +1 appended enum, `misc.h` minor); rebuilt against the same tree.

**Bench results (RX888mk2 + SDDC_FX3, `21d51fac`; re-confirm at `87567fa`):**

| Check | Result |
|-------|--------|
| §10 `command_send` segfault | **did not recur** — `radiod -v` reached `rx888 running` (hot start) |
| Si5351 host synthesis | **correct** — `vco = 27 MHz·(28+4/5) = 777.6 MHz`, `÷12 = 64.8 Msps`, clean integer fractions |
| Live spectrum | **PASS** — `ka9q_smoke.sh`: ~−132 dB textured floor, fs/2 alias +53 dB |
| §11 restart stall | **did not recur** — `ka9q_test.sh` 15 cycles + 3 force-reloads, 18/18 pass, ADC parked (gpif idle, dma frozen) each stop |
| Cold-start re-acquire | **works under `--network host`** (netns/hotplug — §1); fails under bridge |

The `21d51fac` results stand; `87567fa` only *removes* code on these paths
(tuner sends, microsleeps) and *adds* a clock-lock poll, so re-running the same
`ka9q_smoke.sh` + short `ka9q_test.sh` on `87567fa` is the confirmation — watch
for the new `RX888 ... firmware u.u` and (absence of) `ADC clock not locked`
lines in `radiod -v`.

## Container-side requirements (no patches needed)

A bind-mount of `/run/udev:/run/udev:ro` is **required** at
`docker run` time so libusb's hotplug listener inside the container
sees host udev events.  Without it libusb's cached device list never
updates after the FX3 re-enumerates, and `rx888_usb_init()` exits
with "Error or device could not be found".  See §1 for the analysis.

## Patches applied in this container

**None as of v0.1.0.**  The only patch that ever shipped (patch 03 —
inserting `STARTADC` before `STARTFX3`) was retired once the firmware
began reporting Si5351 CLK0 state truthfully; see §8.  The disabled
patch is preserved in-tree as
`docker/ka9q-radio/patches/03-startadc-before-startfx3.patch.disabled`
and is skipped by the Dockerfile's `*.patch` glob.  See
`docker/ka9q-radio/patches/README.md` for the historical record.

The findings in §1 (`sleep(1)`) and §2 (TUNERSTDBY) are documented
above but do **not** become container patches: §1 has a working
container-level workaround (the udev mount), and §2 is purely
cosmetic.  Carrying patches we don't strictly need would dilute any
future ask we make of the upstream maintainer.

## Open work

- Long-run streaming stability against the v0.1.0 container is not
  yet measured.  Initial confirmation in v0.1.0 prep: container
  reaches "rx888 running" and starts streaming without host-side
  patches.
- File issues for VHF support (firmware-side R82xx return) and the
  LED bit-position decision; both are non-blocking for HF receive.
- Report the `6a5094ac` rx888-driver segfault (§10) upstream to KA9Q,
  with the `gdb` backtrace, and retest a later SHA once it lands — the
  bump is desirable (drops `hack_no_usb_reset`) but blocked until the
  crash is fixed.
