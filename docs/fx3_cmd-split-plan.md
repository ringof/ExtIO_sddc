# Plan: Package `fx3_cmd` for diagnostics & split it from the firmware test harness

Status: **DRAFT — awaiting approval.** No code changes until this plan is approved.

## 1. Goal

Phil wants `fx3_cmd` packaged as a quick **diagnostics** tool. Today it is a
single ~5,830-line monolith (`tests/fx3_cmd.c`) that mixes two unrelated jobs:

1. **Operator diagnostics** — probe, poke, and recover a device on the bench.
2. **Firmware regression / fuzz / soak harness** — ~50 issue-numbered
   scenarios driven by CI and the `*.sh` orchestration scripts.

A clean diagnostics package requires separating (1) from (2), not shipping the
whole monolith.

## 2. Findings that drive the design (verified, not assumed)

- `fx3_cmd.c` has **no compile-time dependency on the firmware tree**. It
  includes only libusb + its own sibling modules (`fx3_proto/usb/stats/bulk/
  fuzz/lifecycle`). The only firmware coupling is a *default path string*
  (`../SDDC_FX3/SDDC_FX3.img`, overridable with `-F`).
- The destination repo **`rx888-tools`** (`github.com/ringof/rx888_tools`,
  already wired here as the `tests/rx888_tools` submodule) is explicitly the
  Linux host-tools home: `librx888` + `rx888_stream` + `rx888_dsp` +
  `iqrecord`, with CI, `udev/99-rx888.rules`, an `install`/`uninstall`
  Makefile, and a `firmware/` bump workflow.
- `rx888-tools/include/rx888.h` **already declares `enum FX3Command`**
  (`STARTFX3=0xAA … RESETFX3=0xB1`), `enum ArgumentList`, `enum GPIOPin`,
  `enum SI5351Registers`. So `tests/fx3_proto.h` is a **third** copy of a
  protocol that exists in both the firmware (`SDDC_FX3/protocol.h`, the source
  of truth) and the destination repo. Consolidation removes a drift hazard.
- The firmware test scripts (`soak_test.sh`, `fw_test.sh`, `ka9q_test.sh`,
  `validate.sh`, `hf_sweep.sh`) invoke the **diagnostic subcommands as
  primitives** (`load`, `test`, `start`, `stop`, `adc`, `gpio`, `reload`).
  The harness therefore cannot simply *lose* those commands — it needs the
  same core the diagnostics CLI uses.
- Firmware upload (`-F`, `load`, `reload`) is implemented via `rx888_stream`
  from the submodule. Pure-EP0 diagnostics (`test`, `stats`, `gpio`, `i2c*`,
  `reset`, `raw`) need only libusb.

## 3. Command classification (proposed)

**Diagnostics CLI (moves to rx888-tools, also reused by the harness):**
`load`, `reload`, `test`, `gpio`, `adc`, `att`, `vga`, `wdg_max`, `start`,
`stop`, `i2cr`, `i2cw`, `reset`, `usbreset`, `debug`, `raw`, `stats`.

**Firmware harness (stays in rx888-firmware):** everything else — all
`*_fuzz`, `soak`, `watchdog_*`, `*_recovery`, `ep0_*`, `stats_i2c/pib/pll/shdn`,
and the ~40 issue-numbered scenario commands.

**Borderline (decide during review):** `stats_pll`, `stats_shdn`,
`stack_check` — these read real diagnostic state but are written as PASS/FAIL
verification tests. Default: leave with the harness; optionally expose a
read-only `stats --verbose` in the diagnostics CLI instead.

## 4. Target architecture (end-state)

Single source of host-side FX3 protocol + transport, owned by rx888-tools:

```
rx888-tools/
  include/rx888.h            ← canonical FX3Command/GPIO/SI5351 enums (exists)
  src/fx3_core.{c,h}         ← NEW: USB open/claim/reset, EP0 senders,
                                GETSTATS decode, bulk read (from fx3_usb.c,
                                fx3_stats.c, fx3_bulk.c)
  src/fx3_cmd.c              ← NEW: diagnostics CLI (the §3 "diagnostics" set)
  Makefile                   ← add fx3_cmd as a 4th binary; install + man page

rx888-firmware/  (builds against the submodule's fx3_core)
  tests/fx3_fuzz.c, fx3_lifecycle.c, harness scenarios in fx3_cmd_test.c
  tests/Makefile             ← link harness against rx888_tools/src/fx3_core
```

