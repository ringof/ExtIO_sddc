/*
 * fx3_fuzz.c — seeded USB/vendor-command fuzzers for the SDDC_FX3 firmware.
 * See fx3_fuzz.h and the tests/README.md fuzzing section (issue #139).
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
     * possible follow-up. */
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

/* #148: does the device still stream?  The EP0 health gate only checks
 * TESTFX3 + GETSTATS, so a run that left the device unable to stream (e.g.
 * protocol_fuzz reconfigured the Si5351 via I2CWFX3) could still report PASS.
 * Probe streaming so the final verdict is honest.  Returns 1 if data flows. */
static int fuzz_streams_ok(libusb_device_handle *h)
{
    if (!h) return 0;
    cmd_u32(h, STARTADC, 32000000);
    int got = primed_start_and_read_retry(h, 16384, 2000);
    cmd_u32(h, STOPFX3, 0);
    return got > 0;
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
        if (rc == 0) {
            /* #148: EP0 survived — confirm the device still streams. */
            if (fuzz_streams_ok(h)) {
                printf("PASS protocol_fuzz: %llu ops, %u resets, %u/%u health "
                       "checks ok (device streams)\n",
                       (unsigned long long)log.seq, log.resets,
                       log.health_checks - log.health_fails, log.health_checks);
            } else {
                printf("WARN protocol_fuzz: %llu ops, EP0 healthy but NOT "
                       "streaming — Si5351 likely reconfigured by I2CWFX3 fuzz; "
                       "device needs a reload before streaming.\n",
                       (unsigned long long)log.seq);
                rc = 2;   /* caller reload-verifies if -F is available */
            }
        }
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
/* dir_mismatch — #142 isolation: well-formed vendor requests with ONLY*/
/* the data-phase direction bit flipped.  Everything else (bRequest,   */
/* wValue, wIndex, wLength, payload) is valid for the command, so if    */
/* this alone wedges EP0 it isolates wrong-direction as the cause       */
/* (independent of oversize / bad-I2C / unknown-code malformations).    */
/* Build-free falsifier for the #142 hypothesis — run before flashing   */
/* the firmware direction guard.                                        */
/* ------------------------------------------------------------------ */

/* Well-formed templates: correct direction + plausible args per command.
 * dir_in = the command's CORRECT direction (we send the OPPOSITE). */
struct dir_tmpl { uint8_t code; int dir_in; uint16_t wValue, wIndex, wLength; uint32_t payload; };
static const struct dir_tmpl DIR_TMPL[] = {
    { TESTFX3,       1, 0,    0,         4,            0 },
    { GETSTATS,      1, 0,    0,         GETSTATS_LEN, 0 },
    { I2CRFX3,       1, 0xC0, 0,         1,            0 },
    { READINFODEBUG, 1, 0,    0,         64,           0 },
    { GPIOFX3,       0, 0,    0,         4,            0x0820 },     /* LED_BLUE|SHDWN = idle */
    { STARTADC,      0, 0,    0,         4,            32000000 },
    { I2CWFX3,       0, 0xC0, 0,         1,            0 },
    { SETARGFX3,     0, 0,    DAT31_ATT, 1,            0 },
    { STARTFX3,      0, 0,    0,         4,            0 },
    { STOPFX3,       0, 0,    0,         4,            0 },
};
#define NDIR_TMPL ((int)(sizeof(DIR_TMPL) / sizeof(DIR_TMPL[0])))

static void dir_mismatch_step(libusb_device_handle *h, struct fuzz_rng *rng,
                              struct fuzz_log *log)
{
    const struct dir_tmpl *t = &DIR_TMPL[fuzz_below(rng, NDIR_TMPL)];

    /* Flip the direction: an IN command sent OUT, an OUT command sent IN. */
    uint8_t flipped = t->dir_in ? LIBUSB_ENDPOINT_OUT : LIBUSB_ENDPOINT_IN;
    uint8_t bmrt = flipped | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE;

    static uint8_t buf[64];
    uint16_t plen = t->wLength <= sizeof(buf) ? t->wLength : (uint16_t)sizeof(buf);
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &t->payload, plen < 4 ? plen : 4);

    int rc = libusb_control_transfer(h, bmrt, t->code, t->wValue, t->wIndex,
                                     buf, plen, 250);
    struct fuzz_op op = { bmrt, t->code, t->wValue, t->wIndex, t->wLength, plen, rc };
    fuzz_record(log, &op, /*streaming=*/0);
}

