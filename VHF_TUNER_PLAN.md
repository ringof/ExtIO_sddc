# Plan: Re-implementing VHF Tuner Support in rx888-firmware

> Status: **DIRECTION SET — Option C (host-side driver + minimal firmware
> command support).** Maintainer chose a host-side R828D driver with a
> "modicum of firmware command support." The license question (§2) is
> resolved by construction: tuner register logic lives on the host, so no GPL
> code re-enters the firmware binary. The firmware deliverable is **one CLKB
> clock command** (§2a.A / §3.4) plus a **host-side contract header** (§2a.B,
> §4) — the "header to manage the tuner chip" offered to Phil. The plan
> supports **both tuning models** (§2c): tune via the R828D chip
> (conventional) or via the Si5351 clock (Phil's preference, to be tested on
> his harness). Host-side driver + ka9q-radio VHF arrive as a **PR from Phil**
> that we cherry-pick (§4). Options A/B retained only as rejected
> alternatives.

## 0. Design goals (the lens for every decision below)

The firmware aims to be **easy to write a driver against, without getting in
the way of advanced users — while still making failure loud and impossible
to wedge.** Three goals; every specific choice in this plan is one of them:

- **G1 — Easy to write a driver.** One published contract header (§2a.B),
  sane defaults (HF path + CLKB off at boot), and a documented happy-path
  bring-up. The common case is short and obvious.
- **G2 — Don't get in the way of advanced users.** Ship **mechanism, not
  policy**: raw I2C passthrough to the R828D, **arbitrary** CLKB frequencies
  (enabling Model 2 and anything else), raw GPIO. The firmware never blesses
  a tuning model or rejects an unconventional sequence. The *only* reserved
  resource is the Si5351 — and only because it is shared with the ADC clock
  (a mechanism boundary, §2a.A, not an opinion about how to tune).
