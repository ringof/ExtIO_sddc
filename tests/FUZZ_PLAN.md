# Plan: Seeded USB/vendor-command fuzzing for SDDC_FX3

Turns the existing "very good regression torture" suite into black-box
**hostile-interface testing**. Scope for *this* PR (agreed):

- `protocol_fuzz` — seeded EP0 control-transfer fuzzer
- `stream_fuzz` — seeded bulk/streaming + host-lifecycle fuzzer
- a rolling **operation ring-buffer failure log** (seed + last N ops)
- per-command **coverage tracking** ("we tried to kill it" made measurable)
- wired into the soak rotation as **low-weight bounded bursts**

Explicitly **deferred to a follow-up PR**: `destructive_test.sh` and the
formal `docs/` per-command coverage matrix.

The new fuzzer lands as its **own module** (`fx3_fuzz.c`), which is also the
forcing function for **Phase 0** below: a first, contained step in breaking
the 6060-line `fx3_cmd.c` into compilable modules. Everything compiles here
against libusb, but **cannot be hardware-validated in this container**
(no RX888) — validation is by code inspection plus a clean build; a
hardware validation procedure is provided for the maintainer to run.

---

## 0. Modularization — a *start* (not a full rewrite)

`fx3_cmd.c` is one 6060-line translation unit. A full split is risky and out
of scope; instead this PR extracts exactly the shared lower layers the new
fuzz module depends on, establishes the multi-object build, and leaves the
bulk of the file (scenarios, soak, recovery, `main()`) in place for a
follow-up. Net effect: `fx3_cmd.c` shrinks by the extracted layers and gains
`#include`s; behavior is byte-for-byte unchanged.

New files (header = declarations, `.c` = definitions moved verbatim):

| Module | Contents moved out of `fx3_cmd.c` |
|--------|-----------------------------------|
| `fx3_proto.h` | All protocol constants/enums now duplicated at the top of `fx3_cmd.c`: VID/PIDs, command codes, GPIO masks, SETARGFX3 arg ids, `GETSTATS_LEN`, `CTRL_TIMEOUT_MS`, `EP1_IN`, EP0-max mirror. Header-only. |
| `fx3_usb.{h,c}` | USB transport + device lifecycle: `ctrl_write_u32/buf`, `ctrl_read`, `cmd_u32`, `cmd_u32_retry`, `set_arg`, `ep0_alive_after_stall`, `open_rx888`/`close_rx888`, `find_rx888_stream`, `upload_firmware`, `do_usbreset`, `do_reload`, and the `g_ctx`/`g_firmware_path` globals (defined here, `extern` in header). |
| `fx3_stats.{h,c}` | `struct fx3_stats`, `read_stats()`, `do_stats()`. |
| `fx3_bulk.{h,c}` | `struct primed_xfer_state`, `primed_start_and_read[_retry]()`. |

What **stays** in `fx3_cmd.c` for now (explicitly deferred): every
`do_test_*` scenario, the debug console + local-command table, the soak
runner, the Level-4/5 recovery tests, and `main()`. A short "Future
modularization" note is added at the top of `fx3_cmd.c` naming the obvious
next cuts (`fx3_scenarios.c`, `fx3_soak.c`).

Mechanical risk is the only risk: `static` → external linkage for the moved
symbols, consistent declarations, no semantic edits. Verified by the clean
build here (the whole point of doing it under libusb in-container).

### Makefile
`fx3_cmd` becomes a multi-object link:

```make
FX3_OBJS := fx3_cmd.o fx3_usb.o fx3_stats.o fx3_bulk.o fx3_fuzz.o
fx3_cmd: $(FX3_OBJS)
	$(CC) $(CFLAGS) -o $@ $(FX3_OBJS) $(LDLIBS)
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<
```
`clean` updated to `rm -f *.o`. The `check-health` host unit test is
untouched.

---

## 1. Shared infrastructure (new — in `fx3_fuzz.c`)

