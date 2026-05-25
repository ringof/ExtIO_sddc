# Plan: Spectre Docker Test Environment for RX888mk2 Firmware

A second test harness alongside `docker/ka9q-radio/`, validating
`SDDC_FX3.img` against a different host stack: SoapySDR-SDDC →
GNU Radio (via [Spectre](https://github.com/spectregrams/spectre)).
A green run transitively certifies the firmware on the
SoapySDR-SDDC seam, so any GNU Radio host using `driver=SDDC` is
covered — Spectre is the proxy test, not the only target.

## Decisions

- **Image strategy: A — upstream images unchanged.** Use
  `spectregrams/spectre-server:3.5.0-alpha` and `spectre-cli:3.5.0-alpha`
  via compose; no custom Dockerfile. Recon confirmed SoapySDR-SDDC is
  built into the image from `spectregrams/ExtIO_sddc v0.0.1`, and an
  `rx888mk2` receiver (mode `fixed_center_frequency`) is a first-class
  citizen in `backend/src/spectre_server/core/receivers/`.
- **Firmware injection: layered.** A one-shot `rx888-uploader`
  sidecar runs `rx888_stream -f /firmware/SDDC_FX3.img` before
  `spectre-server` starts (compose `service_completed_successfully`),
  AND the same `.img` is bind-mounted into `spectre-server` at
  libsddc's resolved firmware path. Either layer alone has failure
  modes the other covers (warm boot, mid-session re-upload, udev
  races, upstream path drift); combined, the harness provably tests
  *our* firmware regardless of libsddc internals.
- **Validation signal: phased proof bar.** Phase 1 (no bench
  equipment, runs anywhere) — a 50 Ω load or antenna; the spectrogram
  must show a live, textured thermal floor, not the featureless flat
  line a dead/frozen/shut-down ADC produces. Phase 2 (bench sig-gen) —
  a CW tone at 10 MHz, –30 dBm; the carrier must stand a known margin
  above that floor. Canned config `rx888-noise-floor.json` drives
  Phase 1, `rx888-siggen-10mhz.json` drives Phase 2. Both reduce the
  Spectre FITS to numbers (see Operational rigor §5 and Validation
  test), not an eyeballed PNG.

## Operational rigor (adapted from the ka9q-radio harness)

The ka9q-radio harness (`tests/ka9q_test.sh`, `ka9q_smoke.sh`,
`hf_sweep.sh`, `PLAN-KA9Q-HARNESS.md` on the `shdn-when-stopped` branch)
earned several lessons driving a real third-party consumer against this
firmware. The host stack differs but the device contract does not, so
the *methodology* transfers directly — turning this harness from a
one-shot "capture a PNG" into a repeatable, quantified gate. We take the
operating model, not ka9q's specific numbers (those are radiod/`powers`
calibrated; Spectre's are established at the bench, see Baseline).

1. **Lifecycle cycling, not a single capture.** The unit of work is
   start→capture→stop of the *Spectre receiver session* (the thing that
   opens/closes the SoapySDR-SDDC device), repeated. One green capture
   proves the happy path once; cycling proves the firmware survives the
   start/stop path the field actually exercises.

2. **Restart-recovery is a first-class assertion.** After each stop,
   restart spectre-server (or re-open the receiver) and confirm the
   SoapySDR-SDDC driver re-acquires the device and streams real samples
   again. "Did the rx888 driver recover from a Spectre restart?" becomes
   an explicit per-cycle PASS/FAIL — exactly the failure mode a one-shot
   test is blind to.

3. **Exclusive-access alternation.** While spectre-server holds the USB
   interface, host-side `tests/fx3_cmd` cannot claim it (same constraint
   as radiod). The harness alternates: **Spectre up** → assert on the
   captured spectrogram; **Spectre down** → `fx3_cmd stats` to confirm
   the device parked cleanly (GPIF idle, DMA count frozen across two
   reads ~100 ms apart, Si5351 CLK0 state recorded).

4. **Forced firmware reload between cycles.** Beyond the day-one
   uploader sidecar, exercise the cold-start path periodically: chain
   the in-band `RESETFX3` vendor command and a host-side `usbreset`
   (`USBDEVFS_RESET`, enumerate-only, wedge-robust), wait for the
   bootloader PID `04b4:00f3`, re-upload, verify `TESTFX3`. On this
   hardware a bus reset itself drops the FX3 into the bootloader, so
   `ENODEV` mid-reset is success and a re-upload always follows. Run on
   a configurable interval over a soak, not every cycle. The
   `fx3_cmd reload` primitive on the SHDN branch already chains exactly
   this sequence and can be reused unchanged.

5. **Quantitative data collection, scale-independent gate.** Spectre
   writes FITS — numeric spectrogram data — so reduce it rather than
   eyeball the PNG. Per capture, extract floor statistics (mean / stdev
   / min / max / spread) plus a scale-independent liveness metric: the
   fraction of bins within a fixed dynamic range of the spectrum's own
   peak. A live broadband floor keeps most bins in-range; a
   dead/frozen ADC collapses to a lone DC spike with the rest near −∞,
   so the fraction craters. Phase 2 adds carrier peak-to-floor margin
   at the known tone frequency. This is strictly stronger than a
   PNG-size or byte-count check (which noise alone satisfies).

6. **Warm-up / settle.** The first integration after a fresh session
   start can read low while the pipeline settles; take one throwaway
   capture (or trim the first FITS time-row) before the measured one,
   as `hf_sweep.sh` does for cold spectrum channels.

7. **Clean teardown leaves the ADC parked.** On exit the harness stops
   the session and confirms the device returns to the idle,
   ADC-in-SHDN-standby state (consistent with the `fw_test.sh` / soak /
   ka9q teardowns), so a Spectre run never leaves the board powered or
   wedged for the next consumer.

8. **Committed baseline for drift detection.** Record a healthy
   reference once on real hardware — the FITS-derived numbers *and* the
   reference PNG — and commit both. Later runs compare against the
   baseline, so the gate catches regression and slow drift, not just
   total failure.

### Known limitation — direct GPIO/standby readback

As with the ka9q harness, `GETSTATS` does not read back the SHDN GPIO
level, so the "device parked" assertions (§3, §7) are an idle-state
proxy (streaming stopped, DMA frozen), not a direct standby-level read.
If the GETSTATS GPIO-readback enhancement (raised on issue #131) lands,
add a direct standby assertion. The absolute power claim (≥330 mA drop)
remains a manual bench-ammeter check.

## Relationship to upstream Spectre

[spectregrams/spectre#239](https://github.com/spectregrams/spectre/issues/239)
(opened 2026-05-13) proposes Spectre adopt `ringof/rx888-firmware`
as the bundled firmware source; per author context, gated on a 1.0
release here. Implications:

- Direction reverses post-adoption — their build will fetch our
  release asset; today we override their bundled blob.
- This branch's validation PNG is the adoption evidence #239 needs;
  the reference plot should be captured against a release-candidate
  build, not a dev branch.
- The layered firmware-injection design doesn't change post-#239;
  only the source-of-truth for `SDDC_FX3.img` does (local build →
  release-asset URL on both sides).
- Defining 1.0 release criteria and filing a local tracking issue
  for #239 are out of scope for this branch.

## Deliverables

```
docker/spectre/
├── docker-compose.yml      # Pinned upstream images + USB/udev +
│                           # rx888-uploader sidecar + bind-mounts
├── uploader/
│   └── Dockerfile          # debian-slim + libusb + rx888_stream
├── spectre.sh              # Helper: up/down/reflash/status/cli/
│                           # validate/soak/clean
├── fits_stats.py           # Reduce a Spectre FITS to floor stats +
│                           # liveness fraction + carrier margin;
│                           # emits PASS/FAIL numbers (Operational §5)
├── configs/
│   ├── rx888-noise-floor.json   # Phase 1: live-RF texture, no sig-gen
│   └── rx888-siggen-10mhz.json  # Phase 2: known 10 MHz carrier
├── patches/README.md       # Placeholder, mirrors ka9q-radio layout
└── README.md               # Hardware setup, run, validation, regression

docs/spectre-compat-audit.md   # Mirrors docs/ka9q-compat-audit.md style
docs/images/                   # Committed baselines (after first bench run):
│                              #   spectre-noise-floor-expected.png  (Phase 1)
└──                            #   spectre-validation-expected.png   (Phase 2)
```

## Tasks

1. **Recon (live host, not committed):** discover libsddc's
   firmware search path inside `spectre-server:3.5.0-alpha` via
   `docker exec ... strings /usr/local/lib/libsddc.so | grep -E '\.(img|fw)$'`
   and `find / -name 'SDDC_FX3*'`. Record in `docs/spectre-compat-audit.md`.
2. **`docker-compose.yml`:** pin both image tags, mount
   `/dev/bus/usb` + `/run/udev:ro`, bind-mount `SDDC_FX3/SDDC_FX3.img`
   at libsddc's path (from task 1), bind-mount `./capture/` for
   PNG/FITS outputs, wire `rx888-uploader` with
   `depends_on: { rx888-uploader: { condition: service_completed_successfully } }`.
3. **`uploader/Dockerfile`:** debian-slim + `libusb-1.0-0` + a built
   `rx888_stream` from `tests/rx888_tools`. Entrypoint detects PID
   `04b4:00f3` → uploads; `04b4:00f1` → no-op; neither → fails.
4. **`spectre.sh`:** `up`, `down`, `reflash` (re-run uploader only),
   `status` (`SoapySDRUtil --find=driver=SDDC` + `lsusb -d 04b4:`),
   `cli`, `validate` (one capture → `fits_stats.py` → PASS/FAIL),
   `soak` (the lifecycle/restart-recovery loop, Operational §1–4,
   `--cycles`/`--duration`/`--reload-interval` configurable), `clean`.
   Cleanup trap parks the ADC via `fx3_cmd` on exit (Operational §7).
5. **`configs/`:** `rx888-noise-floor.json` (Phase 1 — `rx888mk2`,
   `fixed_center_frequency`, center mid-HF, rate 8 MHz, 50 Ω/antenna,
   no tone) and `rx888-siggen-10mhz.json` (Phase 2 — center 10 MHz,
   rate 8 MHz, 10 s). Param keys confirmed against
   `spectre create config --help` at first run; tags match filenames.
6. **`fits_stats.py`:** read a Spectre FITS (astropy, already in the
   image), reduce to mean/stdev/min/max/spread + the scale-independent
   liveness fraction; with `--carrier <hz>` also report peak-to-floor
   margin. Exit 0/1 against thresholds passed in (no thresholds
   hard-coded — they come from the Baseline). Mirrors the awk reduction
   in `ka9q_smoke.sh`, in Python because the source is FITS not CSV.
7. **Soak / restart-recovery (Operational §1–4):** `spectre.sh soak`
   loops start→capture→`fits_stats.py`→stop→`fx3_cmd stats`, restarts
   between cycles asserting driver re-acquire, and fires `fx3_cmd
   reload` (reused from the SHDN branch) every `--reload-interval`.
   TAP-style line per cycle mirroring `fw_test.sh`.
8. **`README.md`:** mirrors `docker/ka9q-radio/README.md` —
   `lsusb -d 04b4:` walkthrough, run instructions, both-phase
   validation procedure with expected plot descriptions, the soak/
   restart-recovery run, regression note (ka9q-radio container still
   works against same firmware after Spectre run), pointer to compat
   audit.
9. **`docs/spectre-compat-audit.md`:** mirrors
   `docs/ka9q-compat-audit.md`'s structure; initially holds recon
   findings (libsddc fork SHA, firmware path, receiver/mode names).
10. **Doc claim (gated on green real-hardware run):** add "Spectre"
    and "SoapySDR-SDDC + GNU Radio" sections to `docs/compatibility.md`
    and update `README.md`'s host-app list. Lands as a later commit
    on this branch, only after the validation test produces the
    expected plot.

## Validation test (phased)

Each phase brings the stack up, records, reduces the FITS to numbers via
`fits_stats.py`, and PASS/FAILs — then leaves the device parked. The PNG
is committed as the human-readable artifact, but the gate is the numbers.

**Phase 1 — live-RF texture (no bench equipment; the fast gate).**
50 Ω load or antenna on the SMA.

1. `./spectre.sh up` — uploader runs, FX3 ends at PID `00f1`,
   spectre-server comes up.
2. `./spectre.sh status` → one `driver=SDDC` device.
3. `./spectre.sh validate --config rx888-noise-floor.json` records a
   short capture (after a discarded warm-up), then runs `fits_stats.py`.
4. PASS when the floor mean sits inside a sane window and the liveness
   fraction clears its threshold — i.e. a textured thermal floor, not
   the flat line a dead/frozen/shut-down ADC FFTs to. This phase needs
   no signal generator and is the always-run go/no-go, the Spectre
   analogue of `ka9q_smoke.sh`.

**Phase 2 — known carrier (bench sig-gen; the strong gate).**
Signal generator → RX888mk2 SMA, single CW tone at 10.000 MHz, –30 dBm.

5. `./spectre.sh validate --config rx888-siggen-10mhz.json
   --carrier 10000000`.
6. PASS when Phase-1 liveness holds *and* the carrier's peak-to-floor
   margin at 10.000 MHz clears its threshold, with no aliasing wraps.
7. Expected plot: flat noise floor across ~6–14 MHz with one bright
   horizontal line at 10.000 MHz.

**Restart-recovery soak (Operational §1–4).**

8. `./spectre.sh soak --duration 1h --reload-interval 7m` cycles the
   receiver session start→capture→stop, restarting between cycles and
   asserting on each: (a) the SoapySDR-SDDC driver re-acquired the
   device, (b) the capture still passes the Phase-1 liveness gate, (c)
   on the down-leg `fx3_cmd stats` shows the device parked (GPIF idle,
   DMA frozen). `fx3_cmd reload` fires every `--reload-interval` to
   exercise the cold-start / re-upload path. The whole run is one
   PASS/FAIL: did the firmware survive repeated Spectre restarts and
   reloads and recover every time.

## Baseline

On the first green real-hardware run, capture and commit the healthy
reference so later runs detect drift, not just total failure:

- `docs/images/spectre-noise-floor-expected.png` and
  `spectre-validation-expected.png` — the human-readable plots.
- The `fits_stats.py` numbers (floor mean/spread, liveness fraction,
  Phase-2 carrier margin) recorded in `docs/spectre-compat-audit.md`,
  and wired in as the thresholds the `validate`/`soak` gates compare
  against. Thresholds are derived here, empirically — not copied from
  ka9q's radiod/`powers` figures. Per #239, capture against a
  release-candidate build, since this is the adoption evidence upstream
  needs.

## Regression test

After a green Spectre run on a given `SDDC_FX3.img`:

1. `./spectre.sh down`.
2. `cd ../ka9q-radio && ./ka9q.sh start` — same firmware, no
   rebuild, no re-flash.
3. ka9q-radio streams normally; `./ka9q.sh stop`.

Both pipelines independently consuming the same firmware in the
same session proves the firmware isn't silently coupled to one
host stack.

## Open question

Libsddc's exact firmware search path inside
`spectre-server:3.5.0-alpha` is the only unknown remaining. It
must be resolved by task 1 on a host with docker running; until
then, the bind-mount target in task 2 is parameterized as `${SDDC_FW_PATH}`
with candidate values `/usr/local/share/sddc/SDDC_FX3.img` and the
container WORKDIR. README documents the one-liner that resolves it.
