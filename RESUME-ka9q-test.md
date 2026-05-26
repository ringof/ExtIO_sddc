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

## Open — ka9q_test.sh (the soak harness) is NOT green yet

Last bench run (`--duration 150 --reload-interval 60 --stream-secs 12`),
after the readiness-wait fix (commit c4e2f1d), still showed TWO problems:

1. **Cycle 1: radiod fully up (`rx888 running`, `hf.local` registered) but
   `powers` capture returns nothing** in the `ka9q-radio-soak` container.
   **New lead from the local rig:** the *only* things that made `powers`
   return empty locally were (a) radiod dead, or (b) **avahi dead / the
   `.local` name not resolvable** — in which case `powers` blocks until
   timeout with no data. radiod registering `hf.local` in its *log* is not
   the same as the name being resolvable via getaddrinfo at capture time
   (mDNS propagation lag, or avahi not up in the soak container). This is now
   the prime suspect for cycle 1.
2. **Cycles 2/3: radiod stalls in rx888 init on restart** — reaches "found
   rx888 / Si5351 programmed" but never `rx888 running` within 30s. Plain
   restarts (no reload). Restart-path problem. NOT explained.

## Next step (do this FIRST, before any code change)

Run the capture command with **stderr visible** (the harness hides it with
`2>/dev/null`) to see *why* cycle 1's `powers` returns empty — expect either
a name-resolution error or a silent timeout:

```bash
# in the soak/dbg container, while radiod is confirmed up:
avahi-resolve -n hf.local                      # does the name resolve at all?
docker exec ka9q-radio-soak sh -c 'timeout 10 powers -c 1 -i 2 -f 10000000 -b 256 -w 10000 -s 30303 hf.local'; echo "exit=$?"
```

- If `hf.local` doesn't resolve / `powers` errors on it → it's the mDNS path
  (suspect #1): fix by waiting on `avahi-resolve` success (not just the log
  line) before capture, or by giving radiod a literal multicast group.
- If it resolves and `powers` still times out empty → multicast/data-plane,
  dig further.

Then probe #2 by stop/restarting radiod in the same container and watching
whether the 2nd start ever reaches `rx888 running` (and whether a forced
reload between restarts clears it).

Reminder: get the stderr/resolve result first — don't theorize the fix from
the symptom.
