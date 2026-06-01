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

Firmware version **2.5** (`FIRMWARE_VER_MAJOR=2`, `FIRMWARE_VER_MINOR=5`);
queryable via the `TESTFX3` vendor command.

### Changed

- **Firmware-reported version bumped to 2.5** (`FIRMWARE_VER_MINOR` in
  `SDDC_FX3/protocol.h`): 2.3 → 2.4 marked the 0.1.1 cleanup/bugfix pass,
  2.4 → 2.5 the cold-start detection + reset-escalation work (#137). Wire
  format unchanged (`TESTFX3` still returns `major.minor`).
- **Streaming watchdog moved into the health module.** The Level-1
  DMA-stall watchdog was migrated out of `RunApplication.c`'s main loop
  into `SDDC_FX3/health.c`, behind the `health_evaluate()` /
  `health_recover(HEALTH_WEDGED_STREAMING)` cascade interface; the
  recovery cap moved with it (`health_set_max_recovery()` /
  `health_reset_recovery_count()`). Behaviour-preserving; validated by a
  3-hour soak. (#115)
- **GPIF state checks now use named constants.** The streaming watchdog
  and the `STOPFX3` / `STARTADC` stop paths (`USBHandler.c`) previously
  compared the `CyU3PGpifGetSMState()` result against raw numeric literals
  (`5`, `7`, `8`, `9`, `1`, `0`). These now use named macros from a new
  `SDDC_FX3/gpif_states.h` that mirrors the state indices generated into
  `SDDC_GPIF.h`. No behavior change. (#116)

### Fixed

- **`INT32_MIN` undefined behaviour in the debug `%d` formatter.**
  `MyDebugSNPrint` negated a negative `int32_t` in place to get its
  magnitude — signed-overflow UB for `INT32_MIN`. It now computes the
  magnitude in unsigned space; output is byte-identical for all inputs.
  Debug-console path only. (#117)

### Added

- **Host enumeration-race test coverage + open backoff.** `open_rx888()`
  now absorbs a udev/libusb permission-settle race: it distinguishes a
  present-but-unopenable device (retries with bounded backoff, ~2 s, env
  `RX888_OPEN_RETRY_MS`) from an absent one (fail-fast as before), and
  retries `claim_interface` briefly on `BUSY`/`ACCESS`. Two new standalone
  tests model the firmware side of the "device mysteriously disappears"
  failure mode: `reopen_race_storm` (tight close/reopen churn — kernel
  URB-dequeue + XHCI ring restart + firmware `CLEAR_FEATURE`) and
  `two_actor_open` (a second process hammers EP0 while the first streams).
  Both assert the firmware never resets (`boot_count` steady) and keeps
  streaming. Host-side test tooling only. (#143)
- **Cold-start wedge detection + autonomous reset recovery.** The
  streaming watchdog now also detects a *cold-start* wedge — the GPIF SM
  left IDLE (a stream was commanded) but the first DMA buffer never
  arrives (`glDMACount` frozen at 0 for ~500 ms) — which the previous
  `curDMA > 0` guard could not see (#119). Light recovery is tried first;
  if it can't clear it (the cap exhausts), `health_recover` escalates to
  `CyU3PDeviceReset`, gated on a healthy ADC clock and limited to once per
  session. Post-stream backpressure stalls and abandoned streams are
  unaffected — they still cap-and-wait (escalation is cold-start-only). On
  by default; disable at runtime via the new `WDG_RESET_ESCALATE`
  SETARGFX3 arg (15). Adds a test-only `HANGCOLDSTART` (0xD0) command for
  deterministic bench validation. (#137, #119)
- **Seeded USB/vendor-command fuzzing for the host test suite.** New
  `tests/fx3_cmd protocol_fuzz` (seeded EP0 control-transfer fuzzer:
  `bmRequestType` direction/recipient, `bRequest`, `wValue`/`wIndex`,
  `wLength`, payload) and `stream_fuzz` (seeded bulk + host-lifecycle fuzzer:
  random read sizes/timeouts, cancellation, EP0 ops while reads pending,
  close/reopen, abandon). Both add a reproducible PRNG, an operation
  ring-buffer **failure log** (seed + last 32 ops + failure-time GETSTATS +
  visible PID) and a per-command **coverage report**. They run standalone
  (kept out of the unattended `soak` rotation: on hardware `protocol_fuzz`
  found that malformed/wrong-direction vendor EP0 requests can corrupt the
  device into needing a re-flash — issue #142). A first step on modularizing the
  host harness landed alongside: the shared USB/stats/bulk layers moved out
  of `fx3_cmd.c` into `fx3_proto.h` / `fx3_usb` / `fx3_stats` / `fx3_bulk`,
  and the fuzzer into `fx3_fuzz`, behind a multi-object build. Host-side test
  tooling only — no firmware change. (#139)
- **ADC low-power standby when idle.** The firmware now drives the ADC
  `SHDN` line from the streaming state: the ADC is parked in low-power
  standby at boot and on `STOPFX3`, and woken on `STARTFX3` (with a short
  settle before the GPIF state machine starts). Saves ≥330 mA and reduces
  heat whenever a stream is not running. Manual `GPIOFX3` SHDWN control is
  unchanged. (#131)

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