- **G3 — Prevent silent failure and wedging.** The two guarantees the
  firmware *does* insist on:
  - *No silent failure:* every command reports success/failure unambiguously
    (STALL or error status on failure, never silent success); the CLKB
    command **returns the achieved frequency**; unknown commands/args STALL.
    The host always knows what actually happened.
  - *No wedge:* bounded operations only — and the I2C path is properly
    bounded on `main` today. An absent device NAKs (immediate error); a
    stranded/clock-stretched bus is bounded by the **finite** `busTimeout`
    (`I2C_BUS_TIMEOUT` ≈ 4,032,000 core clocks ≈ 10 ms, from **PR #156 /
    issue #154**, present in `main`), well under the 2 s EP0 watchdog; and the
    EP0 watchdog backstops anything else. The VHF work must simply **not
    regress** this (the tuner is a new I2C consumer, addressable while
    absent/unpowered). Plus bounds-checked EP0 buffers and no unbounded loops
    in the vendor callback.

This trio resolves the Model 1 vs Model 2 tension (§2c) on its own: **G2**
says don't block the unconventional "tune via the clock" approach; **G3**
guarantees it can't wedge or fail silently. An advanced user who tries it
gets loud, recoverable feedback — mistuning, a reported lock failure, visible
image products — not a bricked stream. The firmware neither endorses nor
forbids it.

## 1. Background: what VHF support was, and where it went

The RX888 mk2 has two receive paths:

- **HF (0–32 MHz):** direct sampling through the on-board LTC2208 ADC.
  This is the *only* path the current firmware operates.
- **VHF/UHF:** an **R828D** tuner (R82xx family) downconverts to an IF that
  the ADC then samples. Driving it requires an R82xx tuner driver speaking
  I2C to the chip, plus GPIO/antenna-path switching.

### History (verified from git)

The current `main` history was **re-rooted**. `git rev-list --max-parents=0
HEAD` shows multiple root commits; the current tree starts fresh at
`2056a30` as an RX888 mk2-only firmware. The original, full SDDC_FX3
firmware — with every board variant *and* the tuner driver — is still
reachable in history via `--all` on the older root chain
(`ff82ddd` … `0ffa512`).

In the last full-featured commit (`0ffa512`), VHF support consisted of:

| Piece | Location (in `0ffa512`) | Notes |
|-------|-------------------------|-------|
| R82xx tuner driver | `SDDC_FX3/driver/tuner_r82xx.c` (**2,415 lines**) + `.h` | init, `set_freq64`, `set_gain`, `set_vga_gain`, `set_all_gains`, `set_sideband`, `standby`, `set_bandwidth`, filter calibration, register cache |
| Vendor commands | `SDDC_FX3/USBhandler.c` | `TUNERINIT (0xB4)`, `TUNERTUNE (0xB5)`, `TUNERSTDBY (0xB8)` |
| Argument sub-commands | `SETARGFX3` switch | `R82XX_ATTENUATOR`, `R82XX_VGA`, `R82XX_SIDEBAND`, `R82XX_HARMONIC` |
| Radio dispatch | `SDDC_FX3/radio/rx888r2.c` etc. | `r820_initialize()`, antenna/bias GPIO switching, multi-board `switch(HWconfig)` |

### Why it was removed (the load-bearing constraint)

From `README.md` → **Limitations**:

> The GPL-licensed R82xx driver has been removed to resolve a license
> conflict with the proprietary Cypress SDK.

This is the crux. `tuner_r82xx.c` is **GPL-2.0** (it descends from the
`librtlsdr` / Osmocom lineage). The Cypress FX3 SDK it links against is
**proprietary** and not GPL-compatible. A combined firmware binary that
statically links GPL tuner code with the proprietary SDK is, at minimum,
a license conflict the maintainer chose not to ship. **Any plan to bring
the tuner back into the firmware must resolve this first — it is not an
optional footnote.**

### What still survives in the tree today (scaffolding)

The strip-down left useful hooks behind:

- `SDDC_FX3/protocol.h`: GPIO masks `BIAS_HF`, `BIAS_VHF`, `VHF_EN` are
  still defined; `HWconfig` is reduced to `{ NORADIO, RX888r2 }`.
- `SDDC_FX3/radio/rx888r2.c`: still configures and drives
  `GPIO_BIAS_VHF (18)` and `GPIO_VHF_EN (35)`, and reports their state in
  the GETSTATS GPIO snapshot. The antenna-switch plumbing is intact.
- The command enum has a **gap at `0xB4`/`0xB5`/`0xB8`** — exactly where
  `TUNERINIT (0xB4)`/`TUNERTUNE (0xB5)`/`TUNERSTDBY (0xB8)` used to live
  (`0xB7` was always an unused slot). Re-using these opcodes keeps host-side
  compatibility with existing RX888 software (ka9q-radio, rx888_tools).
  **Drift already exists:** `tests/fx3_proto.h` *still* defines `TUNERINIT
  0xB4 / TUNERTUNE 0xB5 / TUNERSTDBY 0xB8` even though the firmware dropped
  them — see §2b.
- `docker/ka9q-radio/patches/04-no-tuner-stdby.patch` exists *only*
  because the firmware no longer answers `0xB8` (the host send STALLs with
  `LIBUSB_ERROR_PIPE`). Restoring a real `TUNERSTDBY` handler would let us
  **drop that patch** and use stock ka9q-radio behavior.

## 2. Decision: Option C — host-side driver, minimal firmware support

**Chosen.** The R828D register/tuning logic lives **on the host**, driven
over the firmware's existing `I2CWFX3`/`I2CRFX3` I2C passthrough. The
firmware contributes only a "modicum" of support: deterministic
antenna/bias **path switching** and (optionally) thin convenience opcodes.

Why this is the right call:

- **No license conflict, by construction.** The GPL tuner logic never
  re-enters the firmware binary that links the proprietary Cypress SDK. The
  host driver can be GPL (e.g. inside ka9q-radio / rx888_tools) with no
  conflict. This is exactly the direction `README.md` → Limitations already
  points users to, so the docs need no walk-back.
- **Smallest firmware surface.** Most of what's needed already exists:
  `rx888r2.c` drives the `BIAS_VHF`/`VHF_EN` GPIOs and the generic
  `GPIOFX3`/`I2CWFX3`/`I2CRFX3` commands are in place.
- **Upgradeable.** If a permissively-licensed firmware driver is ever
  wanted, the host probe + path-switch work here is the foundation for it.

### Rejected alternatives (for the record)

- **Option A — firmware-resident clean-room/permissive driver.** More
  effort, still needs a license decision; deferred, not chosen now.
- **Option B — re-vendor the GPL `tuner_r82xx.c` verbatim.** Re-introduces
  the precise license conflict the maintainer removed. **Rejected.**

### What "a modicum of firmware command support" means here

Concretely, after the command-by-command trace in §2a.0, the firmware side
reduces to **one irreducible thing plus existing primitives**:

1. **The one new command: set tuner reference clock (CLKB).** Reuses the
   already-present `si5351aSetFrequencyB()`; takes an arbitrary frequency,
   **returns the achieved value**, `freq=0` disables, default off. This is
   the *only* function the host cannot do itself, because the Si5351 is
   shared with the ADC clock (§2a.A). It serves **both tuning models** (§2c):
   set once (tune via the R828D), or reprogrammed per channel (tune via the
   Si5351). Restore it under the historical `TUNERINIT (0xB4)` / `TUNERSTDBY
   (0xB8)` pair (so stock ka9q-radio stops STALLing on `0xB8` and we can
   delete `04-no-tuner-stdby.patch`).
2. **Existing primitives the host already uses, just verified/documented:**
   I2C passthrough to the R828D (`I2CWFX3`/`I2CRFX3`), antenna/bias path
   GPIO (`GPIOFX3`), and `GETSTATS` readback. No new firmware code — these
   already exist; the work is the §2b contract documentation and a probe test.

Everything else the original fork did in firmware (tuner init, filter
calibration, `set_freq`, gain, sideband) moves host-side as I2C.

## 2a. Architecture: a firmware safety floor + a published driver contract

The firmware's job divides cleanly into two responsibilities. Getting this
split right is more important than any single command, because it is what
lets host driver authors do their job without re-deriving the protocol and
without being able to drive the hardware into a damaging state.

**Guiding principle:** the firmware owns *only* what the host cannot do
safely from across the USB boundary. After tracing the original fork's
tuner commands (§2a.0), that reduces to **one** thing: sole ownership of the
shared Si5351 clock generator. Everything else — register-level tuning,
gain, sideband, antenna-path GPIO — is the host driver's. This keeps the
firmware tiny, license-clean, and incapable of being driven into a
stream-killing state by a buggy driver.

### 0. What the original fork's tuner commands did, and where each lands now

From `Interface.h` + `USBhandler.c` at `0ffa512`. The decisive column is
whether the action touches the firmware-managed Si5351:

| Command | Opcode | Did | Touches Si5351? | Lands |
|---|---|---|---|---|
| `TUNERINIT` | `0xB4` | cfg + **CLKB on** + `r82xx_init` + filter cal | **Yes (CLKB on)** | FW: CLKB only; rest → host I2C |
| `TUNERTUNE` | `0xB5` | `r82xx_set_freq64` (LO) | No | Host I2C |
| `TUNERSTDBY` | `0xB8` | `r82xx_standby` + **CLKB off** | **Yes (CLKB off)** | FW: CLKB only; rest → host I2C |
| `R82XX_ATTENUATOR` | arg 1 | `set_all_gains` (LNA+mixer) | No | Host I2C |
| `R82XX_VGA` | arg 2 | `set_vga_gain` (IF VGA) | No | Host I2C |
| `R82XX_SIDEBAND` | arg 3 | `r82xx_set_sideband` | No | Host I2C |
| `R82XX_HARMONIC` | arg 4 | `// todo` (never implemented) | — | — |

**Of seven control points, exactly two touch the Si5351 — and it is the
same single action both times: turn the CLKB tuner-reference clock on
(`TUNERINIT`) and off (`TUNERSTDBY`).** Everything else is pure I2C to the
R828D, reachable today over `I2CWFX3`/`I2CRFX3`.

### A. The single real safety floor: firmware owns the Si5351

The tuner itself cannot stall the ADC stream — separate I2C device, separate
analog path, separate clock-domain output (CLKA = ADC sample clock vs CLKB =
tuner reference; the old standby zeroed CLKB without touching CLKA). **But
the Si5351 can**: CLKA and CLKB share one chip, one I2C device, and PLL
resources. A careless host register write reaching for CLKB — wrong
multisynth, a PLL or soft-reset bit — can glitch or drop CLKA and **kill the
ADC stream**. That is the one genuine hazard, and it is why:

1. **The firmware is the sole owner of the Si5351.** CLKB is reached *only*
   through a command, never host raw-I2C passthrough, so the firmware
   programs CLKB coherently against CLKA (the same path that already does
   `si5351aSetFrequencyA` + `si5351_pll_locked()` polling). The reference
   clock can never become collateral damage to the ADC clock.
2. **CLKB is command-gated and default-off.** Off at boot and in HF mode —
   no idle tuner-reference clock spurring into the ADC, less power ("turn off
   clocks we don't need"). The command carries an **arbitrary** frequency;
   **`freq = 0` disables CLKB**, exactly as the old `TUNERSTDBY` did via
   `si5351aSetFrequencyB(0)`. It may be set once (Model 1) or reprogrammed
   per-channel (Model 2) — see §2c; the firmware doesn't care which.
3. **The command resets PLLB only, and returns the achieved frequency.**
   `SetFrequencyB` already issues a PLLB-only reset (`0x80`), never touching
   PLLA/CLK0 (the ADC clock) — this isolation is load-bearing for Model 2's
   per-retune use. And because the Si5351 is fractional-N, the command must
   **report the frequency it actually synthesized**, so the host can know the
   true LO (Phil's "be sure of knowing the actual frequency").

This **is** the firmware deliverable: one command — *set tuner reference
clock (CLKB)* — reusing the already-present `si5351aSetFrequencyB()`, plus an
achieved-frequency readback. It absorbs the firmware halves of `TUNERINIT`
and `TUNERSTDBY` and nothing else. **The firmware never writes a tuner
tuning register** — both tuning models (§2c) are host policy on top of this
one primitive.

Two items that are *not* a floor, kept honest:

- **Finite I2C bus timeout — present on `main`, just don't regress it.**
  `main` sets `busTimeout = I2C_BUS_TIMEOUT` (≈ 4,032,000 core clocks ≈ 10 ms)
  from **PR #156 (issue #154)**. This matters because the I2C transfer runs
  inside the EP0 vendor handler: the old `0xFFFFFFFF` (~10.6 s) would block EP0
  past the **2 s** Level-4 watchdog and trip an unwanted reset; the finite
  value returns promptly (bumping `glCounter[1]`) instead. The VHF tuner is a
  new I2C consumer (addressable while absent/unpowered), so the only action
  needed is to **keep** this finite timeout. Related context from that effort:
  #157 (stream∧I2C wedge), #160 (`Si5351Init` clock-gating). Also bounds-check
  `wLength` against the EP0 buffer and keep STALL-on-error.
- **Host-crash bias-tee fail-safe (optional, decoupled).** Leaving antenna
  bias on after a host crash does not affect streaming; it is at most a
  hardware-tidiness choice. Decide consciously: a one-line bias-off on
  `STOPFX3`/reset, or accept "worst case is bad tuning." Not load-bearing.

Antenna/bias path selection (`BIAS_VHF`/`VHF_EN`) stays **host-side GPIO via
`GPIOFX3`**, with `GETSTATS` readback as feedback — a wrong combination only
mistunes, it cannot stall the stream, so it needs no firmware interlock.

### B. The published driver contract (what driver writers need anyway)

`protocol.h` is already the project-owned, license-clean contract (it is the
internalized former `Interface.h`): it defines the vendor opcodes, the full
GPIO bit map (`BIAS_VHF`, `VHF_EN`, `BIAS_HF`, …), and the radio model. The
work is to **promote it to the canonical published header and complete it**
with the things a host R828D author today has to reverse-engineer from
`USBHandler.c` + `i2cmodule.c`:

- **I2C passthrough wire format** (verified from `i2cmodule.c`
  `I2cTransfer(byteAddress, devAddr, byteCount, …)`): for `I2CWFX3`/
  `I2CRFX3`, **`wValue` = I2C device address (8-bit)**, **`wIndex` =
  register/byte address**, **`wLength` = byte count**, data in the EP0
  buffer; reads internally set `devAddr | 0x01`. Document this in-header
  instead of leaving it buried in the implementation.
- **R828D constants as published facts** (functional data, not GPL
  expression): device addr `0x74`, R820T `0x34`, ID register `0x00` /
  expected value `0x69`, R828D XTAL 16 MHz.
- **RF-path convention:** a documented `GPIOFX3` bit recipe for entering/
  leaving VHF (`BIAS_VHF`/`VHF_EN`), since the host owns path selection.
- **The Si5351 ownership rule, stated as a hard contract:** drivers do *all*
  tuner work over I2C passthrough but **must not write the Si5351 directly** —
  the reference clock (CLKB) is set only via the clock command (§2a.A). The
  Si5351's I2C address is reserved to the firmware. (Documented as a contract
  rule, not firmware-enforced: the firmware still reads Si5351 status for
  `GETSTATS`, so blanket-blocking the address would break legitimate reads.)
- **The CLKB clock command** itself — opcode/args, that `freq=0` disables,
  and that it is the *only* sanctioned way to touch the shared clock chip.

**Payoff:** a host driver author `#include`s the firmware's contract header,
gets the opcodes / addresses / path conventions / guarantees for free, and
supplies only the GPL register-tuning logic on their side — where GPL is
fine. Two consumers, one source of truth, no drift; and the firmware
guarantees nothing dangerous happens regardless of driver bugs.

## 2b. First consumer of the contract: collapse `tests/fx3_proto.h`

This is not hypothetical future tidiness — the drift the contract prevents
**already exists in-tree today**:

- `tests/fx3_cmd` does *not* include the firmware `protocol.h`. It includes
  `tests/fx3_proto.h`, a **67-line hand-maintained duplicate** of the
  opcodes, GPIO bits, and `ArgumentList`.
- The two have **already diverged**: `fx3_proto.h` still declares
  `TUNERINIT 0xB4 / TUNERTUNE 0xB5 / TUNERSTDBY 0xB8` — opcodes the firmware
  `protocol.h` dropped. A test exercising them would only get a STALL.
- The divergence is easy to make: this plan's own first draft mis-stated
  `TUNERINIT` as `0xB7` (the real historical value is `0xB4`). Two
  independent copies of a constant table drift; one shared header cannot.

**Task:** factor `protocol.h` into a portable **contract header** (opcodes,
GPIO map, `ArgumentList`, I2C wire convention, R828D facts) that *both* the
firmware build and `fx3_cmd` include, plus a thin **host-only** header for
the things that legitimately belong only to the test client (`RX888_VID`/
`PID`, `EP1_IN`, `CTRL_TIMEOUT_MS`). `protocol.h` is already pure portable
C — no Cypress SDK types — so it compiles host-side as-is.

**What this buys (scoped honestly):**

- *Reduced source complexity:* one definition of the protocol, not two.
  `fx3_proto.h` shrinks to host-only knobs.
- *Drift-resistance / test fidelity:* `fx3_cmd` asserts against the exact
  opcodes, GPIO bits, and I2C convention the firmware implements, so an
  opcode/GPIO change can't silently desync the suite. Making `fx3_cmd` a
  *consumer* of the contract is itself the strongest validation the contract
  is correct.
- *Not changed:* firmware **runtime** performance and the hardware-watchdog
  robustness are unaffected — header sharing is a build/maintenance-time
  property, not a runtime one. The robustness gained is correctness-drift
  resistance, not faster/sturdier firmware execution.

This consolidation can land **independently of any VHF work** (it only
touches the protocol header and the test include), making it a clean,
low-risk first commit that also de-risks every later step in §3.

## 2c. Two tuning models — both supported by the same firmware primitives

There are two ways to set the VHF LO, and the plan must support **both**.
The key realization: the firmware exposes *mechanism*, not *policy* — the
same small primitive set serves either model, and the host driver chooses.

### Model 1 — tune with the tuner chip (conventional, **recommended default**)

The R828D's **internal PLL** is the tuning element, programmed over I2C
(`r82xx_set_freq64`, host-side). The Si5351 CLKB is a **static reference**,
set once when VHF is entered and left alone. This is how the original
firmware worked, it uses the R828D as designed (tracking filters and image
rejection coordinated with the LO), and it is the recommended default.

- *LO* = f(reference, R828D PLL registers). The host knows the LO from the
  reference it set plus the R828D divider it programmed; precision is limited
  by the R828D's own fractional PLL.
- *Firmware use of CLKB:* set once at VHF entry, `freq=0` to leave.

### Model 2 — tune with the clock chip (Phil's preference, "deviant but fun")

Fix the R828D at a constant integer PLL ratio (one-time I2C setup), then
**tune by reprogramming the Si5351 CLKB per channel**. The Si5351 becomes
the active tuning element; the R828D is a fixed multiplier/mixer.

- *LO* = CLKB_achieved × (fixed R828D ratio). The appeal, in Phil's words:
  *"then I can be sure of knowing the actual frequency"* — Si5351 fractional
  synthesis is deterministic and host-computable, so the LO is known exactly.
- *Firmware use of CLKB:* reprogrammed on **every retune** — which is why the
  Si5351-ownership and PLLB-only-reset discipline (§2a.A) is load-bearing
  here, not incidental.
- *Status:* **experimental.** Not how the firmware worked before; maintainer
  will **test it on the ka9q-radio docker harness** before relying on it.
  Falsifiable on real hardware, not argued on paper.

**Why pure Model 2 is "using the tuner backwards" (real costs, not just
operational friction):**

- **It desyncs the R828D's tracking filter and image rejection.** Those are
  set from the frequency *written to the chip's registers*. Fix the registers
  and move the LO via the reference, and the chip's RF bandpass / image-reject
  cal are tuned for the wrong frequency — you discard the selectivity the
  tuner exists to provide. Severity scales with sweep width.
- **It trades frequency accuracy for phase noise.** With the R828D at a fixed
  ratio N, `LO = reference × N`, so the Si5351's fractional-N phase noise and
  spurs are **multiplied up by ≈20·log₁₀(N) dB**. Accuracy improves; spectral
  purity likely gets *worse* — the wrong trade for weak-signal-near-strong.
- **It pushes the R828D reference path out of its design point** (loop
  dynamics, lock range) — already noted below.

**The sane middle — hybrid.** Coarse-tune the R828D normally (tracking
filters correct, reference near nominal), then trim the reference by a *small*
amount for exact frequency. Captures the accuracy win, avoids all three
costs. Likely where the experiment lands once it meets a bench. And per the
design goals (§0): the firmware neither blesses nor blocks any of this — it
just guarantees none of it can wedge or fail silently.

### What both models require from the firmware (identical)

| Requirement | Model 1 | Model 2 |
|---|---|---|
| CLKB command, **arbitrary** host-chosen freq | set once | set per-retune |
| CLKB command **returns the actually-achieved freq** | useful | **essential** (it's how the LO is known) |
| **PLLB-only** reset (`0x80`), never global/PLLA | matters | **critical** (hammered every retune) |
| R828D reachable over I2C passthrough | yes | yes (one-time ratio setup) |
| VHF antenna/bias path via `GPIOFX3` | yes | yes |

So the firmware does **not** pick a model. It must provide: a CLKB command
that (a) accepts an arbitrary frequency, (b) **returns the frequency it
actually synthesized**, and (c) resets **only PLLB** so the ADC clock (CLK0/
PLLA) is never disturbed — verified in `Si5351.c`: `SetFrequencyA` →
CLK0/PLLA/reset `0x20`, `SetFrequencyB` → CLK2/PLLB/reset `0x80`, already
partitioned. The host driver decides whether to call it once (Model 1) or
continuously (Model 2).

### Model 2 residuals (honest, none are stream-killers)

1. **Per-retune tuner-ref glitch.** Each `SetFrequencyB` soft-resets PLLB →
   CLK2 (tuner reference) blips while PLLB relocks; the tuner reacquires.
   Does **not** touch CLK0/ADC. For glitch-free small steps, the driver's own
   comment notes multisynth-only changes can skip the PLL reset — an
   available optimization if retune cadence demands it.
2. **I2C bus occupancy.** Every retune is a multi-register write sequence on
   the shared bus; it serializes against the R828D's own I2C and the GETSTATS
   Si5351 read. Latency/contention, not a correctness issue — the ADC clock
   is a free-running hardware output once set.
3. **R828D reference-range tolerance (host/tuner-side).** A swept reference
   must stay within what the R828D's reference path tolerates; a wide sweep
   may push its internal PLL out of spec. Phil owns this validation on the
   bench.

## 3. Implementation — firmware side (the "modicum")

Ordered smallest-risk-first. Each step is independently committable.

1. **R828D reachability probe (host-side, GPL-free).** Using only the
   existing `I2CWFX3`/`I2CRFX3` passthrough, read the R82xx ID register and
   assert it equals `0x69` (`R82XX_CHECK_VAL` from the old
   `tuner_r82xx.h`; R828D I2C addr `0x74`, R820T `0x34`). Add this to
   `fx3_cmd` / `tests/`. This proves the I2C path physically reaches the
   tuner on real hardware and becomes the ground-truth regression anchor.
   *Falsifier:* if the probe never returns `0x69`, the I2C bus/addressing
   to the tuner is wrong and **must** be fixed before any tuning work — no
   host driver can succeed otherwise.

2. **Confirm/expose VHF antenna-path switching.** `rx888r2.c` already drives
   `GPIO_BIAS_VHF (18)`, `GPIO_VHF_EN (35)`, `GPIO_BIAS_HF (19)` and these
   are reachable via the generic `GPIOFX3` command. Verify on hardware that
   toggling `VHF_EN`/`BIAS_VHF` actually routes the VHF front-end (vs HF
   direct-sampling). Document the exact GPIO word the host must send to
   enter VHF mode. *Falsifier:* if toggling these GPIOs doesn't change the
   signal path on the bench, the GPIO map disagrees with the mk2 schematic
   and must be corrected first.

3. **GETSTATS VHF-path visibility.** The GETSTATS GPIO snapshot already
   includes `BIAS_VHF`/`VHF_EN`; confirm those bits are reported and add a
   test asserting they reflect a host-commanded VHF path selection. No new
   firmware state machine — just observability for the host driver and CI.

4. **The one firmware command: set tuner reference clock (CLKB).** Wire a
   vendor command to the already-present `si5351aSetFrequencyB()`: `freq>0`
   sets CLKB to an **arbitrary** host-chosen frequency, `freq=0` disables it;
   default off at boot and in HF mode. **Return the actually-synthesized
   frequency** (Si5351 is fractional-N), so the host knows the true LO — this
   serves both tuning models (§2c) and is *essential* for Model 2. Confirm
   the existing **PLLB-only reset** (`0x80`) so per-retune use never disturbs
   CLK0/PLLA (the ADC clock). Restore it as the historical `TUNERINIT (0xB4)`
   (enable) / `TUNERSTDBY (0xB8)` (disable) pair so stock ka9q-radio stops
   STALLing on `0xB8` — then **delete
   `docker/ka9q-radio/patches/04-no-tuner-stdby.patch`** and re-validate the
   container. This is the only *new* firmware logic of substance: a clock
   setting (with the same `si5351_pll_locked()` poll the ADC clock already
   uses), **not** a tuning/calibration loop. Tests: (a) CLKB enable/disable
   visible via `GETSTATS`; (b) ADC stream unaffected across CLKB on/off *and*
   across rapid CLKB retuning (Model 2 stress); (c) returned frequency matches
   the achieved synthesis within fractional-N resolution.

5. **Publish & complete the driver contract header (contract B).** Promote
   `protocol.h` to the canonical published header; document the I2C
   passthrough wire format (`wValue`=devaddr, `wIndex`=regaddr,
   `wLength`=count), add the R828D fact constants, the `GPIOFX3` VHF-path bit
   recipe, the CLKB clock command, and the **Si5351-ownership rule** (host
   must not write the Si5351 directly). No behavior change — this is the
   deliverable host driver authors consume. License-clean (project-owned
   definitions + chip facts).

6. **Anti-silent-failure audit (goal G3).** Confirm every command reports
   success/failure unambiguously — STALL/error on failure, never silent
   success — and that unknown commands and unknown `SETARGFX3` indices STALL,
   so the host always learns what happened. The I2C path is already bounded on
   `main` (finite `I2C_BUS_TIMEOUT` from PR #156; absent device NAKs →
   immediate error; EP0 watchdog backstop), so the only requirement here is to
   **preserve** that — do not reintroduce `busTimeout = 0xFFFFFFFF`. Sanity
   check it still holds with the tuner as a new I2C consumer (the #156
   `i2c_fuzz <ops> <seed>` test: `resets=0`, I2C error counter climbing on
   malformed/stranded reads).

7. *(Optional, conscious decision)* **host-crash bias-tee fail-safe.** A
   one-line bias-off on `STOPFX3`/reset if leaving antenna DC across a host
   crash is judged worth guarding. Decoupled from streaming; drop it if the
   answer is "accept worst-case bad tuning." Separate commit.

## 4. Host side: division of labor, the offered header, and workflow

All R828D init / `set_freq` / gain / sideband / standby register sequences —
and the **choice of tuning model** (§2c) — live in the **host** driver over
the §3.1 I2C passthrough. The old 2,415-line `tuner_r82xx.c` (recoverable
from `0ffa512`) is the reference for *register meanings*. The natural home is
the GPL host stack — ka9q-radio's `rx888.c` and/or `rx888_tools` — where GPL
is already fine. This repo keeps the I2C path, the CLKB command, and the VHF
GPIO path solid and observable; it does **not** ship tuner register code.

### The offered header — host-side contract, not a firmware driver

The maintainer offered Phil *"a header to manage the tuner chip, the same
way you handle the Si clock chip."* That header **is** the §2a.B contract,
and one precision keeps the license win intact:

- ✅ **Header = host-side contract.** Phil's GPL driver manages the R828D
  *itself* over I2C passthrough, using the header's definitions (opcodes,
  R828D address/ID facts, I2C wire format, the CLKB command). Register logic
  stays in his GPL host code.
- ❌ **Header ≠ "firmware does the register writes"** the way `STARTADC` makes
  the firmware program the Si5351. The Si5351 analogy is *asymmetric*: the
  firmware owns the clock chip because it's shared with the ADC; it must
  **not** own the tuner chip the same way, or the GPL R82xx driver lands back
  in the firmware binary and re-creates the conflict §2 removed. The header
  gives Phil *primitives* (passthrough + CLKB + facts), not a resident driver.

### Division of labor & workflow

| Side | Owner | Deliverable |
|---|---|---|
| Firmware: CLKB command + tuner contract **header** | this repo | hand the header to Phil; implement §3.4 |
| Host driver + ka9q-radio VHF + both tuning models | **Phil** | patch set on his docker harness → **PR** |
| Si5351-as-tuning experiment (Model 2) | Phil tests | empirical, on his bench |

Phil will submit the host-side work as a **PR against this repo's
`docker/ka9q-radio` harness**; we **cherry-pick** as appropriate. So the two
sides proceed in parallel: we produce the header + CLKB command; Phil builds
the driver against it and proves VHF end-to-end in the container.

## 5. Validation & regression (per CLAUDE.md Change Documentation policy)

Because this touches the firmware command surface, every increment needs:

- **Validation:** on real RX888 mk2 hardware — (a) select VHF path via
  GPIO and confirm antenna routing; (b) probe R828D ID register == `0x69`
  over I2C; (c) tune to a known VHF carrier and confirm a power bump in
  ka9q-radio `powers` output. (HF-only CI cannot prove VHF; this requires
  bench hardware with a VHF signal source.)
- **Regression:** full `tests/validate.sh` must still pass — HF streaming,
  GETSTATS, watchdog/health, enumeration-race, fuzz. Restoring opcodes must
  not shift the behavior of existing commands. If Option A restores
  `TUNERSTDBY`, **delete `04-no-tuner-stdby.patch`** and re-run the
  ka9q-radio container test to confirm stock behavior is healthy again.
- **License check:** Option C keeps the firmware HF-only at the
  register-logic level, so `README.md` → Limitations stays accurate. When the
  CLKB clock command (§3.4) lands, update the Limitations wording to note the
  firmware now offers the VHF *reference clock* (not tuning) and refresh the
  command table.

## 6. Open questions for the maintainer (remaining)

The big fork is settled (Option C); the firmware deliverable is settled (the
CLKB clock command, §2a.A / §3.4). Remaining, answerable as we go:

1. **CLKB command shape:** restore the historical `TUNERINIT 0xB4` /
   `TUNERSTDBY 0xB8` pair (best ka9q-radio compatibility, lets us delete
   `04-no-tuner-stdby.patch`), or a single freq-carrying opcode where
   `freq=0` disables? Recommendation: the historical pair, for drop-in host
   compatibility. (How the *achieved frequency* is returned — EP0 read after
   the set, or a `GETSTATS` field — is a small sub-decision.)
2. **Achieved-frequency reporting mechanism:** piggy-back on `GETSTATS`
   (add a CLKB-actual field) or an IN data phase on the CLKB command itself?
   Either works; `GETSTATS` keeps the command write-only and consistent.
3. **VHF bench hardware** (mk2 + VHF signal source) availability — needed to
   demonstrate, not merely inspect, the path-switch, probe, and Model 2.

*Resolved:* the host R828D driver lives in ka9q-radio (Phil's PR, §4); the
firmware/host split and the offered header are settled (§4).

## 7. First step (queued, no license entanglement)

Land §3.1 — the **R828D reachability probe** over the existing I2C
passthrough (reads the R82xx ID register, asserts `0x69`). GPL-free, proves
the I2C path on real hardware, anchors a regression test, and is the
foundation for the host driver. This is the recommended first PR-sized unit
of work once this plan is approved.
