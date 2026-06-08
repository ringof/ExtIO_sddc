# Plan: Package `fx3_cmd` for diagnostics & split it from the firmware test harness

Status: **Phases 0–2 complete; Phase 3 verified and ready to implement on
branch `claude/fx3-cmd-to-tool`.** See "Progress" below for the as-built state.

## 1. Goal

Phil wants `fx3_cmd` packaged as a quick **diagnostics** tool. Today it is a
single ~5,800-line monolith (`tests/fx3_cmd.c`) that mixes two unrelated jobs:

1. **Operator diagnostics** — probe, poke, and recover a device on the bench.
2. **Firmware regression / fuzz / soak harness** — ~50 issue-numbered
   scenarios driven by CI and the `*.sh` orchestration scripts.

A clean diagnostics package requires separating (1) from (2), not shipping the
whole monolith. The end-state is a **single shared FX3 host core** owned by
`rx888-tools`, consumed by the firmware harness via the existing submodule, so
the two repos stop carrying parallel copies.

## 2. Findings that drive the design (verified, not assumed)

- `fx3_cmd.c` has **no compile-time dependency on the firmware tree**. It
  includes only libusb + its own sibling modules. The only firmware coupling is
  a *default path string* (`../SDDC_FX3/SDDC_FX3.img`, overridable with `-F`).
- The destination repo **`rx888-tools`** (`github.com/ringof/rx888_tools`,
  already wired here as the `tests/rx888_tools` submodule) is the Linux
  host-tools home: `librx888` + `rx888_stream` + `rx888_dsp` + `iqrecord`.
- `SDDC_FX3/protocol.h` is the **single source of truth** for the wire protocol.
  `tests/fx3_proto.h` mirrors it for the host tools; `rx888-tools/include/
  rx888.h` is the tools-side copy. Consolidation onto `rx888.h` removes a drift
  hazard.
- The firmware test scripts invoke the **diagnostic subcommands as primitives**
  (`load`, `test`, `start`, `stop`, `adc`, `gpio`, `reload`), so the harness
  cannot lose those commands — it needs the same core the diagnostics CLI uses.

## 3. Progress (as built — updated 2026-06-08)

**Phase 0 — reconciliation: DONE.** rx888-tools `main` (post-PR #26, rev
`fd01e67`) ships a reconciled `include/rx888.h`. Verified against the firmware
source of truth — all previously-divergent symbols now agree:

