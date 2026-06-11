# Plan: VHF Tuner Support for the RX888 mk2 — host-side, no firmware changes

> Status: **DIRECTION SET — Option C, and the firmware needs nothing.** The
> R828D tuner is driven entirely **host-side** over the firmware's existing
> passthrough. The pivotal fact: the host **already owns the Si5351** — Phil's
> ka9q-radio `rx888.c` does host-side clock synthesis, programming the Si5351
> directly over `I2CWFX3` (PR #147; "programs Si5351 directly via raw I2CWFX3
> and never sends STARTADC," PR #91). So the tuner reference clock (CLKB) is
> just more host-side Si5351 writes, the R828D is host I2C, and the HF/VHF
> switch is `GPIOFX3`. **No new firmware command, no `TUNER*` opcodes.** The
> deliverables are host-side: a driver (Phil, in ka9q-radio / `rx888_tools`),
> the docs that make it writable, and bench validation.
>
> Live artifacts (supersede the firmware-command framing earlier drafts had):
> `vhf_host_bringup.md` (1-page host bring-up), `tuner_r82xx_explained.md`
> (chip walk-through), `vhf_tune.py` (minimal EP0 config utility).

## 0. Design goals (the lens)

The firmware aims to be **easy to write a driver against, without getting in
the way of advanced users — while making failure loud and impossible to
wedge.** Three goals:

- **G1 — Easy to write a driver.** One published contract header (§2a.B),
  sane defaults, and a documented happy-path bring-up (`vhf_host_bringup.md`).
- **G2 — Don't get in the way of advanced users.** Ship **mechanism, not
  policy**: raw I2C passthrough (R828D *and* Si5351), raw GPIO, raw stream
  control. The firmware never blesses a tuning model or rejects an
  unconventional sequence.
- **G3 — Prevent silent failure and wedging.** Every command reports
  success/failure (STALL/error, never silent success); operations are bounded
  (the finite `I2C_BUS_TIMEOUT` from PR #156, the EP0 watchdog, bounds-checked
  EP0 buffers). A bad driver gets bad *results*, never a device that needs a
  physical replug.

These already hold on `main`. VHF adds **no** firmware surface, so the only G3
obligation is **don't regress** the finite I2C timeout (the tuner is a new I2C
consumer, addressable while absent/unpowered).

## 1. Background: what VHF support was, and where it went

The RX888 mk2 has two receive paths:

- **HF (0–32 MHz):** direct sampling through the on-board LTC2208 ADC — the
  only path the current firmware operates.
- **VHF/UHF:** an **R828D** tuner downconverts to an IF the ADC then samples.
  Driving it needs R82xx I2C control, a reference clock, and GPIO path
  switching.

### History (verified from git)

The current `main` history was **re-rooted** — `git rev-list --max-parents=0
HEAD` shows multiple root commits; the tree starts fresh at `2056a30` as an
RX888 mk2-only firmware. The original full SDDC_FX3 firmware (every board
variant + the tuner driver) is still reachable via `--all` on the older root
chain (`ff82ddd` … `0ffa512`).

In the last full-featured commit (`0ffa512`), VHF support consisted of:

| Piece | Location | Notes |
|---|---|---|
| R82xx tuner driver | `SDDC_FX3/driver/tuner_r82xx.c` (**2,415 lines**) + `.h` | init, `set_freq64`, gains, `set_sideband`, `set_bandwidth`, filter cal |
| Vendor commands | `USBhandler.c` | `TUNERINIT (0xB4)`, `TUNERTUNE (0xB5)`, `TUNERSTDBY (0xB8)` |
| Argument sub-commands | `SETARGFX3` | `R82XX_ATTENUATOR`, `R82XX_VGA`, `R82XX_SIDEBAND`, `R82XX_HARMONIC` |
| Radio dispatch | `radio/rx888r2.c` | `r820_initialize()`, antenna/bias GPIO, `switch(HWconfig)` |

### Why it was removed (the load-bearing constraint)

From `README.md` → **Limitations**: *"The GPL-licensed R82xx driver has been
removed to resolve a license conflict with the proprietary Cypress SDK."*

`tuner_r82xx.c` is **GPL-2.0** (librtlsdr / Osmocom lineage); the Cypress FX3
SDK it links against is proprietary and not GPL-compatible. A combined binary
statically linking the two is the conflict the maintainer chose not to ship.
This single constraint forces the tuner host-side and is *why* the plan below
adds no tuner code to the firmware.

### What still survives in the tree (scaffolding)

- `protocol.h`: GPIO masks `BIAS_HF`, `BIAS_VHF`, `VHF_EN` still defined;
  `HWconfig` reduced to `{ NORADIO, RX888r2 }`.