### 1a. Reproducible PRNG, independent of `rand()`
`rand()`/`srand()` are libc-defined and already owned by the soak loop. A
fuzzer that prints "seed S reproduces this stream" needs its own
deterministic generator:

```c
struct fuzz_rng { uint64_t s; };
static uint64_t fuzz_next(struct fuzz_rng *r);   /* xorshift64* */
static uint32_t fuzz_below(struct fuzz_rng *r, uint32_t bound);
static uint32_t fuzz_pick_weighted(struct fuzz_rng *r,
                                   const int *weights, int n);
```

### 1b. Operation ring buffer + coverage counters
```c
#define FUZZ_RING 32
struct fuzz_op { uint8_t bmRequestType, bRequest;
                 uint16_t wValue, wIndex, wLength, actual_len; int rc; };
struct fuzz_log {
    struct fuzz_op ring[FUZZ_RING]; int head, count; uint64_t seq;
    uint32_t sends[256], stalls[256], accepts[256], errors[256];
    uint32_t in_dir[256], out_dir[256], oversize[256], during_stream[256];
    uint32_t resets_observed, health_checks, health_fails;
};
static void fuzz_record(struct fuzz_log*, const struct fuzz_op*, int streaming);
static void fuzz_dump(const struct fuzz_log*, uint64_t seed,
                      const struct fx3_stats *last_ok);  /* on failure */
static void fuzz_coverage_report(const struct fuzz_log*); /* on exit */
```

`fuzz_dump()` emits, per the proposal's "rolling failure log" (#4):
seed, total ops, the last ≤32 generated ops (decoded), the last good
GETSTATS, failure-time GETSTATS if readable, and which PID (app/boot/none)
is currently visible. xHCI/kernel logs are out of host reach, so noted as
N/A.