| Symbol            | firmware (`protocol.h`/`fx3_proto.h`) | rx888-tools `rx888.h` (post-#26) |
|-------------------|---------------------------------------|----------------------------------|
| VGA SETARG id 11  | `AD8370_VGA`                          | `AD8370_VGA` ✓ (was `AD8340`)    |
| `LED_BLUE`        | bit 11                                | bit 11 ✓ (was bit 12)            |
| `GETSTATS`        | `0xB3`                                | `0xB3` ✓ (was absent)            |
| `WDG_MAX_RECOV`   | SETARG 14                             | `14` ✓ (was absent)              |
| `RX888_VID/PID_*` | present                               | present ✓                        |
| `HANG*` test codes| present (test-only)                   | **absent** ✓ (correctly omitted) |

**Phase 1 — extract shared core: DONE (in rx888-firmware).** `tests/fx3_core.
{c,h}` carry the 19 diagnostics primitives; the harness, debug console, and
`main`/dispatch stay in `fx3_cmd.c`.

**Phase 2 — land diagnostics in rx888-tools: DONE (PR #26, merged).**
`src/fx3_cmd/{fx3_cmd.c, fx3_core.{c,h}, fx3_usb.{c,h}, fx3_stats.{c,h}}` +
reconciled `include/rx888.h`, with its own `tests/{fx3_cmd_smoke,hw_fx3_cmd}.sh`.

**Convergence backport — DONE (this branch).** `--no-claim` read-only mode +
26-byte GETSTATS tolerance backported into the firmware core so it stays
identical to the tools core ahead of the submodule cutover.

**Key verification result:** the two cores are now **byte-identical except the
`#include` line.** Diff of firmware `tests/{fx3_core,fx3_usb,fx3_stats}.{c,h}`
vs rx888-tools `src/fx3_cmd/*` shows the *only* differences are
`#include "fx3_proto.h"` ↔ `#include "rx888.h"`, the location of
`CTRL_TIMEOUT_MS` (header vs proto), and one header comment. `fx3_stats.h` is
identical. **There is zero logic drift** — Phase 3 is a build-seam change, not a
code merge.

## 4. Target architecture (end-state)

Single source of host-side FX3 protocol + transport, owned by rx888-tools:

```
rx888-tools/
  include/rx888.h                 ← canonical protocol enums + USB IDs (DONE)
  src/fx3_cmd/fx3_core.{c,h}       ← USB open/claim/reset, EP0 senders, primitives
  src/fx3_cmd/fx3_usb.{c,h}        ← open/claim, --no-claim, CTRL_TIMEOUT_MS
  src/fx3_cmd/fx3_stats.{c,h}      ← GETSTATS decode (26/30-byte tolerant)
  src/fx3_cmd/fx3_cmd.c            ← diagnostics CLI

rx888-firmware/  (harness links the submodule's shared core)
  tests/fx3_cmd.c                  ← harness main + ~50 scenarios (KEEPS)
  tests/fx3_bulk.c                 ← bulk EP1 reads (KEEPS — no tools equivalent)
  tests/fx3_fuzz.c, fx3_lifecycle.c ← harness-only (KEEP)
  tests/fx3_test_proto.h           ← NEW: harness-only wire bits not in rx888.h
  tests/Makefile                   ← compile core from tests/rx888_tools/src/fx3_cmd
```

**Seam (settled by evidence):**
- **Shared, from the submodule:** `fx3_core`, `fx3_usb`, `fx3_stats`.
- **Firmware-only:** `fx3_bulk` (rx888-tools has no equivalent), `fx3_fuzz`,
  `fx3_lifecycle`, `fx3_cmd.c` (harness main + scenarios).
- **`tests/fx3_proto.h` is deleted.** Its production symbols come from the
  submodule's `rx888.h` + `fx3_usb.h`. Its harness-only symbols —
  `HANGFX3`/`HANGMAIN`/`HANGCOLDSTART` (test-fault injectors), `EP1_IN`,
  `FX3_MAX_EP0LEN` — move to a new firmware-local `tests/fx3_test_proto.h`,
  included only by `fx3_bulk/fuzz/cmd`. They are deliberately kept out of the
  public `rx888.h`.

## 5. Phase 3 — re-point the harness at the shared core (this branch)

1. **Bump the submodule pin** `tests/rx888_tools` `6191883 → fd01e67` (the PR #26
   merge that adds `src/fx3_cmd/`). CI already checks out `submodules: recursive`.
2. **Add `tests/fx3_test_proto.h`** carrying the harness-only bits:
   `HANGFX3`/`HANGMAIN`/`HANGCOLDSTART`, `EP1_IN`, `FX3_MAX_EP0LEN`.
3. **Re-point firmware-local module includes:** `fx3_bulk.c`, `fx3_fuzz.c`,
   `fx3_lifecycle.c`, `fx3_cmd.c` swap `"fx3_proto.h"` → `"rx888.h"` +
   `"fx3_test_proto.h"` (the latter only where the harness-only symbols are used).
4. **Rewire `tests/Makefile`:** compile
   `tests/rx888_tools/src/fx3_cmd/{fx3_core,fx3_usb,fx3_stats}.c` with
   `-Itests/rx888_tools/src/fx3_cmd -Itests/rx888_tools/include`; **delete** the
   local `fx3_core.{c,h}`, `fx3_usb.{c,h}`, `fx3_stats.{c,h}`, and `fx3_proto.h`.
5. **Validate:** `make -C tests` + `check-health` + `check-cli`, then the
   hardware harness (`fw_test.sh`/`validate.sh`); confirm `fx3_cmd` usage and
   dispatch are byte-identical pre/post.

**Risk: low.** No logic moves; the core is proven identical and the protocol is
reconciled. The only realistic failure is a missing production symbol after
deleting `fx3_proto.h`, which the compiler flags immediately in step 5.

## 6. Phase 4 — packaging polish & anti-drift

- Versioning for `fx3_cmd` (independent of firmware), man page / trimmed `--help`.
- **Single-source rule (enforced going forward):** core edits land in
  rx888-tools first, then bump the firmware submodule pin. The firmware repo
  never re-forks `fx3_core/usb/stats` or re-creates a `fx3_proto.h`. Document
  this so no fourth protocol mirror reappears.

## 7. Risks / open questions

- **Cross-repo session scope:** `add_repo`/`list_repos` are unavailable in this
  session, so rx888-tools is read-only here (via the submodule). Phase 3 is
  entirely firmware-side, so this is not a blocker; any future rx888-tools edit
  (e.g. moving `EP1_IN` into a shared header) must go through that repo separately.
- **Submodule pin couples builds:** the firmware-test build now tracks a
  rx888-tools revision. Acceptable and intended; the pin is explicit and bumped
  deliberately.
- **`fx3_bulk` stays forked:** rx888-tools streams via `librx888`/`rx888_stream`,
  so it has no `fx3_bulk`. The harness keeps its own; this is by design, not drift.