int fuzz_dir_mismatch(libusb_device_handle **h_inout, long num_ops, uint64_t seed)
{
    libusb_device_handle *h = *h_inout;
    if (num_ops <= 0) num_ops = 2000;
    seed = fuzz_seed_or_time(seed);
    struct fuzz_rng rng = { seed };
    struct fuzz_log log; fuzz_log_init(&log);

    fuzz_stop = 0;
    signal(SIGINT, fuzz_sigint);
    printf("=== DIR_MISMATCH === ops=%ld seed=0x%016llx (well-formed, wrong direction only)\n",
           num_ops, (unsigned long long)seed);

    uint32_t boot = 0;
    if (fuzz_gate(&h, &log, &boot, /*quiet=*/0) != 0) {
        printf("FAIL dir_mismatch: device unhealthy before start\n");
        *h_inout = h;
        return 1;
    }

    int rc = 0;
    for (long i = 0; i < num_ops && !fuzz_stop; i++) {
        dir_mismatch_step(h, &rng, &log);
        if ((i + 1) % 64 == 0) {
            int hc = fuzz_gate(&h, &log, &boot, /*quiet=*/0);
            if (hc != 0) {
                printf("FAIL dir_mismatch: health gate failed at op %ld (%s) "
                       "— wrong-direction requests alone wedged EP0 (#142 confirmed)\n",
                       i + 1, hc == FUZZ_HWCONFIG_BAD ? "hwconfig changed"
                                                      : libusb_strerror(hc));
                fuzz_dump(&log, seed, "dir_mismatch", h);
                rc = 1;
                break;
            }
        }
    }

    if (h) cmd_u32(h, STOPFX3, 0);
    signal(SIGINT, SIG_DFL);
    fuzz_coverage_report(&log);
    if (rc == 0) {
        /* #148: dir_mismatch STALLs all wrong-direction I2CWFX3 (no Si5351
         * write lands), so a healthy device should still stream — a strong
         * signal here. */
        if (fuzz_streams_ok(h)) {
            printf("PASS dir_mismatch: %llu wrong-direction ops, device healthy "
                   "and streaming (direction mismatch did NOT wedge EP0)\n",
                   (unsigned long long)log.seq);
        } else {
            printf("WARN dir_mismatch: %llu ops, EP0 healthy but NOT streaming — "
                   "unexpected (dir_mismatch shouldn't reconfigure the Si5351); "
                   "needs reload-verify.\n", (unsigned long long)log.seq);
            rc = 2;
        }
    }
    *h_inout = h;
    return rc;
}

/* ------------------------------------------------------------------ */
/* ep0_sweep — deterministic exhaustive bRequest x direction (#149)   */
/* ------------------------------------------------------------------ */

/* Correct data-phase direction for a known command: 1=IN, 0=OUT, -1=unknown.
 * Reuses the DIR_TMPL classification. */
static int known_dir(uint8_t code)
{
    for (int i = 0; i < NDIR_TMPL; i++)
        if (DIR_TMPL[i].code == code) return DIR_TMPL[i].dir_in;
    return -1;
}

/* Deterministically send every bRequest 0..255 in BOTH directions (IN/OUT)
 * and assert the #142 invariant across the whole space: no wrong-direction or
 * unknown request is accepted, and the device survives the entire sweep
 * (no wedge, no reset).  Unlike the seed-sampled protocol_fuzz/dir_mismatch,
 * this is a provable, repeatable pass over the full small space.
 *
 * Destructive: sending real OUT commands runs their side effects (STARTADC,
 * GPIOFX3, ...), so it mutates clock/GPIO state — reload before streaming
 * afterward.  Skips RESETFX3/HANG* (they reset/hang the device).  wLength=64
 * (<= EP0 max, no oversize STALL) so IN handlers have room and OUT handlers
 * get a full data phase. */