`tests/fx3_proto.h` is deleted; both sides include `rx888-tools/include/rx888.h`.
The firmware already pins rx888-tools as a submodule, so the harness gets the
core at a known revision and the protocol stays in lock-step by construction.

## 5. Phased execution

**Phase 0 — Decisions & prerequisites (no code):**
- Confirm rx888-tools is the home (✔ evidenced above).
- Reconcile the three protocol definitions: diff `SDDC_FX3/protocol.h` (truth)
  vs `rx888-tools/include/rx888.h` vs `tests/fx3_proto.h`; resolve any value or
  naming mismatches before anything depends on the merge.
- Decide transport: reuse the extracted `fx3_core` (recommended) vs link
  `librx888` (larger rewrite; defer).
- **Operational:** rx888-tools work needs that repo added to the session scope
  (current GitHub scope is `ringof/rx888-firmware` only). Confirm before Phase 2.

**Phase 1 — Refactor in rx888-firmware (behavior-preserving, CI stays green):**
- Split `tests/fx3_cmd.c` into:
  - `fx3_core.{c,h}` — shared transport + primitive command senders.
  - `fx3_cmd_diag.c` — the §3 diagnostics command handlers + `main`/dispatch.
  - `fx3_cmd_test.c` — the harness scenario handlers.
- No moved files yet; just internal modularization. Run the full `make` +
  `check-health` + a smoke pass to prove no behavior change.

**Phase 2 — Land diagnostics in rx888-tools:**
- Add `fx3_core` + `fx3_cmd` (diagnostics) sources; build as a 4th binary.
- Replace `fx3_proto.h` constants with `include/rx888.h`.
- Wire Makefile (`install`/`uninstall`/`clean`), CI (`ci.yml`), README section,
  and a `fx3_cmd.1` man page / trimmed `--help`. Add to `udev` doc if relevant.
- Decide firmware-upload story: keep `reload`/`-F` delegating to `rx888_stream`
  (same repo now — direct dependency, no submodule needed there).

**Phase 3 — Re-point rx888-firmware harness at the shared core:**
- Build `fx3_cmd_test` (harness) against `tests/rx888_tools/src/fx3_core`.
- Delete the moved diagnostics source and `tests/fx3_proto.h`.
- Update `tests/Makefile`, and the scripts that call diagnostic subcommands.
  Key question: does the harness ship its own `fx3_cmd` (built from the shared
  core + scenarios) or call rx888-tools' installed binary for primitives? Plan
  assumes the former (self-contained harness binary, shared core).
- Bump the submodule pin to the rev that contains `fx3_core`.

**Phase 4 — Packaging polish:**
- Versioning for `fx3_cmd` (independent of firmware), man page, optional `.deb`.
- Document the protocol single-source rule so no fourth mirror reappears.

## 6. Risks / open questions

- **Cross-repo CI:** two repos must stay green; the submodule pin couples
  firmware-test builds to a rx888-tools revision. Plan sequences merges to keep
  both buildable at every step.
- **Protocol reconciliation** is a real subtask, not a rename — `rx888.h` uses
  its own enum style and may not match `protocol.h`/`fx3_proto.h` value-for-value
  everywhere. Must be diffed and unified in Phase 0.
- **Monolith split friction:** `fx3_cmd.c` interleaves helpers and dispatch; the
  Phase 1 cut needs care so shared statics become `fx3_core` API cleanly.
- **Scope of "diagnostics":** is the §3 set the right product surface for Phia's
  use, or does Phil want a smaller/larger set? (Drives §3 borderline calls.)
- **Effort:** Phase 1 (refactor) is the bulk of the work and is valuable to the
  firmware repo regardless of whether the move proceeds. A "quick win" fallback
  is to stop after Phase 1 and ship a diagnostics-only build *from the firmware
  repo* (slim Makefile target), deferring the cross-repo move.

## 7. Decision requested

1. Approve the end-state in §4 (rx888-tools owns a shared FX3 core + diagnostics
   CLI; firmware harness links it)?
2. Approve the §3 command split (incl. borderline default)?
3. Quick win (stop after Phase 1, ship from firmware repo) vs full move?
4. OK to add `ringof/rx888_tools` to the session scope for Phase 2+?
