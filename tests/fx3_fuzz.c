/*
 * fx3_fuzz.c — seeded USB/vendor-command fuzzers for the SDDC_FX3 firmware.
 * See fx3_fuzz.h and tests/FUZZ_PLAN.md (issue #139).
 */
#include "fx3_fuzz.h"
#include "fx3_proto.h"
#include "fx3_usb.h"
#include "fx3_stats.h"
#include "fx3_bulk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/time.h>

/* ------------------------------------------------------------------ */
/* Reproducible PRNG (xorshift64*) — independent of libc rand()/srand()*/
/* so a printed seed reproduces a run regardless of platform.          */
/* ------------------------------------------------------------------ */
struct fuzz_rng { uint64_t s; };

static uint64_t fuzz_next(struct fuzz_rng *r)
{
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* Uniform in [0, bound).  bound == 0 returns 0. */
static uint32_t fuzz_below(struct fuzz_rng *r, uint32_t bound)
{
    return bound ? (uint32_t)(fuzz_next(r) % bound) : 0;
}

static uint64_t fuzz_seed_or_time(uint64_t seed)
{
    if (seed == 0) {
        seed = (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ULL;
        seed ^= (uint64_t)getpid() << 17;
    }
    return seed ? seed : 1;   /* xorshift must not start at 0 */
}

/* ------------------------------------------------------------------ */
/* Operation ring buffer + per-command coverage counters              */
/* ------------------------------------------------------------------ */
#define FUZZ_RING 32

struct fuzz_op {
    uint8_t  bmrt;       /* bmRequestType */
    uint8_t  brequest;
    uint16_t wValue, wIndex, wLength;
    uint16_t plen;       /* payload bytes actually moved by libusb */
    int      rc;         /* libusb return */
};

struct fuzz_log {
    struct fuzz_op ring[FUZZ_RING];
    int      head, count;
    uint64_t seq;
    /* coverage, indexed by bRequest 0..255 */
    uint32_t sends[256], accepts[256], stalls[256], errors[256];
    uint32_t in_dir[256], out_dir[256], oversize[256], during_stream[256];
    uint32_t resets, health_checks, health_fails;
};

static void fuzz_log_init(struct fuzz_log *log) { memset(log, 0, sizeof(*log)); }

static void fuzz_record(struct fuzz_log *log, const struct fuzz_op *op, int streaming)
{
    log->ring[log->head] = *op;
    log->head = (log->head + 1) % FUZZ_RING;
    if (log->count < FUZZ_RING) log->count++;
    log->seq++;

    uint8_t b = op->brequest;
    log->sends[b]++;
    if (op->rc >= 0)                        log->accepts[b]++;
    else if (op->rc == LIBUSB_ERROR_PIPE)   log->stalls[b]++;
    else                                    log->errors[b]++;
    if (op->bmrt & LIBUSB_ENDPOINT_IN)      log->in_dir[b]++;
    else                                    log->out_dir[b]++;
    if (op->wLength > FX3_MAX_EP0LEN)        log->oversize[b]++;
    if (streaming)                          log->during_stream[b]++;
}

/* Known command set — biased toward in the generator, and the rows in the
 * coverage report.  Destructive codes (RESETFX3/HANGFX3/HANGMAIN) are
 * deliberately absent: the default fuzz run is non-destructive. */
struct known_cmd { uint8_t code; const char *name; };
static const struct known_cmd KNOWN[] = {
    { STARTFX3, "STARTFX3" }, { STOPFX3, "STOPFX3" }, { TESTFX3, "TESTFX3" },
    { GPIOFX3, "GPIOFX3" },   { I2CWFX3, "I2CWFX3" }, { I2CRFX3, "I2CRFX3" },
    { STARTADC, "STARTADC" }, { GETSTATS, "GETSTATS" },
    { SETARGFX3, "SETARGFX3" }, { READINFODEBUG, "READINFODEBUG" },
};
#define NKNOWN ((int)(sizeof(KNOWN) / sizeof(KNOWN[0])))

static const char *cmd_name(uint8_t code)
{
    for (int i = 0; i < NKNOWN; i++)
        if (KNOWN[i].code == code) return KNOWN[i].name;
    if (code == RESETFX3) return "RESETFX3";
    if (code == HANGFX3)  return "HANGFX3";
    if (code == HANGMAIN) return "HANGMAIN";
    return "?";
}

/* ------------------------------------------------------------------ */
/* Failure log + coverage report                                      */
/* ------------------------------------------------------------------ */
static void fuzz_dump(const struct fuzz_log *log, uint64_t seed,
                      const char *mode, libusb_device_handle *h)
{
    printf("\n>>> FUZZ FAILURE LOG (%s) <<<\n", mode);
    printf("  seed:       0x%016llx\n", (unsigned long long)seed);
    printf("  total ops:  %llu\n", (unsigned long long)log->seq);
    printf("  last %d operations (oldest first):\n", log->count);

    int start = (log->head - log->count + FUZZ_RING) % FUZZ_RING;
    for (int k = 0; k < log->count; k++) {
        const struct fuzz_op *op = &log->ring[(start + k) % FUZZ_RING];
        const char *res = op->rc >= 0 ? "ok"
                        : op->rc == LIBUSB_ERROR_PIPE ? "STALL"
                        : libusb_strerror(op->rc);
        printf("    [%2d] bmRT=0x%02X %-3s bReq=0x%02X(%-9s) "
               "wVal=0x%04X wIdx=0x%04X wLen=%-4u plen=%-4u -> %s\n",
               k, op->bmrt, (op->bmrt & LIBUSB_ENDPOINT_IN) ? "IN" : "OUT",
               op->brequest, cmd_name(op->brequest),
               op->wValue, op->wIndex, op->wLength, op->plen, res);
    }

    struct fx3_stats s;
    if (h && read_stats(h, &s) == 0)
        printf("  GETSTATS@fail: dma=%u gpif=%u pib=%u i2c=%u faults=%u boot=%u\n",
               s.dma_count, s.gpif_state, s.pib_errors, s.i2c_failures,
               s.streaming_faults, s.boot_count);
    else
        printf("  GETSTATS@fail: unreadable\n");

    /* PID visibility — app, bootloader, or gone. */
    libusb_device_handle *p;
    if ((p = libusb_open_device_with_vid_pid(g_ctx, RX888_VID, RX888_PID_APP))) {
        libusb_close(p); printf("  device PID: 0x%04X (app)\n", RX888_PID_APP);
    } else if ((p = libusb_open_device_with_vid_pid(g_ctx, RX888_VID, RX888_PID_BOOT))) {
        libusb_close(p); printf("  device PID: 0x%04X (bootloader — firmware dropped!)\n",
                                RX888_PID_BOOT);
    } else {
        printf("  device PID: none visible (gone)\n");
    }
    printf("  xHCI/kernel logs: not available to the host tool (N/A)\n");
    printf("  reproduce: fx3_cmd %s <count> 0x%016llx\n",
           mode, (unsigned long long)seed);
    fflush(stdout);
}

static void fuzz_coverage_report(const struct fuzz_log *log)
{
    printf("\n=== %s COVERAGE ===\n", "PROTOCOL");
    printf("%-14s %7s %6s %6s %6s %6s %6s %7s %7s\n",
           "cmd", "sends", "acc", "stall", "err", "IN", "OUT", "oversz", "stream");

    uint32_t t_sends = 0, t_acc = 0, t_stall = 0, t_err = 0,
             t_in = 0, t_out = 0, t_ovr = 0, t_str = 0;
    uint32_t k_sends = 0;
    for (int c = 0; c < 256; c++) {
        t_sends += log->sends[c]; t_acc += log->accepts[c];
        t_stall += log->stalls[c]; t_err += log->errors[c];
        t_in += log->in_dir[c]; t_out += log->out_dir[c];
        t_ovr += log->oversize[c]; t_str += log->during_stream[c];
    }
    for (int i = 0; i < NKNOWN; i++) {
        uint8_t c = KNOWN[i].code;
        k_sends += log->sends[c];
        printf("%-14s %7u %6u %6u %6u %6u %6u %7u %7u\n",
               KNOWN[i].name, log->sends[c], log->accepts[c], log->stalls[c],
               log->errors[c], log->in_dir[c], log->out_dir[c],
               log->oversize[c], log->during_stream[c]);
    }
    /* Aggregate everything that isn't a named command into one row. */
    printf("%-14s %7u %6s %6s %6s %6s %6s %7s %7s\n",
           "unknown/other", t_sends - k_sends, "-", "-", "-", "-", "-", "-", "-");
    printf("%-14s %7u %6u %6u %6u %6u %6u %7u %7u\n",
           "TOTAL", t_sends, t_acc, t_stall, t_err, t_in, t_out, t_ovr, t_str);
    printf("  resets observed: %u   health checks: %u ok / %u total\n",
           log->resets, log->health_checks - log->health_fails, log->health_checks);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Health gate + open-only re-acquire                                 */
/* ------------------------------------------------------------------ */
#define FUZZ_HWCONFIG_BAD (-1000)   /* sentinel: device alive but hwconfig wrong */

/* Raw health probe: TESTFX3 (alive, hwconfig) + GETSTATS (records a reset
 * when boot_count moves).  No retry, no gate counters — see fuzz_gate. */
static int fuzz_probe(libusb_device_handle *h, struct fuzz_log *log,
                      uint32_t *boot_seen)
{
    uint8_t info[4] = {0};
    int r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) return r;
    if (r >= 1 && info[0] != 0x04)   /* hwconfig: RX888r2 (matches soak_health_check) */
        return FUZZ_HWCONFIG_BAD;

    struct fx3_stats s;
    r = read_stats(h, &s);
    if (r < 0) return r;
    if (*boot_seen && s.boot_count != *boot_seen) log->resets++;
    *boot_seen = s.boot_count;
    return 0;
}

/* Close the (possibly stale) handle and reopen at the app PID.  Open-only:
 * the fuzzers do not re-upload firmware — a device that dropped to the
 * bootloader is itself a finding, surfaced by fuzz_dump(). */
static int fuzz_reacquire(libusb_device_handle **h)
{
    if (*h) { close_rx888(*h); *h = NULL; }
    usleep(500000);
    libusb_device_handle *fresh = open_rx888(g_ctx);
    if (fresh) { *h = fresh; return 0; }
    return -1;
}

/* Health gate — mirrors soak_health_check's proven recovery handling.
 *
 * A fuzzed STARTFX3 can leave the device streaming with no EP1 consumer; the
 * firmware's streaming watchdog then churns through its recovery cap, during
 * which EP0 control transfers transiently return EIO/timeout.  A single-shot
 * probe races that recovery (the device is alive, just busy), so on a
 * transient failure we STOP, give the watchdog the established ~2 s window,
 * and probe once more before declaring failure.  Only a definitive NO_DEVICE
 * (re-enumeration) triggers a re-acquire — and only outside quiet/burst mode,
 * where the soak's own safety net owns recovery.
 *
 * Returns 0 healthy, else the last probe's status.  Counts one health check
 * per gate; a fail is counted only when the gate ultimately fails. */
static int fuzz_gate(libusb_device_handle **h, struct fuzz_log *log,
                     uint32_t *boot, int quiet)
{
    log->health_checks++;
    cmd_u32(*h, STOPFX3, 0);          /* halt any abandoned stream */
    usleep(100000);                   /* 100 ms — let GPIF/DMA quiesce */
    int hc = fuzz_probe(*h, log, boot);
    if (hc == 0) return 0;

    if (hc != LIBUSB_ERROR_NO_DEVICE) {
        usleep(2000000);              /* 2 s watchdog recovery window */
        cmd_u32(*h, STOPFX3, 0);
        hc = fuzz_probe(*h, log, boot);
        if (hc == 0) return 0;
    }

    if (hc == LIBUSB_ERROR_NO_DEVICE && !quiet && fuzz_reacquire(h) == 0) {
        log->resets++;
        hc = fuzz_probe(*h, log, boot);
        if (hc == 0) return 0;
    }

    log->health_fails++;
    return hc;
}

/* ------------------------------------------------------------------ */
/* protocol_fuzz — one fuzzed EP0 control transfer                    */
/* ------------------------------------------------------------------ */
static volatile sig_atomic_t fuzz_stop;
static void fuzz_sigint(int sig) { (void)sig; fuzz_stop = 1; }

/* Build and send one random EP0 control transfer, recording it. */
static void protocol_fuzz_step(libusb_device_handle *h, struct fuzz_rng *rng,
                               struct fuzz_log *log, int streaming)
{
    /* bRequest: 70% from the known set, 30% any code. */
    uint8_t br;
    if (fuzz_below(rng, 100) < 70)
        br = KNOWN[fuzz_below(rng, NKNOWN)].code;
    else
        br = (uint8_t)fuzz_below(rng, 256);
    /* Remap destructive codes away so the default run is non-destructive. */
    if (br == RESETFX3 || br == HANGFX3 || br == HANGMAIN)
        br = TESTFX3;

    /* bmRequestType: fully fuzz the DIRECTION (IN/OUT) and RECIPIENT
     * (device/interface/endpoint/other) — both reach the firmware's vendor
     * handler, which dispatches purely on bRequest and never checks them, so
     * wrong-direction and wrong-recipient are exactly the cases we want to
     * hammer (IN to an OUT-only command and vice-versa).
     *
     * The TYPE field is deliberately held at VENDOR.  Standard/class requests
     * (GET_DESCRIPTOR, SET_CONFIGURATION, SET_ADDRESS, ...) are serviced by
     * the Cypress SDK fast-enumeration layer, NOT the firmware logic under
     * test; fuzzing them mostly exercises the host/kernel and can deconfigure
     * the device (host-side false positives).  Full type fuzzing is a
     * documented follow-up (see FUZZ_PLAN.md). */
    uint8_t dir   = fuzz_below(rng, 2) ? LIBUSB_ENDPOINT_IN : LIBUSB_ENDPOINT_OUT;
    uint8_t recip = fuzz_below(rng, 100) < 70 ? LIBUSB_RECIPIENT_DEVICE
                                              : (uint8_t)fuzz_below(rng, 4);
    uint8_t bmrt  = dir | LIBUSB_REQUEST_TYPE_VENDOR | recip;

    uint16_t wValue = (uint16_t)fuzz_below(rng, 0x10000);
    uint16_t wIndex = (uint16_t)fuzz_below(rng, 0x10000);
    /* 30% of the time snap to plausible values to reach deeper paths. */
    if (fuzz_below(rng, 100) < 30) {
        if (br == SETARGFX3)                  wIndex = (uint16_t)(8 + fuzz_below(rng, 10));
        else if (br == I2CWFX3 || br == I2CRFX3) { wValue = 0xC0; wIndex = (uint16_t)fuzz_below(rng, 256); }
    }

    /* wLength weighted across {0, 1, 4, 2..64 exact-ish, 65..512 oversize}. */
    uint16_t wLength;
    uint32_t wsel = fuzz_below(rng, 100);
    if      (wsel < 15) wLength = 0;
    else if (wsel < 30) wLength = 1;
    else if (wsel < 55) wLength = 4;
    else if (wsel < 80) wLength = (uint16_t)(2 + fuzz_below(rng, 63));    /* 2..64 */
    else                wLength = (uint16_t)(65 + fuzz_below(rng, 448));  /* 65..512 */

    static uint8_t buf[512];
    uint16_t plen = wLength <= sizeof(buf) ? wLength : (uint16_t)sizeof(buf);
    for (uint16_t i = 0; i < plen; i++) buf[i] = (uint8_t)fuzz_next(rng);

    int rc = libusb_control_transfer(h, bmrt, br, wValue, wIndex, buf, plen, 250);

    struct fuzz_op op = { bmrt, br, wValue, wIndex, wLength, plen, rc };
    fuzz_record(log, &op, streaming);
}

static int protocol_fuzz_core(libusb_device_handle **h_inout, long num_ops,
                              uint64_t seed, int quiet)
{
    libusb_device_handle *h = *h_inout;
    if (num_ops <= 0) num_ops = 5000;
    seed = fuzz_seed_or_time(seed);
    struct fuzz_rng rng = { seed };
    struct fuzz_log log; fuzz_log_init(&log);

    if (!quiet) {
        fuzz_stop = 0;
        signal(SIGINT, fuzz_sigint);
        printf("=== PROTOCOL_FUZZ === ops=%ld seed=0x%016llx\n",
               num_ops, (unsigned long long)seed);
    }

    uint32_t boot = 0;
    if (fuzz_gate(&h, &log, &boot, quiet) != 0) {
        printf("FAIL protocol_fuzz: device unhealthy before start\n");
        *h_inout = h;
        return 1;
    }

    int rc = 0;
    for (long i = 0; i < num_ops && !fuzz_stop; i++) {
        protocol_fuzz_step(h, &rng, &log, /*streaming=*/0);

        if ((i + 1) % 64 == 0) {
            int hc = fuzz_gate(&h, &log, &boot, quiet);
            if (hc != 0) {
                printf("FAIL protocol_fuzz: health gate failed at op %ld (%s)\n",
                       i + 1, hc == FUZZ_HWCONFIG_BAD ? "hwconfig changed"
                                                      : libusb_strerror(hc));
                fuzz_dump(&log, seed, "protocol_fuzz", h);
                rc = 1;
                break;
            }
        }
    }

    /* Leave the device idle.  h may be NULL here: a failed gate re-acquire
     * (e.g. the wedge tripped the #137 escalation and the device dropped to
     * the bootloader) closes the handle and clears it.  Guard against it —
     * libusb_control_transfer(NULL, ...) dereferences NULL. */
    if (h) cmd_u32(h, STOPFX3, 0);
    if (!quiet) {
        signal(SIGINT, SIG_DFL);
        fuzz_coverage_report(&log);
        if (rc == 0)
            printf("PASS protocol_fuzz: %llu ops, %u resets, %u/%u health checks ok\n",
                   (unsigned long long)log.seq, log.resets,
                   log.health_checks - log.health_fails, log.health_checks);
    } else if (rc != 0) {
        /* Burst mode still surfaces the seed so a soak failure reproduces. */
        printf(">>> protocol_fuzz_burst FAIL seed=0x%016llx\n",
               (unsigned long long)seed);
    }
    *h_inout = h;
    return rc;
}

int fuzz_protocol(libusb_device_handle **h_inout, long num_ops, uint64_t seed)
{
    return protocol_fuzz_core(h_inout, num_ops, seed, /*quiet=*/0);
}

/* ------------------------------------------------------------------ */
/* stream_fuzz — async bulk + host-lifecycle fuzzer                   */
/* ------------------------------------------------------------------ */
#define SF_POOL 8

struct sf_slot {
    struct libusb_transfer  *x;
    uint8_t                 *buf;
    struct primed_xfer_state st;
    int                      active;
};

/* Candidate bulk read sizes — spans sub-packet, packet-boundary, and large
 * transfers (proposal #6). */
static const int SF_SIZES[] = {
    1, 2, 3, 511, 512, 1023, 1024, 1025, 16384, 65536, 262144, 1048576
};
#define SF_NSIZES ((int)(sizeof(SF_SIZES) / sizeof(SF_SIZES[0])))

static int sf_submit(libusb_device_handle *h, struct sf_slot *s,
                     int len, int timeout_ms)
{
    s->buf = malloc(len);
    if (!s->buf) return LIBUSB_ERROR_NO_MEM;
    s->x = libusb_alloc_transfer(0);
    if (!s->x) { free(s->buf); s->buf = NULL; return LIBUSB_ERROR_NO_MEM; }
    s->st.completed = 0; s->st.actual_length = 0; s->st.status = 0;
    libusb_fill_bulk_transfer(s->x, h, EP1_IN, s->buf, len,
                              primed_xfer_cb, &s->st, timeout_ms);
    int r = libusb_submit_transfer(s->x);
    if (r < 0) {
        libusb_free_transfer(s->x); free(s->buf);
        s->x = NULL; s->buf = NULL;
        return r;
    }
    s->active = 1;
    return 0;
}

/* Free any slot whose transfer has reported completion (incl. cancellation). */
static void sf_reap(struct sf_slot *pool)
{
    for (int i = 0; i < SF_POOL; i++) {
        if (pool[i].active && pool[i].st.completed) {
            libusb_free_transfer(pool[i].x);
            free(pool[i].buf);
            pool[i].x = NULL; pool[i].buf = NULL; pool[i].active = 0;
        }
    }
}

static int sf_count_active(struct sf_slot *pool)
{
    int n = 0;
    for (int i = 0; i < SF_POOL; i++) if (pool[i].active) n++;
    return n;
}

/* Cancel every in-flight transfer and pump events until they are all freed.
 * libusb forbids closing a handle with submitted transfers, so this is how
 * "abandon / close with reads in flight" is modeled safely host-side: the
 * kernel still dequeues the URBs (the path open_rx888()'s clear_halt
 * recovers), without leaking transfers in the fuzzer itself. */
static void sf_drain(struct sf_slot *pool)
{
    for (int i = 0; i < SF_POOL; i++)
        if (pool[i].active) libusb_cancel_transfer(pool[i].x);
    int guard = 0;
    while (sf_count_active(pool) > 0 && guard++ < 1000) {
        struct timeval tv = { 0, 5000 };   /* 5 ms */
        libusb_handle_events_timeout(g_ctx, &tv);
        sf_reap(pool);
    }
}

static int stream_fuzz_core(libusb_device_handle **h_inout, double seconds,
                            uint64_t seed, int quiet)
{
    libusb_device_handle *h = *h_inout;
    if (seconds <= 0) seconds = 60.0;
    seed = fuzz_seed_or_time(seed);
    struct fuzz_rng rng = { seed };
    struct fuzz_log log; fuzz_log_init(&log);
    struct sf_slot pool[SF_POOL];
    memset(pool, 0, sizeof(pool));

    if (!quiet) {
        fuzz_stop = 0;
        signal(SIGINT, fuzz_sigint);
        printf("=== STREAM_FUZZ === secs=%.1f seed=0x%016llx\n",
               seconds, (unsigned long long)seed);
    }

    uint32_t boot = 0;
    if (fuzz_gate(&h, &log, &boot, quiet) != 0) {
        printf("FAIL stream_fuzz: device unhealthy before start\n");
        *h_inout = h;
        return 1;
    }

    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double next_gate = 1.0;
    int streaming = 0;
    int rc = 0;

    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;
        if (elapsed >= seconds || fuzz_stop) break;

        /* Pump async completions first. */
        struct timeval tv = { 0, 2000 };   /* 2 ms */
        libusb_handle_events_timeout(g_ctx, &tv);
        sf_reap(pool);

        /* Pick a weighted action. */
        uint32_t a = fuzz_below(&rng, 100);
        if (!streaming && a < 25) {
            /* Start streaming: program a random ADC rate, then STARTFX3. */
            uint32_t rates[] = { 16000000, 32000000, 50000000, 64000000, 1000000 };
            cmd_u32(h, STARTADC, rates[fuzz_below(&rng, 5)]);
            int r = cmd_u32(h, STARTFX3, 0);
            struct fuzz_op op = { LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
                                  STARTFX3, 0, 0, 4, 4, r };
            fuzz_record(&log, &op, streaming);
            if (r >= 0) streaming = 1;
        } else if (streaming && a < 15) {
            /* Stop (possibly with reads in flight). */
            int r = cmd_u32(h, STOPFX3, 0);
            struct fuzz_op op = { LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
                                  STOPFX3, 0, 0, 4, 4, r };
            fuzz_record(&log, &op, streaming);
            streaming = 0;
        } else if (a < 55) {
            /* Submit an async read of a random size/timeout, if a slot is free. */
            int slot = -1;
            for (int i = 0; i < SF_POOL; i++) if (!pool[i].active) { slot = i; break; }
            if (slot >= 0) {
                int len = SF_SIZES[fuzz_below(&rng, SF_NSIZES)];
                int to_choices[] = { 1, 5, 20, 100, 500, 2000 };
                int timeout = to_choices[fuzz_below(&rng, 6)];
                sf_submit(h, &pool[slot], len, timeout);
            }
        } else if (a < 70) {
            /* Cancel a random in-flight transfer (cancel at a random offset). */
            int act = sf_count_active(pool);
            if (act > 0) {
                int pick = (int)fuzz_below(&rng, (uint32_t)act);
                for (int i = 0, seen = 0; i < SF_POOL; i++) {
                    if (pool[i].active && seen++ == pick) {
                        libusb_cancel_transfer(pool[i].x);
                        break;
                    }
                }
            }
        } else if (a < 88) {
            /* EP0 op while reads may be pending. */
            switch (fuzz_below(&rng, 4)) {
            case 0: { struct fx3_stats s; read_stats(h, &s); break; }
            case 1: { uint8_t b[8]; ctrl_read(h, I2CRFX3, 0xC0,
                       (uint16_t)fuzz_below(&rng, 256), b, 1); break; }
            case 2: { uint8_t b[64]; ctrl_read(h, READINFODEBUG, 0, 0, b, sizeof(b)); break; }
            case 3: cmd_u32(h, STARTADC, 32000000); break;
            }
        } else if (a < 94) {
            /* Release + re-claim / close + reopen, sometimes with reads in
             * flight (sf_drain cancels them first — see its comment). */
            sf_drain(pool);
            libusb_device_handle *fresh;
            close_rx888(h);
            usleep(50000);
            fresh = open_rx888(g_ctx);
            if (!fresh) {
                printf("FAIL stream_fuzz: reopen failed after close cycle\n");
                fuzz_dump(&log, seed, "stream_fuzz", NULL);
                h = NULL; rc = 1; break;
            }
            h = fresh;
            streaming = 0;
        } else {
            /* Abandon window: stop reading for a random 1 ms .. 200 ms. */
            usleep(1000 + fuzz_below(&rng, 199000));
        }

        /* Periodic health gate. */
        if (elapsed >= next_gate) {
            next_gate = elapsed + 1.0;
            sf_drain(pool);
            streaming = 0;
            int hc = fuzz_gate(&h, &log, &boot, quiet);
            if (hc != 0) {
                printf("FAIL stream_fuzz: health gate failed at %.1fs (%s)\n",
                       elapsed, hc == FUZZ_HWCONFIG_BAD ? "hwconfig changed"
                                                        : libusb_strerror(hc));
                fuzz_dump(&log, seed, "stream_fuzz", h);
                rc = 1;
                break;
            }
        }
    }

    /* Teardown: cancel/free outstanding transfers, stop, park idle. */
    sf_drain(pool);
    if (h) cmd_u32(h, STOPFX3, 0);

    if (!quiet) {
        signal(SIGINT, SIG_DFL);
        printf("  stream_fuzz: %llu bulk/EP0 ops, %u resets, %u/%u health ok\n",
               (unsigned long long)log.seq, log.resets,
               log.health_checks - log.health_fails, log.health_checks);
        if (rc == 0)
            printf("PASS stream_fuzz: device survived %.1fs of stream fuzzing\n",
                   seconds);
    } else if (rc != 0) {
        printf(">>> stream_fuzz_burst FAIL seed=0x%016llx\n",
               (unsigned long long)seed);
    }
    *h_inout = h;
    return rc;
}

int fuzz_stream(libusb_device_handle **h_inout, double seconds, uint64_t seed)
{
    return stream_fuzz_core(h_inout, seconds, seed, /*quiet=*/0);
}

/* ------------------------------------------------------------------ */
/* Bounded soak bursts                                                */
/* ------------------------------------------------------------------ */

/* 64-bit seed from two rand() draws so the burst stays reproducible from the
 * soak's srand(seed). */
static uint64_t burst_seed(void)
{
    uint64_t s = ((uint64_t)(uint32_t)rand() << 32) ^ (uint32_t)rand();
    return s ^ 0x9E3779B97F4A7C15ULL;
}

int do_test_protocol_fuzz_burst(libusb_device_handle *h)
{
    libusb_device_handle *hh = h;
    int rc = protocol_fuzz_core(&hh, 200, burst_seed(), /*quiet=*/1);
    if (rc == 0) printf("PASS protocol_fuzz_burst: 200 ops, device healthy\n");
    return rc;
}

int do_test_stream_fuzz_burst(libusb_device_handle *h)
{
    libusb_device_handle *hh = h;
    int rc = stream_fuzz_core(&hh, 3.0, burst_seed(), /*quiet=*/1);
    if (rc == 0) printf("PASS stream_fuzz_burst: 3s, device healthy\n");
    return rc;
}
