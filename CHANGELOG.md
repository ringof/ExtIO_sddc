# Changelog

All notable changes to this firmware are documented here.

The format is loosely based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
uses [semantic-ish versioning](https://semver.org/) — see the
[releases page](https://github.com/ringof/rx888-firmware/releases) for
the canonical list of artifacts.

Firmware-reported version (`FIRMWARE_VER_MAJOR.FIRMWARE_VER_MINOR` in
`SDDC_FX3/protocol.h`, queryable via the `TESTFX3` vendor command) is
tracked separately from the repo release tag and is noted per-release
below where it changes.

## [Unreleased]

### Added

- **ADC low-power standby when idle.** The firmware now drives the ADC
  `SHDN` line from the streaming state: the ADC is parked in low-power
  standby at boot and on `STOPFX3`, and woken on `STARTFX3` (with a short
  settle before the GPIF state machine starts). Saves ≥330 mA and reduces
  heat whenever a stream is not running. Manual `GPIOFX3` SHDWN control is
  unchanged. (#131)

### Testing / tooling

- **ka9q-radio test container tracks the host-side-Si5351 `rx888.c` driver,
  now patch-free** (ka9q-radio `42273761` → `21d51fac` → `87567fa`; ka9q-web
  `b63c991` → `91cbfca`). The driver synthesizes the Si5351 clock on the host
  (new `si5351.c` PLL solver) and writes the registers over `I2CWFX3`; at
  `87567fa` it also polls the Si5351 for lock, drops ~10 microsleeps, and logs
  the rx888 firmware version via `TESTFX3`. **All local compatibility patches
  are now upstream** — `01`/`02` (powers float/double) and `04` (no-tuner-stdby,
  removed at `87567fa`) — so the container builds **vanilla ka9q-radio with zero
  patches**. `rx888-test.conf` uses the new `reset = no` (default-off) key in
  place of the retired `hack_no_usb_reset`. Validated on RX888mk2 hardware
  (hot/cold start, restart soak, live spectrum); see `docs/ka9q-compat-audit.md`
  §12. *(Test-environment change; not a firmware change.)*
- **ka9q test container cold-boot ergonomics.** The container now defaults
  `FFTW_RIGOR=estimate` (instant cold boot for compatibility checks; set
  `measure`/`patient` for long-term operation), and the firmware location is
  parametrized — bind-mount any directory containing `SDDC_FX3.img` onto
  `/firmware` (or `FIRMWARE_DIR=… ./ka9q.sh start`), since the built `.img`
  normally lives outside the repo.
- **ka9q test harness moved to `--network host`.** radiod's cold-start path
  (upload firmware → FX3 re-enumerates `00f3`→`00f1` → re-acquire) depends on a
  USB hotplug event, and hotplug is delivered over a network-namespace-scoped
  netlink socket a bridge container can't hear — so cold start failed under
  bridge despite the `/run/udev` mount, and works under host networking.
  `ka9q.sh`, `ka9q_test.sh`, and the run examples now use `--network host`
  (one model); multicast determinism is preserved by keeping everything on
  loopback (radiod defaults to `lo`, consumers pin `-I lo`). Mechanism written
  up in `docs/ka9q-compat-audit.md` §1, networking details in `docs/docker.md`
  §2.
- **New `docs/ka9q-health-inspection.md`** — a per-subsystem health/diagnostics
  runbook for inspecting ka9q-radio and the systems the harness depends on
  (USB/FX3, radiod, rx888.so/GPIF, Si5351, dbus/avahi, multicast/RTP, ka9q-web,
  FFTW), going deeper than the `ka9q_smoke.sh` / `ka9q_test.sh` pass/fail gates.

## [0.1.0] — 2026-05-14

First named release.  Firmware version **2.3** (`FIRMWARE_VER_MAJOR=2`,
`FIRMWARE_VER_MINOR=3`).

### Highlights

- **ka9q-radio runs without host-side patches.**  Firmware now reports
  Si5351 CLK0 state truthfully, so ka9q-radio's direct Si5351
  programming path passes the `STARTFX3` preflight without needing a
  redundant `STARTADC`.  The container's patch 03 has been retired.
- **5-level recovery cascade** for streaming wedges, EP0 stalls, and
  main-thread freezes — including FX3 hardware-watchdog fallback for
  the worst-case lockup.  Validated under multi-hour soak.
- **GitHub Pages developer site** at
  [ringof.github.io/rx888-firmware](https://ringof.github.io/rx888-firmware/)
  with a complete USB API reference verified against the C source
  line-by-line.

### Added

- Live Si5351 CLK0_CONTROL register read inside
  `si5351_clk0_enabled()`; removed the `glAdcClockEnabled`
  host-cache flag.  (`SDDC_FX3/driver/Si5351.c`, PR #122.)
- `GETSTATS` reply extended from 24 to 26 bytes — new offsets:
  [24] raw CLK0_CONTROL register byte, [25] boolean chip-query
  result.  Backwards compatible: hosts that still request `wLength=24`
  continue to work.  (`SDDC_FX3/USBHandler.c`.)
- `clk0_chip_query` soak scenario that exercises the live I2C
  preflight check.  (`tests/fx3_cmd.c`.)
- `soak --weight NAME=N` / `-w NAME=N` flag to bias scenario rotation
  for context-dependent failure hunts.  (`tests/fx3_cmd.c`.)
- GitHub Pages site: `docs/_config.yml`, `docs/index.md`, `docs/api.md`
  (USB vendor-command reference), `docs/building.md`,
  `docs/compatibility.md`, plus Jekyll front-matter on existing docs.
  Workflow `.github/workflows/pages.yml`.  (PR #121.)
- `CHANGELOG.md` (this file).

### Changed

- `docker/ka9q-radio/patches/03-startadc-before-startfx3.patch` renamed
  to `…patch.disabled`; kept in-tree for archaeology.  Dockerfile loop
  hardened with a POSIX empty-glob guard so a zero-active-patch build
  succeeds cleanly.  (PR #122.)
- `docs/ka9q-compat-audit.md` §8 reframed: original failure mode
  preserved for context, but marked resolved firmware-side.
- `docs/vendor-protocol-plan.md` commits 1 and 2 marked DONE; commits
  3 (`GETCAPABILITIES`, `GETSTATE`) and 4 (architecture docs) remain
  open.

### Fixed

- `STARTFX3` preflight no longer rejects ka9q-radio's direct Si5351
  programming path with `LIBUSB_ERROR_PIPE` / EP0 STALL → "No rx888
  data for 5 seconds, quitting".

### Known issues

- Cold-start `startadc_mid_stream` flake at the very first scenario
  after firmware boot (≈ once per multi-thousand-cycle soak).
  Tracked in [#119](https://github.com/ringof/rx888-firmware/issues/119).
- Intermittent failures in `setarg_gap_index`
  ([#111](https://github.com/ringof/rx888-firmware/issues/111)) and
  `test_health_recovery`
  ([#113](https://github.com/ringof/rx888-firmware/issues/113)).
- Broader doc audit pending
  ([#114](https://github.com/ringof/rx888-firmware/issues/114)).