`fuzz_coverage_report()` prints the per-command matrix (#5/#7): for each
known bRequest and an "unknown" bucket — sends, accepts, stalls, errors,
IN vs OUT, oversize attempts, during-stream attempts, resets observed,
health pass/fail.

---

## 2. `protocol_fuzz [num_ops] [seed]`  (addition #1)

Default `num_ops=5000`, `seed=time(NULL)`. One fuzzed EP0 transfer per
step:

- **bRequest:** ~70% from the known set
  `{STARTFX3,STOPFX3,TESTFX3,GPIOFX3,I2CWFX3,I2CRFX3,STARTADC,GETSTATS,
  SETARGFX3,READINFODEBUG}`; ~30% any `0..255`.
  **Deliberately remapped away from `RESETFX3(0xB1)`, `HANGFX3(0xCE)`,
  `HANGMAIN(0xCF)`** so the default run is non-destructive and fast
  (no 2–5 s EP0 sleeps, no forced re-enumeration). Documented; a
  `--destructive` opt-in is left for the follow-up.
- **bmRequestType:** randomize direction/type/recipient; ~50% biased to the
  legit `VENDOR|DEVICE` pair, else any combination — covering invalid
  direction, invalid recipient, invalid type, IN-to-OUT-only, OUT-to-IN-only.
- **wValue/wIndex:** random 16-bit, occasionally snapped to plausible values
  (valid SETARGFX3 arg ids, valid I2C addr) to reach deeper code paths.
- **wLength:** weighted over `{0, 1, 4, rand 2..64, 65..512 oversize}`
  → covers 0, short, exact, and larger-than-EP0-buffer.
- **payload (OUT):** random bytes, length == wLength (capped at a 512-byte
  buffer). `wLength==0` on a data-expecting command models the **missing
  OUT data stage**.

Every 64 ops → **health gate**: TESTFX3 (alive, hwconfig==0x04) + GETSTATS.
`boot_count` delta ⇒ a reset occurred (recorded; if the device dropped to
bootloader, reuse `soak_try_reacquire()` to reload+reacquire). A health
failure is the **only PASS/FAIL gate** — individual STALLs are expected and
merely logged. On failure: `fuzz_dump()` then attempt recovery.

**Known limitation (documented):** a *truly* short OUT data stage
(setup says N, host sends <N) needs raw `USBDEVFS_SUBMITURB`; libusb's sync
API always moves exactly `wLength`. Out of scope for v1 — noted in code and
README.

End: STOPFX3 cleanup + `fuzz_coverage_report()`.

## 3. `stream_fuzz [seconds] [seed]`  (addition #2)

Default `seconds=60`. Maintains a pool of up to 8 async bulk transfers
(reusing the existing `primed_xfer_state` pattern) and drives
`libusb_handle_events_timeout_completed()` each iteration to reap them.
Weighted random action per step (addition #6 behaviors):

- start streaming (STARTADC@random rate + STARTFX3) when idle
- stop streaming (STOPFX3) — incl. **STOP while reads pending**
- submit an async bulk read of a random size from
  `{1,2,3,511,512,1023,1024,1025,16384,65536,1<<20}` with a random timeout
- cancel a random pending transfer (cancel at random offsets)
- GETSTATS / I2CR / READINFODEBUG / STARTADC **while transfers pending**
- release+re-claim / close+reopen the handle (repeat during streaming)
- stop reading for a random 1 ms–5 s window, then resume

**Host-safety note:** libusb forbids `libusb_close()` while transfers are
still submitted, so "close with transfers in flight" is modeled as
**cancel → close immediately** (don't fully drain) — this reproduces the
kernel's URB-dequeue path that `open_rx888()`'s `clear_halt` is built to
recover, without crashing the fuzzer itself. Documented.

Periodic health gate: device must return to answering TESTFX3+GETSTATS;
`streaming_faults` may rise but must stay **bounded** (the watchdog cap).
Unrecoverable wedge ⇒ FAIL + `fuzz_dump()`.

## 4. Soak integration (low weight, bounded)

Two thin wrappers reusing the engines with a small budget, seeded from the
soak's `rand()` so they stay reproducible from the soak seed:

- `do_test_protocol_fuzz_burst(h)` — ~200 ops, health-gated, ends STOPFX3.
- `do_test_stream_fuzz_burst(h)` — ~3 s of stream fuzzing, ends idle.

Added to `scenarios[]` at **weight 3** each, and to `local_cmds_noarg[]`
(`!protocol_fuzz_burst` / `!stream_fuzz_burst`). They obey soak
conventions: STOPFX3 on success, lean on inter-scenario cleanup for early
exits, never intentionally re-enumerate (destructive codes excluded).

## 5. Wiring & docs

- `fx3_cmd.c` gains `#include "fx3_proto.h"`, `"fx3_usb.h"`, `"fx3_stats.h"`,
  `"fx3_bulk.h"`, `"fx3_fuzz.h"` in place of the moved code.
- `main()`: dispatch `protocol_fuzz` / `stream_fuzz` (with `[n]/[secs]`
  + `[seed]` args); extend `usage()`.
- `README.md`: new fuzzer subsections (usage, the failure-log format, the
  coverage report) under EP0 and streaming.
- `CHANGELOG.md`: one entry.

## 6. Validation (maintainer-run; not possible in this container)

- **Build:** `cd tests && make fx3_cmd` — clean (`-Wall -Wextra`). Done here.
- **Functional:** `./fx3_cmd -F ../SDDC_FX3/SDDC_FX3.img protocol_fuzz 5000 1`
  and `./fx3_cmd stream_fuzz 60 1` on hardware → expect PASS, a coverage
  report, and (on an injected fault) a seed-stamped ring dump that
  reproduces with the same seed.
- **Regression:** `./fw_test.sh` quick TAP pass unchanged; a short
  `./fx3_cmd soak 0.05 42` still completes — the two new weight-3 bursts
  appear in the summary table without destabilizing existing scenarios.

## 7. Risks

- The soak bursts add randomness to the rotation; bounded op/time budgets
  and exclusion of destructive codes keep them from masking or cascading
  into other scenarios. If a burst proves to contaminate neighbors (à la
  `gpio_extremes`), it gets pulled from `scenarios[]` and stays
  subcommand-only — same escape hatch the suite already uses.