int fuzz_ep0_sweep(libusb_device_handle **h_inout)
{
    libusb_device_handle *h = *h_inout;
    struct fuzz_log log; fuzz_log_init(&log);
    uint32_t boot = 0;

    fuzz_stop = 0;
    signal(SIGINT, fuzz_sigint);
    printf("=== EP0_SWEEP === bRequest 0..255 x {OUT,IN}, wLength=64 "
           "(deterministic; destructive — mutates clock/GPIO)\n");

    if (fuzz_gate(&h, &log, &boot, /*quiet=*/0) != 0) {
        printf("FAIL ep0_sweep: device unhealthy before start\n");
        signal(SIGINT, SIG_DFL);
        *h_inout = h;
        return 1;
    }

    static uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    int unexpected = 0;
    int first_bad = -1;
    int rc = 0;

    for (int code = 0; code < 256 && !fuzz_stop; code++) {
        if (code == RESETFX3 || code == HANGFX3 ||
            code == HANGMAIN || code == HANGCOLDSTART)
            continue;   /* destructive — would reset/hang the device */

        for (int in = 0; in < 2; in++) {
            uint8_t bmrt = (in ? LIBUSB_ENDPOINT_IN : LIBUSB_ENDPOINT_OUT)
                         | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE;
            int r = libusb_control_transfer(h, bmrt, (uint8_t)code, 0, 0,
                                            buf, (uint16_t)sizeof(buf), 250);
            struct fuzz_op op = { bmrt, (uint8_t)code, 0, 0,
                                  (uint16_t)sizeof(buf), (uint16_t)sizeof(buf), r };
            fuzz_record(&log, &op, 0);

            /* #142 invariant: a wrong-direction or unknown request must NOT be
             * accepted.  expected_accept = known command in its correct
             * direction (it may still legitimately STALL on these zero args). */
            int kd = known_dir((uint8_t)code);
            int expected_accept = (kd >= 0 && in == kd);
            if (r >= 0 && !expected_accept) {
                unexpected++;
                if (first_bad < 0) first_bad = (code << 1) | in;
            }
        }

        if ((code & 0x1f) == 0x1f) {   /* health-gate (and STOP) every 32 codes */
            cmd_u32(h, STOPFX3, 0);
            if (fuzz_gate(&h, &log, &boot, /*quiet=*/0) != 0) {
                printf("FAIL ep0_sweep: device wedged after bRequest 0x%02X\n", code);
                fuzz_dump(&log, 0, "ep0_sweep", h);
                rc = 1;
                break;
            }
        }
    }

    if (rc == 0) {
        cmd_u32(h, STOPFX3, 0);
        if (fuzz_gate(&h, &log, &boot, /*quiet=*/0) != 0) {
            printf("FAIL ep0_sweep: device wedged after the sweep\n");
            fuzz_dump(&log, 0, "ep0_sweep", h);
            rc = 1;
        }
    }

    signal(SIGINT, SIG_DFL);
    fuzz_coverage_report(&log);

    if (rc == 0 && unexpected > 0) {
        printf("FAIL ep0_sweep: %d wrong-direction/unknown request(s) ACCEPTED "
               "(first: bReq=0x%02X %s) — direction-guard gap (#142)\n",
               unexpected, first_bad >> 1, (first_bad & 1) ? "IN" : "OUT");
        rc = 1;
    } else if (rc == 0 && log.resets > 0) {
        /* fuzz_gate counts (and re-acquires through) re-enumerations — a
         * boot_count change in fuzz_probe or a NO_DEVICE reacquire — without
         * failing.  The sweep's invariant is that NO (bRequest,direction)
         * resets the device, so enforce it explicitly here; otherwise a
         * reset-inducing request could still print "boot steady". */
        printf("FAIL ep0_sweep: device reset %u time(s) during the sweep — a "
               "(bRequest,direction) request triggered a re-enumeration\n",
               log.resets);
        rc = 1;
    } else if (rc == 0) {
        printf("PASS ep0_sweep: %llu requests (all bRequest x IN/OUT), no "
               "wrong-direction/unknown accepted, no resets, device healthy. "
               "NOTE: clock/GPIO mutated — reload before streaming.\n",
               (unsigned long long)log.seq);
    }
    *h_inout = h;
    return rc;
}

/* ------------------------------------------------------------------ */
/* i2c_fuzz — #154 isolation: malformed I2C only (correct direction)  */
/* ------------------------------------------------------------------ */

