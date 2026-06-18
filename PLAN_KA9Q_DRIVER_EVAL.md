# Plan — Evaluate ka9q-radio's new `rx888.c` driver (Si5351-host-synthesis revision)

Branch: `Claude/ka9q-driver-eval` (off `main`).

## Goal

Phil KA9Q asked us to bench-test the new revision of `rx888.c` in
ka9q-radio `main`. Bring the docker test environment up to that revision
so it can be exercised against SDDC_FX3 firmware.

## Compatibility analysis (evidence-based, completed before this plan)

- **New `rx888.c` (main `21d51fac`)** moves Si5351 clock synthesis into the
  host driver. It adds `#include "si5351.h"` and calls a new pure-integer PLL
  solver (`si5351_solve`, `si5351_get_pll_pvals`, `si5351_get_ms_pvals`).
- The pinned tree (`42273761`) has **no `si5351.{c,h}`** (HTTP 404), so the new
  `rx888.c` is **not** a standalone file-swap onto the pinned commit.
- `si5351.c` is **self-contained** (libc + its own header only) and **compiles
  clean** standalone (`gcc -std=c11 -Wall -Wextra`, 0 warnings — verified).
- The `SI5351_*` register macros are **already** in the pinned `rx888.h`
  (identical 26 defs); `rx888.h`'s only delta is `DEFAULT_IMAGE_FILE`, which the
  new `rx888.c` does **not** reference.
- Exported plugin interface is a **superset** (adds `rx888_shutdown`) →
  ABI-compatible.
- **Decision (user):** bump the whole `KA9Q_RADIO_SHA` to main rather than graft
  files, so the driver is tested in exactly Phil's tree.

## Patch-set impact

| Patch | Status at `21d51fac` | Action |
|-------|----------------------|--------|
| `01-powers-freq-double` | **Upstreamed** — main already `encode_double`s `RADIO_FREQUENCY` (line 160) | Retire → `.disabled` |
| `02-powers-rbw-float`   | **Upstreamed** — main already `decode_float`s `RESOLUTION_BW` (line 357)   | Retire → `.disabled` |
| `03-startadc-before-startfx3` | already disabled/historical | unchanged |
| `04-no-tuner-stdby`     | **Applies clean** to main (two `TUNERSTDBY` sends still present, identical context) | Keep |

## §10 regression risk (the reason a prior bump was reverted)

`docs/ka9q-compat-audit.md` §10: bumping to `6a5094ac` was reverted because
`radiod` segfaulted in `command_send` during `rx888_setup` (invalid libusb
handle). At main (`21d51fac`, a month later) this is **structurally addressed**:

- `rx888_usb_init()` sets `sdr->dev_handle` and **NULL-checks** it (line 834)
  before returning; setup bails on failure before any control transfer.
- The init-time USB reset now **defaults OFF** (`Reset=false`; `libusb_reset_device`
  only fires `if(Reset)`), so the handle isn't invalidated by an init reset and
  the firmware is not knocked to DFU.
- First control transfer in setup (Si5351 programming) runs only *after* a
  successful `rx888_usb_init`.

This is a positive structural signal but **not proof**; the bench `docker build`
+ run is the decisive test. If `radiod` segfaults at `rx888_setup` like §10, the
bump is blocked again and we revert.

## Config impact

- main **replaced** `hack_no_usb_reset` with a `reset` key (default **false**).
- `hack_no_usb_reset = yes` is no longer a recognized key at main (would trigger
  an unknown-key warning). Remove it; the new default already does what it asked.
  Add `reset = no` explicitly to document intent + the firmware/DFU rationale.

## ka9q-web (decision: bump to HEAD)

ka9q-web links `multicast.o status.o misc.o decode_status.o rtp.o` from
ka9q-radio. Header deltas pinned→main: `multicast.h` unchanged, `status.h`
+1 appended enum (`LIFETIME`), `misc.h` +10 (minor).

**Decision (user):** pair newest-with-newest — bump ka9q-web `b63c991d` →
`91cbfca25eef7b60923eebec120595dbbba5053d` (current HEAD) so it tracks the same
era as main ka9q-radio. This is an independent moving part; the `docker build`
(which compiles + links ka9q-web against the main ka9q-radio objects) is the
decisive check. If ka9q-web fails to build, fallbacks are: pin to a
ka9q-web commit known to pair with main, or make the ka9q-web build step
non-fatal (it is not needed for the `powers`-based smoke test).

## Files to change

1. `docker/ka9q-radio/Dockerfile` — `KA9Q_RADIO_SHA` → `21d51facd8c1f0e1a70a8e12c03307218f88ae53`; ka9q-web pin → `91cbfca25eef7b60923eebec120595dbbba5053d`; refresh both pin comments (Si5351 host synthesis, §10 status, reset default-off, newest-with-newest pairing).
2. `docker/ka9q-radio/patches/01-powers-freq-double.patch` → `.disabled`.
3. `docker/ka9q-radio/patches/02-powers-rbw-float.patch` → `.disabled`.
4. `docker/ka9q-radio/patches/README.md` — move 01/02 to disabled/historical (upstreamed), note 04 still active & applies clean, update SHA reference.
5. `docker/ka9q-radio/rx888-test.conf` — drop `hack_no_usb_reset = yes`; add `reset = no` with updated comment.
6. `docs/ka9q-compat-audit.md` — new section recording the bump to `21d51fac`: Si5351 host-side, §10 structurally addressed, powers fixes upstreamed, reset key; update §10 status line.
7. `docker/ka9q-radio/README.md` — refresh "Known compatibility notes" (01/02 retired, only 04 active).
8. `CHANGELOG.md` — entry for the driver-eval bump.
9. `tests/hf_sweep.sh` — update the comment that says it "requires the patched `powers`" (the fixes are now upstream at `21d51fac`); no functional change.
10. **NEW** `docs/ka9q-health-inspection.md` — the deeper-than-the-harness health/diagnostics runbook requested (outline below). Link it from `docs/index.md`.

