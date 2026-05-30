# PLAN — ka9q-radio Docker integration harness

End-to-end firmware validation that drives the real host stack
(`radiod` + the `rx888` plugin) against an attached RX888mk2, cycling the
receiver and correlating captured `radiod` logs with firmware state. This
exercises the `STARTADC` / `STARTFX3` / `STOPFX3` paths through the actual
consumer of the firmware rather than through `fx3_cmd` synthetic commands —
and validates that the ADC returns to a non-streaming (standby) state
between sessions (issue #131).

## Constraints (what shapes the design)

1. **Hardware-only, never GitHub CI.** Requires a real RX888 on USB and
   `--privileged` Docker with `/dev/bus/usb` passthrough. Same class as
   `tests/soak_test.sh`.
2. **Exclusive device access.** While `radiod` holds the USB interface, the
   host-side `tests/fx3_cmd` cannot claim it. The harness therefore
   alternates: **radiod up** → assert on logs (and, Phase 2, on the output
   multicast); **radiod down** → run `fx3_cmd stats` to confirm the device
   parked cleanly.
3. **"Start/stop the receiver" = the `radiod` session, not channels.**
   Creating/destroying a ka9q demod channel keeps the device streaming;
   only `radiod` opening/closing the device drives the firmware
   start/stop path. So the unit of work is start/stop (or restart) of
   `radiod`.
4. **No journald in the container.** The image runs
   `radiod /etc/radio/radiod@rx888-test.conf` in the foreground
   (`entrypoint.sh` → `exec "$@"`), so logs go to stdout/stderr. The
   harness captures those (chosen over a native systemd install).

## Existing assets reused

- `docker/ka9q-radio/Dockerfile` — builds `radiod`, `rx888.so`, `control`,
  `tune`, `monitor`; CMD is `radiod /etc/radio/radiod@rx888-test.conf`.
- `docker/ka9q-radio/rx888-test.conf` — ADC `samprate = 64m8`, status group
  `hf.local`, output stream `wwv-pcm.local`, firmware at
  `/firmware/SDDC_FX3.img`.
- `docker/ka9q-radio/ka9q.sh` — container start/stop/console helpers and the
  exact `docker run` invocation (privileged, USB + udev mounts, host
  network, wisdom volume).
- `tests/fx3_cmd` — host tool; `stats` decodes GETSTATS (GPIF SM state, DMA
  count, Si5351 CLK0 state).
- `tests/fw_test.sh` — TAP-style output conventions to mirror.

## Cycle model

Recommended: **persistent container, exec-driven radiod.** Start the
container once with CMD overridden so it idles
(`docker run ... ka9q-radio sleep infinity` — `entrypoint.sh` still runs
dbus/avahi/wisdom setup, then `exec sleep infinity`). Per cycle:

1. `docker exec -d ka9q-radio sh -c 'radiod /etc/radio/radiod@rx888-test.conf > /tmp/radiod.<n>.log 2>&1'`
2. Wait for the device to be claimed and a healthy-start marker to appear in
   the log.
3. Assert on the captured log (see below).
4. Stop cleanly: `docker exec ka9q-radio pkill -INT radiod` (SIGINT → clean
   shutdown → `STOPFX3`); poll until the process is gone and the USB device
   is released.
5. Run host `tests/fx3_cmd stats`; assert the device parked.

This keeps dbus/avahi/wisdom warm across cycles. Simpler fallback:
fresh `docker run --rm -d` per cycle with `docker logs` capture, at the cost
of ~1 s avahi/dbus startup churn each iteration.

## Forced firmware reload

The harness needs a way to force a fresh firmware **reload** between cycles
(not just a radiod restart against the already-resident RAM image), so the
upload path and cold-start behaviour are exercised too. We deliberately use
**both** reset mechanisms — the firmware vendor reset and a host-side USB
reset — because they cover different failure modes and reflect how the
device is actually driven in the field:

- **`RESETFX3` (0xB1) — firmware vendor reset.** Already exposed as
  `fx3_cmd reset` but, notably, **not currently used by anything** in the
  toolchain or by ka9q-radio. It reboots the FX3 to the bootloader
  (PID `0x00F3`) so the next `radiod` start re-uploads
  `/firmware/SDDC_FX3.img` (or `fx3_cmd load <img>`). This is the clean,
  in-band reload path and exercising it is itself valuable coverage. It
  requires the device be reachable and free (radiod stopped) to accept the
  command.
- **Host-side `usbreset` (`USBDEVFS_RESET`) — the universally-relied-upon
  kick.** Because the *old* firmware wedged so readily, essentially every
  real workflow recovers the device with a host USB reset; the harness must
  reproduce that path. It forces the kernel to re-enumerate the port and
  drop stale handle/endpoint state even when the device is half-claimed or
  wedged and `RESETFX3` can't be delivered. Bundled into `fx3_cmd` as the
  `usbreset` subcommand (DONE) so the harness has no dependency on an
  external `usbreset` binary. **Implemented via the raw `USBDEVFS_RESET`
  ioctl on the `/dev/bus/usb/BBB/DDD` node, NOT `libusb_reset_device()`** —
  the latter must open/claim the device, which fails on exactly the wedged
  state we're recovering. `fx3_cmd usbreset` only enumerates (reads
  descriptors, no claim) to locate the bus/address, then issues the ioctl,
  so it works even when libusb can't claim. Linux-only, which matches the
  Docker-on-Linux harness.