/* Fire only I2CWFX3 (OUT) / I2CRFX3 (IN) with the CORRECT direction but a
 * random I2C address (wValue), register (wIndex), length, and payload — the
 * one thing protocol_fuzz does that dir_mismatch/ep0_sweep don't.  If this
 * alone wedges EP0, it isolates the I2C path as the #154 vector (a malformed
 * multi-byte transfer hanging I2cTransfer, which runs inside the EP0 handler).
 * The host bias to address 0xC0 (the Si5351) matches protocol_fuzz so this
 * reproduces the conditions that wedged at op ~2432.
 *
 * Note: this scribbles the Si5351 — a PASS means "I2C abuse did not wedge EP0"
 * (the clock is still likely corrupted; reload before streaming).  A FAIL is
 * the #154 wedge reproduced in isolation. */
static void i2c_fuzz_step(libusb_device_handle *h, struct fuzz_rng *rng,
                          struct fuzz_log *log)
{
    int is_read = fuzz_below(rng, 2);
    uint8_t code = is_read ? I2CRFX3 : I2CWFX3;
    uint8_t dir  = is_read ? LIBUSB_ENDPOINT_IN : LIBUSB_ENDPOINT_OUT; /* correct */
    uint8_t bmrt = dir | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE;

    /* wValue = I2C device address, wIndex = register (firmware:
     * I2cTransfer(wIndex, wValue, wLength, ...)).  35% snap to the Si5351. */
    uint16_t addr = (fuzz_below(rng, 100) < 35) ? 0x00C0
                                                : (uint16_t)fuzz_below(rng, 0x10000);
    uint16_t reg  = (uint16_t)fuzz_below(rng, 0x10000);

    /* Length <= 64 (no oversize — keep it past the wLength>64 STALL so
     * I2cTransfer actually runs), weighted toward small + multi-byte. */
    uint16_t wLength;
    uint32_t wsel = fuzz_below(rng, 100);
    if      (wsel < 20) wLength = 0;
    else if (wsel < 50) wLength = 1;
    else if (wsel < 80) wLength = (uint16_t)(2 + fuzz_below(rng, 7));   /* 2..8 */
    else                wLength = (uint16_t)(1 + fuzz_below(rng, 64));  /* 1..64 */

    static uint8_t buf[64];
    uint16_t plen = wLength <= sizeof(buf) ? wLength : (uint16_t)sizeof(buf);
    for (uint16_t i = 0; i < plen; i++) buf[i] = (uint8_t)fuzz_next(rng);

    int rc = libusb_control_transfer(h, bmrt, code, addr, reg, buf, plen, 250);
    struct fuzz_op op = { bmrt, code, addr, reg, wLength, plen, rc };
    fuzz_record(log, &op, 0);
}

int fuzz_i2c(libusb_device_handle **h_inout, long num_ops, uint64_t seed)
{
    libusb_device_handle *h = *h_inout;
    if (num_ops <= 0) num_ops = 5000;
    seed = fuzz_seed_or_time(seed);
    struct fuzz_rng rng = { seed };
    struct fuzz_log log; fuzz_log_init(&log);

    fuzz_stop = 0;
    signal(SIGINT, fuzz_sigint);
    printf("=== I2C_FUZZ === ops=%ld seed=0x%016llx "
           "(I2CWFX3/I2CRFX3 only, correct direction, random addr/reg/len)\n",
           num_ops, (unsigned long long)seed);

    uint32_t boot = 0;
    if (fuzz_gate(&h, &log, &boot, /*quiet=*/0) != 0) {
        printf("FAIL i2c_fuzz: device unhealthy before start\n");
        signal(SIGINT, SIG_DFL);
        *h_inout = h;
        return 1;
    }

    int rc = 0;
    for (long i = 0; i < num_ops && !fuzz_stop; i++) {
        i2c_fuzz_step(h, &rng, &log);
        if ((i + 1) % 64 == 0) {
            int hc = fuzz_gate(&h, &log, &boot, /*quiet=*/0);
            if (hc != 0) {
                printf("FAIL i2c_fuzz: health gate failed at op %ld (%s) — "
                       "malformed I2C alone wedged EP0 (#154 confirmed)\n",
                       i + 1, hc == FUZZ_HWCONFIG_BAD ? "hwconfig changed"
                                                      : libusb_strerror(hc));
                fuzz_dump(&log, seed, "i2c_fuzz", h);
                rc = 1;
                break;
            }
        }
    }

    if (h) cmd_u32(h, STOPFX3, 0);
    signal(SIGINT, SIG_DFL);
    fuzz_coverage_report(&log);
    if (rc == 0)
        printf("PASS i2c_fuzz: %llu I2C ops, EP0 survived (#154 not reproduced "
               "by I2C alone). NOTE: Si5351 likely reconfigured — reload before "
               "streaming.\n", (unsigned long long)log.seq);
    *h_inout = h;
    return rc;
}