## New doc: `docs/ka9q-health-inspection.md` (outline)

Purpose: how to *directly* inspect ka9q-radio and every subsystem the harness
depends on — going beyond `ka9q_smoke.sh`/`ka9q_test.sh`, which infer health
from "process alive + log markers + spectrum shape". Each subsystem section
gives: what it is, the single fastest "is it alive?" check, deeper probes, and
the failure signatures. All commands are run *inside* the container via a
persistent shell (`./docker/ka9q-radio/ka9q.sh console`), per CLAUDE.md bench
policy — not wrapped in per-action host-side `docker exec`.

Cross-references existing docs rather than duplicating: `docs/docker.md`
(container/multicast/avahi gotchas), `docs/diagnostics_side_channel.md`
(firmware GETSTATS telemetry), `docs/wedge_detection.md`, `docs/gpif-and-recovery.md`.

Sections (the "several systems we need to run our harnessing"):

1. **At-a-glance health board** — a one-screen sequence: `pgrep -a radiod`,
   `lsusb -d 04b4:`, `avahi-browse -atr` (or `-d`), `control hf.local` reachable,
   `powers` returns a tile. A table mapping each green/red to its section.
2. **USB / FX3 firmware** — `lsusb -d 04b4:` PID decode (`00f3` DFU vs `00f1`
   loaded), `lsusb -v` for SuperSpeed (5 Gb/s) negotiation, `dmesg | grep -i
   usb` for re-enumeration/reset storms, `/sys/bus/usb/.../speed`. Direct
   firmware counters via `fx3_cmd stats` / `test` — **but only when radiod is
   NOT running** (claim conflict; see `ka9q_test.sh` design note). Failure sigs:
   stuck at `00f3`, repeated reset→DFU, High-Speed (480 Mb/s) fallback.
3. **radiod core** — run it in the foreground (`radiod -v
   /etc/radio/radiod@rx888-test.conf`) to read the bring-up sequence live;
   the exact log markers that mean "driver loaded", "Si5351 programmed",
   "rx888 running"; how to attach `gdb -p $(pgrep radiod)` and read the
   backtrace (the §10/§11 wedge signatures: `command_send`/`rx888_setup` vs
   stalled before stream). `radiod -v`/`-vv` verbosity.
4. **rx888.so plugin + GPIF streaming** — confirm the plugin loaded
   (`Dynamically loading rx888 ...`), the data plane is live (DMA count
   advancing via `fx3_cmd stats` post-stop, or `powers` floor textured), and
   the **new** Si5351 host-synthesis log lines (`RX888 Si5351 PLL: vco = ...`,
   `output divider: samprate = ...`) print sane numbers for 64.8 Msps / 27 MHz.
5. **Si5351 clock (now host-programmed)** — read back CLK0_CONTROL (reg 16) and
   PLL/MS registers over I2C with `fx3_cmd i2cr 0xC0 ...` (device idle), compare
   to what `si5351_solve` logged; how to spot an unlocked/misprogrammed PLL
   (no fs/2 alias spike, floor flat). Ties to `docs/diagnostics_side_channel.md`
   §4 (PLL lock) and the firmware's CLK0-readback preflight.
6. **dbus + avahi (mDNS)** — `pgrep avahi-daemon`/`dbus-daemon`,
   `avahi-resolve -n hf.local`, `avahi-browse -rt _ka9q-ctl._udp` (or the actual
   service type), the two-resolver-path trap (`docs/docker.md` §4) and the
   "don't bounce avahi" rule (§3). Failure sig: `Temporary failure in name
   resolution`.
7. **Multicast / RTP data plane** — `control hf.local` (curses status), `tune`,
   and `tcpdump -ni lo 'udp and multicast'` to watch status + RTP packets;
   interface-pinning trap on bridge networking (`docs/docker.md` §2);
   `ip maddr show` to confirm group joins; `monitor` for audio. Distinguish
   "readiness ≠ data-plane" (`docs/docker.md` §8).
8. **ka9q-web + libonion** — `pgrep -a ka9q-web`, `ss -ltnp | grep 8081`,
   `curl -sI http://localhost:8081/`, `ldd $(which ka9q-web)` for libonion link.
9. **FFTW wisdom** — `ls -l /var/lib/ka9q-radio/wisdom`, `FFTW_RIGOR` effect on
   start latency; how a missing/!-CPU-matched wisdom shows up as a slow first
   start, not a failure.
10. **Putting it together / decision tree** — given a harness FAIL, which
    subsystem check to run first; the §10 segfault and §11 restart-stall
    playbooks specifically (this is the driver-eval's main watch-item).

## Validation (bench)

- `docker build -f docker/ka9q-radio/Dockerfile -t ka9q-radio .` must succeed (compiles new
  `rx888.c` + `si5351.o` into `rx888.so`; builds ka9q-web).
- Run container; `radiod` must reach `rx888 running` without the §10 segfault
  and without dropping to DFU.
- `tests/ka9q_smoke.sh` PASS (live textured thermal spectrum).

## Regression

- Patch 04 still removes both `TUNERSTDBY` sends (no EP0 STALL on HF path).
- `powers` tunes to `-f` and reports correct RBW (now via upstream code, not our
  patches) — `tests/hf_sweep.sh` / `ka9q_smoke.sh`.
- No new unknown-key warnings from `config_validate_section`.