Using both, in sequence, is intentional: `RESETFX3` is the in-band path we
want to start validating (nothing else does), and the host `usbreset` is the
out-of-band recovery everyone already depends on — together they cover both
"firmware is healthy enough to take a command" and "firmware is wedged and
only a bus reset gets us back."

**Bench finding (corrects an earlier assumption).** On the RX888mk2,
`USBDEVFS_RESET` does *not* leave a running FX3 loaded — the port reset
reboots the chip straight into the **bootloader** (observed
`04b4:00f1` → `04b4:00f3`, new bus/address). So on this hardware `usbreset`
is itself a force-reload, not merely a re-enumeration. Two consequences:

1. The ioctl returns `ENODEV` because the original `/dev/bus/usb` node
   disappears mid-reset (the device re-enumerates as a new one). This is
   success, not failure — `fx3_cmd usbreset` now treats `ENODEV` as PASS,
   same "device disconnected on success" semantics as `RESETFX3`.
2. **Anything that issues `usbreset` (or `RESETFX3`) must re-upload firmware
   afterward** — the device comes back in DFU. Any `fw_test`/soak that uses
   it needs a re-upload step (`-F <img>` / `fx3_cmd load`, or radiod's
   auto-upload) before the device is usable again. The teardown paths added
   in `fw_test.sh` / the soak runner deliberately do **not** use `usbreset`
   for exactly this reason — they `STOPFX3` + assert SHDN to leave a usable,
   ADC-parked device; `usbreset` belongs only in the reload cycle.

The reload primitive is now a single `fx3_cmd` subcommand (DONE):
`fx3_cmd [-F <img>] reload` does RESETFX3 (in-band, if the device is in app
mode and claimable) **and** a host-side `usbreset`, waits for the bootloader
PID `0x00F3`, re-uploads the image, and verifies the device answers TESTFX3
at the app PID. Harness `force_reload()` therefore = stop radiod →
`fx3_cmd -F <img> reload` → next radiod start re-validates. (A literal power
cycle that clears RAM still needs hardware — switched hub / replug — and is
out of scope for a software harness.)

**Soak cadence:** this is a next-level soak — an hour-long run of radiod
start/stop cycles with `force_reload()` fired on a **time interval of
~5–10 minutes** (not every cycle), so most cycles are fast restarts that
churn the streaming start/stop path while periodic reloads exercise the full
reset + re-upload path. Interval and total duration are configurable.

## Assertions

**On start (radiod up, from the captured log):**
- rx888 plugin initialized and the configured ADC rate confirmed
  (`64m8` / `64800000`).
- mDNS publication succeeded (e.g. `Established under name` for `hf.local` /
  `wwv-pcm.local`).
- Absence of error markers: a denylist grep for `error`, `USB`, `overrun`,
  `timeout`, `failed`, `Can't`, PIB/clock-loss strings.

**On stop (radiod down, from host `fx3_cmd stats`):**
- GPIF SM is idle (state `0` or `1`).
- DMA count is frozen across two reads spaced ~100 ms (no streaming).
- Si5351 CLK0 state recorded (radiod's teardown behaviour for the clock is
  observed, not assumed).

## Known limitation — direct SHDN confirmation

GETSTATS does **not** read back the `SHDN` GPIO output level, so the harness
cannot directly prove the ADC is in standby — it can only confirm streaming
stopped and the device is idle. Three ways to close that gap, in order of
effort:

1. **Trust the firmware path** (code-verified in `USBHandler.c` STOPFX3 →
   `rx888r2_AdcStandby(CyTrue)`); the harness asserts the observable
   "stopped/idle" state. (Default.)
2. **Small firmware enabler (optional follow-up):** add an `SHDN` GPIO
   readback byte to the GETSTATS payload so tooling can assert the standby
   level directly. Cheap and broadly useful.
3. **Bench ammeter** for the absolute ≥330 mA power claim — manual, not
   automatable.

## Phasing

1. **Lifecycle + logs + forced reload — FIRST CUT (build & test this now).**
   Hour-long soak of radiod start/stop cycles; `force_reload()` every
   ~5–10 min; assert the start/stop log markers and the post-stop
   device-idle state via `fx3_cmd stats`. Highest value, lowest complexity;
   directly validates the #131 stop→standby path through the real host and
   exercises both reset mechanisms. **Get this done and green before
   anything else.**
2. **Data plane — SECOND CUT. DONE (pending bench).** Uses ka9q's `powers`
   tool (added to the image) to pull a power spectrum from radiod each cycle
   — `powers` dynamically creates a spectrum channel via the status group
   (`hf.local`), so no conf change is needed — and asserts the spectrum
   looks like **live RF**, not a dead/frozen ADC. The metric is
   scale-independent: broadband noise keeps most bins within a dynamic range
   (`SPEC_DYN_RANGE_DB`, default 60 dB) of the spectrum's own peak, whereas a
   shut-down/frozen ADC FFTs to a lone DC spike with the rest near −∞ — so we
   require ≥ `SPEC_MIN_FRAC` (default 0.5) of bins to qualify. This is a
   stronger signal than a PCM byte-count (which noise alone would satisfy)
   and ties directly to the frozen-ADC failure mode discussed for #131.
   Auto-skipped if `powers` isn't in the image.
3. **Fault injection (stretch).** Assert `SHDN` or pull the Si5351 clock
   mid-stream and confirm `radiod` logs the degradation and recovers. NOTE:
   this collides with constraint #2 — `fx3_cmd` cannot touch the device
   while `radiod` holds it, so mid-stream injection needs a different
   mechanism (a firmware-side debug/injection vendor path, or reuse of the
   in-process injection already in the soak runner). Scope after Phases 1–2.

## Deliverables

- `tests/ka9q_test.sh` — host harness, TAP output mirroring `fw_test.sh`,
  reusing `ka9q.sh`'s container invocation. Cleanup trap stops radiod and
  the container, then parks the ADC via `fx3_cmd` (consistent with the new
  `fw_test.sh` / soak teardowns). **DONE (Phase 1) — pending bench
  validation.**
- `tests/fx3_cmd.c` — new `usbreset` subcommand via the raw `USBDEVFS_RESET`
  ioctl (enumerate-only, no claim; wedge-robust) for the forced-reload /
  wedge-recovery step, so the harness needs no external `usbreset` binary.
  **DONE.**
- `tests/fx3_cmd.c` — new `reload` subcommand (RESETFX3 + usbreset → wait
  bootloader → re-upload via `-F` → verify TESTFX3); the `force_reload()`
  primitive the soak calls. **DONE.**
- Short `docker/ka9q-radio/README.md` addition documenting the idle-container
  run mode used by the harness.
- No image rebuild required for Phase 1 (CMD override + `docker exec`).

## Resolved decisions

- **Environment:** assumes a host with an attached RX888 and exclusive
  device use (same premise as the existing Docker setup). Confirmed.
- **Cycle unit:** cycle `radiod` (start/stop the session). Confirmed.
- **Forced reload:** use **both** resets deliberately — the in-band
  `RESETFX3` vendor command (currently unused anywhere) and a host-side
  `usbreset` (the recovery everyone relies on). `force_reload()` chains
  them; add a new `fx3_cmd usbreset` subcommand; run on a configurable
  interval. Confirmed.

## Resolved (continued)

- **Scope:** Phase 1 only for the first cut — build and test it before
  starting Phase 2. Phase 2 (data plane) is a second cut; Phase 3 a stretch.
- **Soak cadence:** hour-long run, `force_reload()` every ~5–10 min, both
  interval and duration configurable.
- **GPIO readback:** the inability to read back SHDN (or any control GPIO)
  from the host is being raised on issue #131 as a broader "we should have
  GPIO state readback in GETSTATS" enhancement. Until that lands the harness
  uses the idle-state proxy; once it lands, add a direct standby-level
  assertion.