/* ------------------------------------------------------------------ */
/* oversend_fuzz — #154 isolation: short-wLength on fixed-size IN      */
/* responders (GETSTATS=30, TESTFX3=4, READINFODEBUG=len+1) that ignore*/
/* wLength and over-send when the host requests fewer bytes.           */
/* ------------------------------------------------------------------ */
int fuzz_oversend(libusb_device_handle **h_inout, long num_ops, uint64_t seed)
{
    libusb_device_handle *h = *h_inout;
    if (num_ops <= 0) num_ops = 5000;
    seed = fuzz_seed_or_time(seed);
    struct fuzz_rng rng = { seed };
    struct fuzz_log log; fuzz_log_init(&log);

    fuzz_stop = 0;
    signal(SIGINT, fuzz_sigint);
    printf("=== OVERSEND_FUZZ === ops=%ld seed=0x%016llx "
           "(GETSTATS/TESTFX3/READINFODEBUG, correct IN dir, wLength < payload)\n",
           num_ops, (unsigned long long)seed);

    /* Enable debug mode so READINFODEBUG has output queued (len+1 > 1). */
    { uint8_t info[4]; ctrl_read(h, TESTFX3, 1, 0, info, 4); }

    uint32_t boot = 0;
    if (fuzz_gate(&h, &log, &boot, /*quiet=*/0) != 0) {
        printf("FAIL oversend_fuzz: device unhealthy before start\n");
        signal(SIGINT, SIG_DFL);
        *h_inout = h;
        return 1;
    }

    /* The fixed-size over-senders (I2CRFX3 is excluded — it honours wLength). */
    static const uint8_t responders[] = { GETSTATS, TESTFX3, READINFODEBUG };

    int rc = 0;
    static uint8_t buf[64];
    for (long i = 0; i < num_ops && !fuzz_stop; i++) {
        uint8_t code = responders[fuzz_below(&rng, 3)];
        /* wLength 0..3 — below TESTFX3's 4 and far below GETSTATS's 30, so the
         * handler's fixed SendEP0Data over-sends past what the host asked. */
        uint16_t wLength = (uint16_t)fuzz_below(&rng, 4);
        uint8_t bmrt = LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR
                     | LIBUSB_RECIPIENT_DEVICE;
        int r = libusb_control_transfer(h, bmrt, code, 0, 0, buf, wLength, 250);
        struct fuzz_op op = { bmrt, code, 0, 0, wLength, wLength, r };
        fuzz_record(&log, &op, 0);

        if ((i + 1) % 64 == 0) {
            int hc = fuzz_gate(&h, &log, &boot, /*quiet=*/0);
            if (hc != 0) {
                printf("FAIL oversend_fuzz: health gate failed at op %ld (%s) — "
                       "short-wLength over-send alone wedged EP0 (#154 confirmed)\n",
                       i + 1, hc == FUZZ_HWCONFIG_BAD ? "hwconfig changed"
                                                      : libusb_strerror(hc));
                fuzz_dump(&log, seed, "oversend_fuzz", h);
                rc = 1;
                break;
            }
        }
    }

    if (h) cmd_u32(h, STOPFX3, 0);
    signal(SIGINT, SIG_DFL);
    fuzz_coverage_report(&log);
    if (rc == 0)
        printf("PASS oversend_fuzz: %llu over-send ops, EP0 survived "
               "(#154 not reproduced by over-send alone)\n",
               (unsigned long long)log.seq);
    *h_inout = h;
    return rc;
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
        if (rc == 0) {
            /* #148: confirm the device still streams after the run. */
            if (fuzz_streams_ok(h)) {
                printf("PASS stream_fuzz: device survived %.1fs of stream fuzzing "
                       "(still streams)\n", seconds);
            } else {
                printf("WARN stream_fuzz: survived %.1fs but EP0-healthy and NOT "
                       "streaming — needs reload-verify.\n", seconds);
                rc = 2;
            }
        }
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
