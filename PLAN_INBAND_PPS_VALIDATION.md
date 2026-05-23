# In-band PPS — GPIO Loopback Validation Plan

*Staged bring-up and test of the GPIO 18/19 control path for the in-band PPS
marker (issue #125), using a 100 kΩ hardware loopback to inject a
software-simulated PPS before a real GPS PPS is attached.*

---

## Hardware premise

A **100 kΩ resistor bridges GPIO 18 ↔ GPIO 19** on the test unit. GPIO 18
drives; GPIO 19 either reads it back (Phase 1) or acts as the GPIF **CTL[0]**
input the PPS comparator watches (Phase 2 onward). All later phases assume this
resistor is present.

## Current firmware state (audit)

- **IO matrix** (`StartUp.c`): `gpioSimpleEn[]`/`gpioComplexEn[]` are all zero.
  GPIOs are claimed individually via `CyU3PDeviceGpioOverride()`
  (`RunApplication.c:69`), so freeing GPIO 19 for the GPIF is a matter of *not
  overriding* it — no IO-matrix bitmap edits required.
- **GPIO 18/19** (`radio/rx888r2.c`): both configured `ConfGPIOsimpleout`
  (outputs), driven from the bias bits in `rx888r2_GpioSet()`.
- **Input capability already exists**: `ConfGPIOsimpleinput()` /
  `ConfGPIOsimpleinputPU()` (`RunApplication.c:91-92`); read via
  `CyU3PGpioGetValue`.
- **`SYNTH_PPS` (0xB7)** vendor command + `synth_pps.c` ThreadX timer already
  exist — repurposed in Phase 4 as the GPIO 18 PPS driver (replacing the
  dead `CyU3PDmaMultiChannelSetWrapUp` path).

## Two build configurations

Selected at compile time by `#define PPS_CTL_ENABLE`, **default OFF** so the
production `main` build and behavior are untouched.

| Config | `PPS_CTL_ENABLE` | GPIO 18 | GPIO 19 | Phases |
|---|---|---|---|---|
| **A** | off | simple GPIO | simple GPIO | 1 |
| **B** | on | simple output (PPS driver) | released → GPIF **CTL[0]** | 3, 4, 5 |

Phase 2 is the transition from A to B.

## Risks to retire (rationale for the phased order)

1. **Reading through 100 kΩ.** Valid for a high-Z input *without* an internal
   pull-up. Use plain `ConfGPIOsimpleinput` (not `…inputPU`) so the 100 k
   dominates leakage. *Phase 1 proves this.*
2. **Soft edge into a CTL pin.** 100 kΩ × input capacitance yields a
   ~hundreds-of-ns edge; at 64–128 MHz PCLK that spans several clocks — risk of
   the comparator registering bounce/multiple matches instead of one.
   *Phases 1 & 4 test for exactly one edge.*
3. **GPIO 19 ↔ CTL[0] mapping.** The GPIF II Designer assigned PPS to
   "GPIO_19" and emitted comparator mask `0x1` (CTL bit 0). *Phase 2 confirms
   that releasing GPIO 19 actually lands it on CTL[0] as assumed.*

---

## Phase 1 — Loopback sanity (Config A, both GPIO)

- **Firmware:** a **temporary** vendor command (new, never-recycled code) that:
  reconfigures GPIO 19 → input (no pull); drives GPIO 18 low, reads 19; drives
  18 high, reads 19; restores GPIO 19 → output; returns the two readbacks.
  Removed after Phase 1 passes.
- **Test:** host issues the command; assert readbacks `= {0, 1}`.
- **Pass:** the 100 kΩ loopback faithfully transfers both logic levels. (Failure
  here means the resistor/path is wrong — everything downstream is blocked.)

## Phase 2 — Reconfigure to Config B

- **Firmware:** behind `PPS_CTL_ENABLE`, drop GPIO 19 from
  `rx888r2_GpioInitialize()`'s simple-out list and from `rx888r2_GpioSet()`;
  keep GPIO 18 as output. Confirm the GPIF loads with GPIO 19 as CTL[0].
- **Test:** build + load; confirm USB enumeration and that
  `CyU3PGpifGetSMState` reports a live SM. No streaming assertions yet.
- **Pass:** boots clean in Config B; GPIO 18 still controllable.

## Phase 3 — Baseline regression, GPIO 18 idle (Config B)

- **Firmware:** none beyond Phase 2; GPIO 18 left untouched (no PPS injected).
- **Test:** full existing suite (`tests/fw_test.sh`) + 1-hour soak +
  ka9q-radio docker.
- **Pass:** behavior identical to production — streaming, start/stop, watchdog,
  GETSTATS all unaffected by releasing GPIO 19 → CTL and loading the 14-state
  waveform. This is the "did we break the radio" gate.

## Phase 4 — Software-PPS injection, full SM regression (Config B)

- **Firmware:** rework `synth_pps.c` so its timer **toggles GPIO 18** (driving
  CTL[0] via the loopback) instead of calling `SetWrapUp`; `SYNTH_PPS`
  start/stop/oneshot drive it.
- **Test:** re-run the *entire* GPIF suite **with PPS actively toggling** —
  streaming integrity, soft-stop reaches IDLE, watchdog wedge/recovery, no
  overrun — and confirm short transfers appear with **exactly one commit per
  edge** (Risk 2).
- **Pass:** PPS injection never degrades any existing SM function, and markers
  are produced.

## Phase 5 — End-to-end injection + detection (host side)

- **Host (`rx888_stream`):** detect short transfers, compute marker sample
  position (`total_samples + actual_length/2`, 16-bit samples), and reconstruct
  dropped markers from the doubled inter-marker interval.
- **Test:** drive a known PPS rate via GPIO 18; confirm the host detects the
  correct cadence and positions; long run for dropped-marker statistics.
- **Pass:** detected marker stream matches the injected rate within the
  ±1–2 PCLK synchronizer tolerance.

---

## Decisions (resolved)

- Config switch: **compile-time** `PPS_CTL_ENABLE`, default OFF.
- Phase 1 command: **temporary**, removed once the loopback is confirmed.
