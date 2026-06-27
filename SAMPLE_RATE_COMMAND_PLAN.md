# Sample-Rate / Clock-Set Command — Implementation Plan

**Issue:** [#133](https://github.com/ringof/rx888-firmware/issues/133)
**Branch:** `claude/133_startadcfp` (PR #134)
**Status:** Draft for approval. No firmware changed yet. Per `CLAUDE.md`, this
plan must be approved before implementation, and each commit ships with the
standard change-doc block.

---

## 1. Goal

Replace the Si5351 multisynth register-selection math with a best-rational
(continued-fraction) approach so that:

1. The existing `STARTADC` command (0xB2) **quietly synthesizes more
   accurately** — no wire-format change, existing hosts benefit for free.
2. A **new typed clock-set command** accepts a fractional frequency, enabling
   sub-Hz / ppb adjustment of the Si5351 output. This is meaningful for users
   running a precision reference (TCXO / GPSDO) where the reference error no
   longer dominates and the synthesis grid becomes the binding limit.

The analysis backing these choices lives in issue #133 and the host-side
harness on this branch (`tests/si5351_math_test.c`, `tests/si5351_mc.c`,
`tests/si5351_bench.c`).

## 2. What we learned from the harness (drives the design)

- **Accuracy:** the best-rational algorithm reaches the per-divider hardware
  optimum for generic frequencies; the current integer algorithm is off by up
  to ~2.9 Hz at the high rates.
- **Exactness matters in one corner:** a floating-point continued fraction
  with an `epsilon` early-out leaves up to ~0.3 ppm on near-integer feedback
  ratios where an exact fraction exists. An **exact integer/rational
  continued fraction with semiconvergents** closes that to zero. → Use exact
  integer arithmetic, no floating point.
- **The output divider is *not* incidental** *(correction in flight)*. The
  oracle in the harness currently exhausts feedback denominators **only at
  the divider `select_ms` returns** — `select_ms` picks the largest even
  `output_ms` ≤ 900 MHz/`f`. For some frequencies that fixed choice is
  fractional, while a different legal even divider (still within VCO
  600–900 MHz) is exactly representable. Example: 34,441,768 Hz → `select_ms`
  picks divider 26 (irreducible fractional denom 1,687,500, exceeds the
  2²⁰−1 chip limit), but divider 18 yields feedback fractional 180,221/187,500
  which fits exactly. **Implication for this plan:** the algorithm should try
  a small set of legal even output dividers and prefer one that yields an
  exact (or smaller-denominator) feedback ratio. Single-divider exact is no
  longer the recommendation. The oracle is being corrected (PR follow-up); the
  shape of the recommendation is independent of the rerun result.
- **Cost is a non-issue:** a single-divider exact solve is ~tens of µs on the
  FX3; even an exhaustive divider search is ~tens of ms — well under the ~2 s
  EP0 handler budget and smaller than the ~100 ms PLL-lock poll already
  performed by `STARTADC` (`USBHandler.c:240-246`). A small multi-divider
  scan (a handful of even `output_ms` values keeping VCO in 600–900 MHz)
  costs sub-millisecond. The only heavy step in a full search (a linear
  D-scan) is removable by computing `D = floor(VCO_target / (fout · R))`
  directly.
- **Spectral purity** is governed by the output-stage topology, already
  AN619-correct (integer, even output MultiSynth; `MS0_INT` set). The
  best-rational change operates only inside the PLL feedback term and is
  spectrally neutral.

## 3. Algorithm (exact, integer-only, small multi-divider search)

All arithmetic in `uint64_t` (no FPU dependency).

Given target output `f` (as an exact rational `P/Q`, see §5) and reference
`fref` (27 MHz, `SI5351_FREQ` in `SDDC_FX3/driver/Si5351.c:32`):

1. Apply the R-divider: double `f` until ≥ 1 MHz (`rdiv` 0..7), as today.
2. Build the candidate set of **legal even output dividers**: all even
   `output_ms` in `[ceil(600e6/rf), floor(900e6/rf)]` ∩ `[4, 900]`. (For high
   `f` this is typically 1–3 values; for low `f`, more.)
3. For each candidate `output_ms`:
   - Compute the feedback ratio = (`rf · output_ms`) / `fref` as an exact
     rational. Split into integer part `A` and fraction.
   - Approximate the fraction by `B/C` with `C ≤ 1,048,575` using continued
     fractions **plus the semiconvergent step** (the validated
     `solve_feedback_exact` / `best_rational_exact` in `tests/si5351_mc.c`).
   - Score this candidate: primary by exact-error (zero when `C` divides the
     reduced denominator), secondary preference for **integer-PLL** (`B == 0`)
     and **smaller `C`** (cleaner fractional-N spurs).
4. Choose the lowest-scored candidate and pack `A + B/C` into PLL P1/P2/P3
   (`SetupPLL`) and `output_ms` into the output MultiSynth P1/P2/P3
   (`SetupMultisynth`), exactly as today.

Operand bounds keep the single-evaluation path in 64-bit
(VCO ≤ 900e6, `fref` = 27e6). Candidate error, where needed for ranking, is
computed in `double` only for comparison — never in the register math.

## 4. AN619 spectral invariants (acceptance criteria, not incidental)

These must hold and be regression-checked (see §8):

- **Output MultiSynth stays integer and even** (`P2=0, P3=1` in
  `SetupMultisynth`) — fractionality lives only in the PLL feedback. *(Already
  true; preserve.)*
- **`MS0_INT` integer-mode bit set** — `CLK0_CONTROL = 0x4F` already has bit 6
  set. *(Preserve.)*
- **VCO kept in 600–900 MHz** — the multi-divider candidate set enforces
  this; pick the candidate closest to 900 MHz on ties (best phase noise per
  AN619).
- **Preserve the Fig.10 init sequence** introduced in #163 (commit
  `1cd7de8`) — `Si5351Init` follows the datasheet power-up flow (output
  disable → drivers down → SSC off → both-PLL reset → enable). Our changes
  touch `si5351aSetFrequencyA` (and possibly `si5351aSetFrequencyB`); the
  init path is unchanged.
- **`DIVBY4` handling** for `output_ms == 4`: unreachable at our ≤135 MHz
  ceiling (so the candidate set has `output_ms ≥ 6`) but add the correct
  register form for range headroom.
- **Optional enhancement — integer-PLL mode when `B == 0`:** set the PLL
  integer-mode bit for the lowest spurs on exactly-integer feedback ratios.
  Exact register/bit to be confirmed against AN619 before implementing; gated
  as a later commit (§7, commit 4).

## 5. Wire protocol

### 5.1 `STARTADC` (0xB2) — unchanged on the wire
4-byte little-endian integer Hz, exactly as documented in
`docs/api.md`. Internally it converts to the rational form and calls the new
core, so it gains the accuracy improvement transparently.

### 5.2 New command — fractional clock set
A new typed configuration setter. **Open decisions in §9**, with recommended
defaults below:

| Field | Recommended |
|---|---|
| Name | `SETCLOCK` (alt: `SET_SAMPRATE`) |
| Opcode | `0xD0` — from the reserved typed-setter range `0xD0–0xDF` (`docs/vendor-protocol-plan.md`) |
| `bmRequestType` | `0x40` (OUT, Vendor, Device) |
| `wValue`/`wIndex` | 0 (ignored) |
| `wLength` | 8 |
| Data | **8-byte little-endian `uint64` frequency in milliHz** |

**Why milliHz integer rather than IEEE-754 double:** it keeps the firmware
integer-only (no soft-float pulled into the image), gives exact rational math,
and still resolves far below ppb (1 mHz ≈ 0.016 ppb at 64 MHz). `STARTADC`'s
integer Hz maps to milliHz by ×1000. *(Decision flagged in §9; a `double` or
explicit `num/den` encoding remain alternatives.)*

EP0 payloads are capped at 64 bytes (`CYFX_SDRAPP_MAX_EP0LEN`,
`USBHandler.c:47`); 8 bytes is well within range, and `wLength > 64` already
STALLs (`USBHandler.c:187-193`).

## 6. Firmware changes (files)

- **`SDDC_FX3/driver/Si5351.c`**
  - Add the exact best-rational core + the multi-divider scorer.
  - Refactor `si5351aSetFrequencyA(UINT32)` (line ~189 in pre-#163 layout —
    re-verify post-merge) to convert its argument to the rational form and
    call the core (transparent improvement).
  - Add a `double`-free entry point taking the rational target for the new
    command. Decide whether to refactor `si5351aSetFrequencyB` (CLK2) onto the
    same core now (§9).
  - Leave the new Fig.10 `Si5351Init` from #163 untouched.
- **`SDDC_FX3/protocol.h`** — add `SETCLOCK = 0xD0` to `enum FX3Command`; bump
  `FX3_CMD_COUNT` / extend the name tables consistently.
- **`SDDC_FX3/USBHandler.c`** — add a `case SETCLOCK` in the EP0 vendor
  dispatch, modeled on the `STARTADC` handler: read the 8-byte payload,
  validate length, call the core, poll PLL lock, STALL on
  out-of-range/failure leaving the chip untouched.
- **`SDDC_FX3/DebugConsole.c`** — add the trace name for the new opcode.

## 7. Phasing (independent commits)

1. **Core solver + `STARTADC` improvement.** Exact best-rational core with
   small multi-divider scoring in `Si5351.c`; `si5351aSetFrequencyA` delegates
   to it. No wire change. Ships with extended host unit tests (§8).
   Independently mergeable and valuable on its own.
2. **New `SETCLOCK` command.** Opcode, handler, dispatch, debug name, host
   tooling, `docs/api.md`.
3. **Integration tests.** `tests/fx3_cmd.c` + `tests/fw_test.sh` random-rate
   sweeps and streaming sample-count checks.
4. **Optional spectral/coverage enhancements.** Integer-PLL (`FBA_INT`) when
   `B == 0`, `DIVBY4` form, and CLK2 (`si5351aSetFrequencyB`) refactor — only
   if chosen in §9.

## 8. Test plan

**Validation (host, no hardware):**
- Extend `tests/si5351_math_test.c` to assert the multi-divider exact solver
  matches the true cross-divider hardware optimum across a frequency sweep
  **including** (a) the near-integer-feedback band (the corner the float
  version missed) and (b) frequencies like 34,441,768 Hz where a non-default
  divider is exactly representable. Emitted registers must satisfy the AN619
  invariants in §4 (integer/even output MS, `MS0_INT` set, VCO in 600–900 MHz).
  Run via `make -C tests check-math`.
- The Monte-Carlo harness (`make -C tests mc-plot`) and benchmark
  (`make -C tests bench`) remain characterization tools; the oracle in the
  Monte-Carlo program needs the across-divider fix (Codex review on PR #134)
  before any post-implementation claims.

**Validation (hardware, host-driven):**
- `tests/fx3_cmd.c`: add a sender for the new command and a test that programs
  a set of **random frequencies** (integer via `STARTADC`, fractional via
  `SETCLOCK`) and confirms PLL lock via `GETSTATS` byte [19] — demonstrating
  the solver "always comes up with a solution."
- `tests/fw_test.sh`: extend the ADC-setting tests and the streaming test to
  set several rates, stream for X seconds, and assert the captured sample
  count matches the requested rate within tolerance.

**Regression:**
- Existing `STARTADC` tests (`do_test_freq_hop`, `do_test_clock_pull`,
  `do_test_pib_overflow` in `tests/fx3_cmd.c`) must pass unchanged — the wire
  contract is identical; only accuracy improves.
- Si5351 init / chip-recovery soak from #163 must remain green (the multi-
  divider scorer only changes what `SetupPLL`/`SetupMultisynth` are called
  with, not whether they are called, and does not touch `Si5351Init`).
- Recoverability (per `docs/vendor-protocol-plan.md` Principle 2): malformed
  `SETCLOCK` payloads (wrong length, out-of-range value) STALL cleanly with no
  state change; the EP0 handler returns within bounded time; unknown opcodes
  still STALL.

## 9. Open decisions (need sign-off before coding)

1. **Command name:** `SETCLOCK` vs `SET_SAMPRATE`.
2. **Opcode:** `0xD0` (reserved typed-setter range) vs a one-off byte.
3. **Wire encoding:** `uint64` milliHz (recommended, integer-only) vs IEEE-754
   `double` vs explicit `num/den`.
4. **Out-of-range / unsynthesizable input:** STALL leaving the chip untouched
   (recommended) vs clamp-to-nearest.
5. **CLK2 (`si5351aSetFrequencyB`):** refactor onto the new core now, or leave
   for later.
6. **Multi-divider scoring weight:** primary error, then prefer integer-PLL
   then smallest `C` (recommended) — or another ordering?
7. **Integer-PLL (`FBA_INT`) enhancement:** include in commit 4 or defer.

## 10. Out of scope

- An exhaustive multi-target VCO sweep — the small multi-divider search above
  is sufficient given the analysis; revisit only if hardware measurements
  show otherwise.
- `GETCAPABILITIES` advertising the new command — that command is unwritten
  (commit 3 of `docs/vendor-protocol-plan.md`); when it lands, add `SETCLOCK`
  to its `commands` list so hosts can feature-detect it.
- VHF / R82xx paths.
