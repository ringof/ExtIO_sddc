# Resume — ka9q_test.sh bench validation

Branch `claude/shdn-when-stopped` (PR #132).

## Green / done

- **Firmware SHDN (#131)** — implemented + verified in code (rx888r2.c,
  USBHandler.c, radio.h, CHANGELOG, docs). Core objective met.
- **ka9q_smoke.sh** — PASSED on the bench: full 0..fs/2 sweep, mean
  -132.19 dB, spread 58.02 dB (fs/2 alias spike), PASS. The "firmware
  streams REAL samples, no receiver UI" proof works.
- **`powers` TLV fixes (freq->double, rbw->float)** — merged upstream on the
  fork (`ringof/ka9q-radio` PR #1) with a sig_gen-based test. Independently
  re-confirmed here (see below); the patched numbers matched the PR exactly.
- **hf_sweep.sh** — tile fixes done and verified flat edge-to-edge:
  - fixed-MAXBINS window slide + warm-up (earlier), then
  - **retune-ghost fix (commit 70fdcea):** per-tile `-c 2` keeps the settled
    second spectrum (kills a stale-data ghost a strong signal throws one
    tile-width away); warm-up removed as redundant; per-tile `timeout` added
    so a dead radiod fails the tile instead of hanging the sweep.
- Container reverted to ka9q-radio 42273761 + hack_no_usb_reset (the
  6a5094ac bump segfaults — audit §10, report upstream later).
- Docs (tests/README, docker README, PLAN-SHDN integration gate) updated.
- **validate.sh** — end-to-end wrapper (Stage 1 fw_test, 2 soak, 3A
  ka9q_smoke + fs/2-alias gate, 3B ka9q_test kill-and-return). 1/2/3A green.
- **Container networking: bridge, not `--network host`** — radiod/powers/
  ka9q-web all in-container so multicast stays on the container's `lo`
  (deterministic; a multi-homed host with host networking landed radiod and
  powers on different interfaces). ka9q-web published on `127.0.0.1:8081`.
- **ka9q_test.sh** hardening: cross-cycle avahi-collision wait + restart
  fallback; FFTW `estimate`; `KA9Q_SPEC_DEBUG` to surface powers stderr.
- **fx3_cmd `resetup_cycle`** — host-style full re-setup restart, in-handle +
  `RESETUP_REOPEN` fresh-handle, `RESETUP_STANDBY_MS` knob.

## Hardware-free ka9q + sig_gen rig (reproducible; no RX888 needed)

Built `radiod` + `sig_gen.so` + `powers` from ka9q `main` and ran spectra
against the synthetic `sig_gen` front end — no SDR, no docker. This is the
fastest way to debug `powers`/`hf_sweep` and reproduce artifacts. Recipe:

- `apt-get install` the ka9q build deps (see docker/ka9q-radio/Dockerfile)
  plus `avahi-daemon avahi-utils libnss-mdns dbus`.
- Clone ka9q, `make -C src radiod sig_gen.so powers` (ENABLE_SIG_GEN=1).
- Copy `sig_gen.so` to `/usr/local/lib/ka9q-radio/`, share/ to
  `/usr/local/share/ka9q-radio/`.
- Start `dbus-daemon --system` and `avahi-daemon` **detached with setsid**
  (they get reaped otherwise — this cost real time).
- radiod config: `hardware = sig_gen`, `status = sgtest.local`,
  `[sig_gen] carrier=10m0 noise=-50 samprate=30m0 real=y`.
- Launch radiod **detached with setsid** too. `powers ... sgtest.local`.
- To run `hf_sweep.sh` (which calls `docker exec`), use a shim:
  `docker(){ [ "$1" = exec ] && { shift 2; exec "$@"; }; }` on PATH.

What it proved today: the `powers` TLV fix is real and deterministic; the
original RX888 first/last-tile baseline shifts reproduce exactly when the fix
is unwound; and it surfaced the retune ghost the dummy-load sweep never could.

## Conclusion: the restart issues are ka9q-side; firmware is exonerated

Stages 1/2/3A are green. Stage 3B (ka9q_test kill-and-return soak) is red, but
**both failure modes are ka9q-radio's, not the RX888 firmware** (audit §11):

1. **cross-cycle "no spectrum"** — the persistent container's avahi carries
   `hf.local` across radiod restarts → `Local name collision` → powers
   resolves a stale entry. Mitigated in `stop_radiod` (wait for withdrawal +
   avahi-restart fallback); the root is radiod's withdrawal lag.
2. **restart stall** — radiod hangs in its own streaming-transfer setup
   *after* claiming the device. **Firmware exonerated:** `fx3_cmd
   resetup_cycle` reproduces a full host re-setup restart (in-handle AND
   `RESETUP_REOPEN` fresh-handle) at 0/40/200 ms standby and **passes every
   time** — vendor sequence, wake timing, and fresh open/claim/stream/release/
   close per cycle all clean. So it's radiod's per-restart USB/URB handling.

#131's real claim (clean ADC-parked idle between sessions) is proven without
ka9q-radio at all by `fw_test`/`soak` + the green `ka9q_smoke` gate.

## Interim workaround still in place (revisit)

`validate.sh` Stage 3A pre-loads firmware (`fx3_cmd reload`) before radiod
because radiod's self-upload is flaky (the ka9q `sleep(1)`-after-upload
re-enumeration race, audit §1). Not acceptable long-term; marked INTERIM
in-code. The firmware re-enumerates fine; it's ka9q's fixed wait.

## Next — into ka9q-radio itself

All firmware-side restart angles are eliminated, so debug ka9q-radio directly:
why `radiod` hangs in streaming-transfer setup on the 2nd/3rd restart in one
container, and the avahi withdrawal lag. Carry the `resetup_cycle` evidence
(firmware streams across the full restart surface) when raising it upstream.
Tools: the hardware-free `sig_gen` rig (above) and `fx3_cmd resetup_cycle`.