- `radio/rx888r2.c`: still drives `GPIO_BIAS_VHF (18)` / `GPIO_VHF_EN (35)` and
  reports them in the GETSTATS GPIO snapshot. Antenna-switch plumbing intact.
- The command enum has a gap at `0xB4`/`0xB5`/`0xB8` where the `TUNER*` opcodes
  lived. **Drift already exists:** `tests/fx3_proto.h` still declares them even
  though the firmware dropped them — see §2b.
- `docker/ka9q-radio/patches/04-no-tuner-stdby.patch` exists only because the
  firmware no longer answers `0xB8`. (The merged ka9q work already *deleted*
  the host send, so this is effectively resolved upstream.)

## 2. Decision: Option C — host-side driver, **zero** firmware additions

**Chosen.** The R828D register/tuning logic — and the reference clock — live
**on the host**, over the firmware's existing `I2CWFX3`/`I2CRFX3`/`GPIOFX3`.

Why this is the right call:

- **No license conflict, by construction.** The GPL tuner logic never enters
  the firmware binary. The host driver can be GPL (ka9q-radio / `rx888_tools`).
  This is exactly where `README.md` → Limitations already points.
- **Nothing to add.** Every primitive VHF needs already exists and is already
  used by the host (see §2a). The firmware change set is **empty**.
- **Complexity lives where it's cheap to change.** Firmware is expensive/risky
  to reflash; host driver iteration is free. Tuner sequences, gain, cal all
  belong host-side.

### Rejected alternatives (for the record)

- **Option A — firmware-resident clean-room/permissive driver.** More effort,
  needs a license decision; not chosen.
- **Option B — re-vendor the GPL `tuner_r82xx.c` verbatim.** Re-creates the
  exact license conflict. **Rejected.**

## 2a. Where the work lives

### 0. What the original fork's tuner commands did, and where each lands now

From `Interface.h` + `USBhandler.c` at `0ffa512`. The decisive column is
whether the action needs the Si5351 at all:

| Command | Opcode | Did | Si5351? | Now |
|---|---|---|---|---|
| `TUNERINIT` | `0xB4` | cfg + CLKB on + `r82xx_init` + filter cal | CLKB on | host I2C (incl. CLKB) |
| `TUNERTUNE` | `0xB5` | `r82xx_set_freq64` (LO) | no | host I2C |
| `TUNERSTDBY` | `0xB8` | `r82xx_standby` + CLKB off | CLKB off | host I2C (incl. CLKB) |
| `R82XX_ATTENUATOR` | arg 1 | LNA+mixer gain | no | host I2C |
| `R82XX_VGA` | arg 2 | IF VGA gain | no | host I2C |
| `R82XX_SIDEBAND` | arg 3 | injection side | no | host I2C |
| `R82XX_HARMONIC` | arg 4 | never implemented | — | — |

Of seven control points, five are pure R828D I2C; the other two are the
CLKB reference clock on/off. **All of it is reachable from the host** — the
R828D over `I2CWFX3`/`I2CRFX3`, the CLKB over the same Si5351 writes the host
already does for the ADC clock.

### A. Why the firmware needs nothing: the host already owns the Si5351

Earlier drafts argued the firmware must own the Si5351 — because CLKB shares
the chip and PLL resources with CLKA (the ADC sample clock), and a careless
write could glitch CLKA and **kill the stream**. The hazard is real. But the
resolution is **not** a firmware command, because:

**The host is already the sole Si5351 programmer.** Phil's `rx888.c` does
host-side clock synthesis — it computes the multisynth and writes the Si5351
directly over `I2CWFX3` for CLKA/the ADC clock (PR #147; PR #91: "programs
Si5351 directly via raw I2CWFX3 and never sends STARTADC"). So:

- CLKB (tuner reference) is **the same kind of write the host already makes** —
  a different output (CLK2) on a chip it already drives. No new firmware path.
- Adding a firmware CLKB command would **split** Si5351 ownership between two
  writers (firmware + host) on one chip — *introducing* the CLKA-clobber
  coordination hazard, not removing it. Sole host ownership is the safer state,
  and it already exists.

So the CLKA-protection discipline (program coherently; the `SetFrequencyB`
analogue resets **PLLB only** (`0x80`), never PLLA/CLK0) is now a **host**
responsibility, documented in the contract (§2a.B) — not firmware code.

What the firmware keeps (already present, unchanged): the I2C/GPIO/stream
passthrough, `GETSTATS`, the health watchdog, and the finite I2C timeout (G3).
The VHF change set is: **nothing.**

### B. The published driver contract (the actual firmware-repo deliverable)

`protocol.h` is already the project-owned, license-clean contract (the
internalized former `Interface.h`). Promote it to the canonical published
header and complete it with what a host R828D author has to reverse-engineer:

- **I2C passthrough wire format** (from `i2cmodule.c`): `I2CWFX3`/`I2CRFX3` use
  **`wValue` = device addr (8-bit)**, **`wIndex` = register**, **`wLength` =
  count**, data in the EP0 buffer (reads set `devAddr | 0x01` internally).
- **R828D facts:** addr `0x74`, ID reg `0x00` == `0x69` (after the R82xx read
  bit-reverse), reference fed from Si5351 CLK2 (not a fixed crystal).
- **`GPIOFX3` VHF-path recipe:** 32-bit LE word in the data phase
  (`wLength=4`); set `VHF_EN` (bit 15, `0x8000`); whole-word write, so start
  from the HF word. (No AGC pins on this board — gain/AGC is R828D-internal
  over I2C.)
- **The Si5351 coherence rule (host responsibility):** the host owns the whole
  Si5351; when it programs CLK2/CLKB it must reset **PLLB only** so CLK0 (the
  ADC clock) is never glitched. The firmware still reads Si5351 status for
  `GETSTATS`, so the host must not assume exclusive bus access mid-stream.

**Payoff:** a host driver author consumes one header for the opcodes /
addresses / wire format, and supplies the GPL register logic on their side.
Two consumers, one source of truth, no drift (§2b).

## 2b. First consumer of the contract: collapse `tests/fx3_proto.h`

The drift the contract prevents **already exists in-tree**:

- `tests/fx3_cmd` does *not* include `protocol.h`; it includes
  `tests/fx3_proto.h`, a **67-line hand-maintained duplicate**.
- They've **diverged**: `fx3_proto.h` still declares `TUNERINIT 0xB4 /
  TUNERTUNE 0xB5 / TUNERSTDBY 0xB8` — opcodes the firmware dropped.
- The divergence is easy to make: this plan's first draft itself mis-stated
  `TUNERINIT` as `0xB7` (real value `0xB4`).

**Task:** factor `protocol.h` into a portable **contract header** (opcodes,
GPIO map, `ArgumentList`, I2C wire convention, R828D facts) that *both* the
firmware build and `fx3_cmd` include, plus a thin host-only header for
test-client knobs (`RX888_VID`/`PID`, `EP1_IN`, `CTRL_TIMEOUT_MS`).
`protocol.h` is already pure portable C — no Cypress types — so it compiles
host-side as-is. This lands **independently of any VHF work** and is the
recommended low-risk first commit.

## 2c. Two tuning models — both host-side policy

There are two ways to set the VHF LO. Both are **host driver decisions**; the
firmware sees neither (it only carries the I2C writes).

### Model 1 — tune with the tuner chip (conventional, recommended default)

The R828D's internal PLL is the tuning element (`r82xx_set_freq64`); the
Si5351 CLKB is a **static reference**, set once at VHF entry. Uses the R828D as
designed — tracking filters and image rejection coordinated with the LO.

### Model 2 — tune with the clock chip (Phil's preference, experimental)

Fix the R828D at a constant integer PLL ratio, then **tune by reprogramming
the Si5351 CLKB per channel**. `LO = CLKB_achieved × ratio`; the appeal is
exactness ("be sure of knowing the actual frequency") since Si5351 synthesis
is host-computable. Not how the firmware worked before; to be **tested on the
ka9q-radio harness** before relying on it.

Real costs of pure Model 2 (why it's "the tuner backwards"):

- **Desyncs the R828D tracking filter / image rejection** — those follow the
  registers, not the reference; moving the LO via the reference leaves the RF
  bandpass on the wrong band.
- **Trades accuracy for phase noise** — at fixed ratio N, the Si5351's
  fractional-N phase noise/spurs multiply up by ≈20·log₁₀(N) dB.
- **Pushes the R828D reference path off its design point** (loop dynamics,
  lock range) — a host/tuner-side bench question.

**The sane middle — hybrid:** coarse-tune the R828D normally, then trim the
reference slightly for exact frequency. Captures the accuracy win, avoids the
costs. Per G2, the firmware neither blesses nor blocks any of these — and per
G3, none can wedge or fail silently.

## 3. Work items

None of these is firmware tuner code; they are host-side + the contract.
Ordered smallest-risk-first.

1. **Publish the contract header + collapse `fx3_proto.h` (§2b).** Firmware
   repo, independent of VHF, low risk. Promote `protocol.h`, add the wire
   format / R828D facts / GPIO recipe, make `fx3_cmd` consume it.
2. **R828D reachability probe (host, GPL-free).** Over `I2CWFX3`/`I2CRFX3`,
   read R828D reg `0x00` and check `0x69` (with the R82xx read bit-reverse).
   The ground-truth "I2C reaches the tuner" check; anchor a regression test.
   Already stubbed in `vhf_tune.py`. *Falsifier:* if it never reads `0x69`,
   the bus/addressing is wrong and nothing downstream can work.
3. **Confirm VHF antenna-path switching on the bench.** `VHF_EN`/`BIAS_VHF`
   via `GPIOFX3` (see `vhf_host_bringup.md` step 1). Verify it actually routes
   the VHF front-end. *Falsifier:* if toggling these doesn't change the path,
   the GPIO map disagrees with the schematic.
4. **Host VHF driver + demo.** In `rx888_tools`/`librx888` (or ka9q-radio):
   port a clean upstream R82xx driver (steve-m/librtlsdr — *not* the
   FX3-mangled firmware copy), swap its I/O for the I2C passthrough, set
   `cfg->xtal` = the CLK2/CLKB you program. `vhf_tune.py` is the minimal
   reference — an EP0 config utility that can run **concurrently** with the
   streamer (EP0 vs EP1; the `two_actor_open` pattern, #143).
5. **Keep the finite I2C timeout (G3).** No new firmware, but verify VHF's new
   I2C consumer doesn't tempt anyone to revert `I2C_BUS_TIMEOUT`. Sanity:
   `i2c_fuzz <ops> <seed>` → `resets=0`.

*Optional / convenience only:* thin `TUNERINIT`/`TUNERSTDBY` opcodes that
bundle a default tuner+reference setup or park the reference. **Not needed**
(the host does all of it), and they'd split Si5351 ownership (§2a.A), so they
are a deliberate ergonomic choice, not a requirement.

## 4. Host side: division of labor & workflow

The R828D register logic, the reference-clock (CLKB) writes, the gain/
sideband, and the **choice of tuning model** (§2c) all live in the **host**
driver. The old `tuner_r82xx.c` (`0ffa512`) and `tuner_r82xx_explained.md` are
**reference for chip behavior**, not a porting base — use clean upstream
librtlsdr for the code.

### The "header to manage the tuner chip" Phil was offered

That header **is** the §2a.B contract — and one precision keeps the license
win: it's a **host-side contract** (Phil's GPL driver manages the R828D itself
over passthrough, using the header's definitions), **not** a firmware that does
the register writes. The firmware must *not* own the tuner chip the way it once
owned the clock — that would drag the GPL driver back into the binary.

| Side | Owner | Deliverable |
|---|---|---|
| Firmware repo | this repo | the contract header + docs (`vhf_host_bringup.md`, explainer); **no code** |
| Host driver + ka9q-radio VHF + both tuning models | **Phil** | patch set on the `docker/ka9q-radio` harness → **PR**, we cherry-pick |
| Si5351-as-tuning experiment (Model 2) | Phil tests | empirical, on the bench |

## 5. Validation

- **On real RX888 mk2 hardware:** (a) select VHF via `GPIOFX3`, confirm antenna
  routing; (b) probe R828D ID == `0x69`; (c) program CLK2/CLKB + tune the
  R828D, stream the IF, and confirm a known VHF carrier appears at the **IF
  offset (≈4.57 MHz with the 8 MHz filter)**. HF-only CI cannot prove VHF —
  this needs a bench + VHF source.
- **Regression:** full `tests/validate.sh` must still pass (HF streaming,
  GETSTATS, watchdog/health, enumeration-race, fuzz). Since the firmware is
  unchanged, regression risk is confined to the contract-header refactor (§2b)
  — `fx3_cmd` consuming `protocol.h` must produce identical behavior.
- **Docs/license:** `README.md` → Limitations stays accurate (firmware remains
  HF-only at the register level). Optionally update it to note VHF is now
  supported *host-side* with no firmware change.

## 6. Open questions

1. **Ship the optional convenience `TUNERINIT`/`TUNERSTDBY` opcodes?** Default
   **no** — the host needs nothing and they'd split Si5351 ownership. Only
   reconsider for stock-host ergonomics, and the merged ka9q work already
   dropped the `0xB8` send, so the compatibility pressure is gone.
2. **VHF bench hardware** (mk2 + VHF signal source) availability — needed to
   demonstrate (not merely inspect) the path-switch, probe, and Model 2.

*Resolved:* the host driver lives in ka9q-radio / `rx888_tools` (Phil); the
firmware/host split and the offered header are settled (§4); the firmware
adds no code.

## 7. First step

Land §3.1 — **publish the contract header and collapse `fx3_proto.h`**. It's
the only firmware-repo code change, it's independent of hardware, it kills a
live drift, and it gives Phil the one header his host driver consumes. Then the
host work (probe → path → driver) proceeds on the bench against it.
