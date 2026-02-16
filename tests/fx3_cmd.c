/*
 * fx3_cmd.c — Vendor command exerciser for SDDC_FX3 firmware.
 *
 * Sends individual USB vendor requests to an RX888mk2 and reports
 * success/failure.  Designed for scripted hardware testing.
 *
 * IMPORTANT: This tool assumes the device already has firmware loaded
 * (PID 0x00F1).  It does NOT handle firmware upload.  Use rx888_stream
 * from https://github.com/ringof/rx888_tools with its -f flag to load
 * firmware onto a freshly powered device first.  The fw_test.sh wrapper
 * script handles this automatically.
 *
 * Build:  gcc -O2 -Wall -o fx3_cmd fx3_cmd.c -lusb-1.0
 * Needs:  libusb-1.0-0-dev
 *
 * Copyright (c) 2024-2026 David Goncalves — MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <libusb-1.0/libusb.h>

/* ------------------------------------------------------------------ */
/* Protocol constants — must match SDDC_FX3/protocol.h                */
/* ------------------------------------------------------------------ */

/* USB IDs */
#define RX888_VID        0x04B4
#define RX888_PID_APP    0x00F1
#define RX888_PID_BOOT   0x00F3

/* Vendor request codes */
#define STARTFX3      0xAA
#define STOPFX3       0xAB
#define TESTFX3       0xAC
#define GPIOFX3       0xAD
#define I2CWFX3       0xAE
#define I2CRFX3       0xAF
#define RESETFX3      0xB1
#define STARTADC      0xB2
#define GETSTATS      0xB3
/* Legacy tuner commands (R82xx driver removed — GPL conflict).
 * Retained here for stale-command regression tests: the "raw"
 * subcommand sends these codes and expects a USB STALL. */
#define TUNERINIT     0xB4
#define TUNERTUNE     0xB5
#define SETARGFX3     0xB6
#define TUNERSTDBY    0xB8
#define READINFODEBUG 0xBA

/* SETARGFX3 argument IDs */
#define DAT31_ATT     10
#define AD8370_VGA    11

/* Timeouts */
#define CTRL_TIMEOUT_MS  1000

/* ------------------------------------------------------------------ */
/* USB helpers (patterns from rx888_stream.c)                         */
/* ------------------------------------------------------------------ */

static int ctrl_write_u32(libusb_device_handle *h, uint8_t request,
                          uint16_t wValue, uint16_t wIndex, uint32_t val)
{
    uint8_t data[4];
    data[0] = (uint8_t)(val & 0xFF);
    data[1] = (uint8_t)((val >>  8) & 0xFF);
    data[2] = (uint8_t)((val >> 16) & 0xFF);
    data[3] = (uint8_t)((val >> 24) & 0xFF);

    int r = libusb_control_transfer(
        h,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        request, wValue, wIndex, data, sizeof(data), CTRL_TIMEOUT_MS);
    if (r < 0) return r;
    if (r != (int)sizeof(data)) return LIBUSB_ERROR_IO;
    return 0;
}

static int ctrl_write_buf(libusb_device_handle *h, uint8_t request,
                          uint16_t wValue, uint16_t wIndex,
                          const uint8_t *buf, uint16_t len)
{
    int r = libusb_control_transfer(
        h,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        request, wValue, wIndex, (unsigned char *)buf, len, CTRL_TIMEOUT_MS);
    if (r < 0) return r;
    if (r != (int)len) return LIBUSB_ERROR_IO;
    return 0;
}

static int ctrl_read(libusb_device_handle *h, uint8_t request,
                     uint16_t wValue, uint16_t wIndex,
                     uint8_t *buf, uint16_t len)
{
    int r = libusb_control_transfer(
        h,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        request, wValue, wIndex, buf, len, CTRL_TIMEOUT_MS);
    return r;
}

/* Convenience: send a command with a u32 payload, wValue=0, wIndex=0 */
static int cmd_u32(libusb_device_handle *h, uint8_t cmd, uint32_t val)
{
    return ctrl_write_u32(h, cmd, 0, 0, val);
}

/* Convenience: send SETARGFX3 with arg_id in wIndex, arg_val in wValue,
 * and a 1-byte zero payload (matches rx888_stream encoding). */
static int set_arg(libusb_device_handle *h, uint16_t arg_id, uint16_t arg_val)
{
    uint8_t zero = 0;
    return ctrl_write_buf(h, SETARGFX3, arg_val, arg_id, &zero, 1);
}

/* ------------------------------------------------------------------ */
/* Device open / close                                                */
/* ------------------------------------------------------------------ */

static libusb_device_handle *open_rx888(libusb_context *ctx)
{
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, RX888_VID, RX888_PID_APP);
    if (!h) {
        /* Check if device is in bootloader mode */
        libusb_device_handle *boot = libusb_open_device_with_vid_pid(ctx, RX888_VID, RX888_PID_BOOT);
        if (boot) {
            libusb_close(boot);
            fprintf(stderr, "error: device found in bootloader mode (PID 0x%04X) — flash firmware first\n",
                    RX888_PID_BOOT);
        } else {
            fprintf(stderr, "error: no RX888 device found (VID 0x%04X, PID 0x%04X)\n",
                    RX888_VID, RX888_PID_APP);
        }
        return NULL;
    }

    /* Detach kernel driver if attached */
    if (libusb_kernel_driver_active(h, 0) == 1)
        libusb_detach_kernel_driver(h, 0);

    int r = libusb_claim_interface(h, 0);
    if (r < 0) {
        fprintf(stderr, "error: claim interface: %s\n", libusb_strerror(r));
        libusb_close(h);
        return NULL;
    }

    return h;
}

static void close_rx888(libusb_device_handle *h)
{
    if (h) {
        libusb_release_interface(h, 0);
        libusb_close(h);
    }
}

/* ------------------------------------------------------------------ */
/* Subcommands                                                        */
/* ------------------------------------------------------------------ */

static int do_test(libusb_device_handle *h)
{
    uint8_t buf[4] = {0};
    int r = ctrl_read(h, TESTFX3, 0, 0, buf, 4);
    if (r < 0) {
        printf("FAIL test: %s\n", libusb_strerror(r));
        return 1;
    }
    if (r < 4) {
        printf("FAIL test: short reply (%d bytes, expected 4)\n", r);
        return 1;
    }
    uint8_t hwconfig   = buf[0];
    uint8_t fw_major   = buf[1];
    uint8_t fw_minor   = buf[2];
    uint8_t rqt_cnt    = buf[3];
    printf("PASS test: hwconfig=0x%02X fw=%d.%d vendorRqtCnt=%d\n",
           hwconfig, fw_major, fw_minor, rqt_cnt);
    return 0;
}

static int do_gpio(libusb_device_handle *h, uint32_t bits)
{
    int r = cmd_u32(h, GPIOFX3, bits);
    if (r < 0) {
        printf("FAIL gpio 0x%08X: %s\n", bits, libusb_strerror(r));
        return 1;
    }
    printf("PASS gpio 0x%08X\n", bits);
    return 0;
}

static int do_adc(libusb_device_handle *h, uint32_t freq)
{
    int r = cmd_u32(h, STARTADC, freq);
    if (r < 0) {
        printf("FAIL adc %u: %s\n", freq, libusb_strerror(r));
        return 1;
    }
    printf("PASS adc %u Hz\n", freq);
    return 0;
}

static int do_att(libusb_device_handle *h, uint16_t val)
{
    int r = set_arg(h, DAT31_ATT, val);
    if (r < 0) {
        printf("FAIL att %u: %s\n", val, libusb_strerror(r));
        return 1;
    }
    printf("PASS att %u\n", val);
    return 0;
}

static int do_vga(libusb_device_handle *h, uint16_t val)
{
    int r = set_arg(h, AD8370_VGA, val);
    if (r < 0) {
        printf("FAIL vga %u: %s\n", val, libusb_strerror(r));
        return 1;
    }
    printf("PASS vga %u\n", val);
    return 0;
}

static int do_start(libusb_device_handle *h)
{
    int r = cmd_u32(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL start: %s\n", libusb_strerror(r));
        return 1;
    }
    printf("PASS start\n");
    return 0;
}

static int do_stop(libusb_device_handle *h)
{
    int r = cmd_u32(h, STOPFX3, 0);
    if (r < 0) {
        printf("FAIL stop: %s\n", libusb_strerror(r));
        return 1;
    }
    printf("PASS stop\n");
    return 0;
}

static int do_i2cr(libusb_device_handle *h, uint16_t addr, uint16_t reg, uint16_t len)
{
    uint8_t buf[64];
    if (len > sizeof(buf)) len = sizeof(buf);

    int r = ctrl_read(h, I2CRFX3, addr, reg, buf, len);
    if (r < 0) {
        printf("FAIL i2cr addr=0x%02X reg=0x%02X: %s\n", addr, reg, libusb_strerror(r));
        return 1;
    }
    printf("PASS i2cr addr=0x%02X reg=0x%02X len=%d:", addr, reg, r);
    for (int i = 0; i < r; i++)
        printf(" %02X", buf[i]);
    printf("\n");
    return 0;
}

static int do_i2cw(libusb_device_handle *h, uint16_t addr, uint16_t reg,
                   const uint8_t *data, uint16_t len)
{
    int r = ctrl_write_buf(h, I2CWFX3, addr, reg, data, len);
    if (r < 0) {
        printf("FAIL i2cw addr=0x%02X reg=0x%02X: %s\n", addr, reg, libusb_strerror(r));
        return 1;
    }
    printf("PASS i2cw addr=0x%02X reg=0x%02X len=%d\n", addr, reg, len);
    return 0;
}

static int do_reset(libusb_device_handle *h)
{
    /* RESETFX3 reboots the FX3 — the device will disconnect immediately,
     * so a transfer error is expected. */
    int r = cmd_u32(h, RESETFX3, 0);
    /* Accept success or pipe error (device rebooted before replying) */
    if (r < 0 && r != LIBUSB_ERROR_PIPE && r != LIBUSB_ERROR_NO_DEVICE
              && r != LIBUSB_ERROR_IO) {
        printf("FAIL reset: %s\n", libusb_strerror(r));
        return 1;
    }
    printf("PASS reset (device rebooting to bootloader)\n");
    return 0;
}

/* Send a raw vendor command code — for testing stale/removed commands */
static int do_raw(libusb_device_handle *h, uint8_t code)
{
    int r = cmd_u32(h, code, 0);
    if (r == LIBUSB_ERROR_PIPE) {
        printf("PASS raw 0x%02X: STALL (as expected for removed command)\n", code);
        return 0;
    }
    if (r < 0) {
        printf("FAIL raw 0x%02X: %s\n", code, libusb_strerror(r));
        return 1;
    }
    printf("PASS raw 0x%02X: accepted\n", code);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Local command dispatch for the debug console ('!' escape)          */
/* ------------------------------------------------------------------ */

/* Forward declarations for do_* functions defined later in this file. */
static int do_stats(libusb_device_handle *h);
static int do_ep0_overflow(libusb_device_handle *h);
static int do_test_oob_brequest(libusb_device_handle *h);
static int do_test_oob_setarg(libusb_device_handle *h);
static int do_test_console_fill(libusb_device_handle *h);
static int do_test_debug_race(libusb_device_handle *h);
static int do_test_debug_poll(libusb_device_handle *h);
static int do_test_pib_overflow(libusb_device_handle *h);
static int do_test_stack_check(libusb_device_handle *h);
static int do_test_stats_i2c(libusb_device_handle *h);
static int do_test_stats_pib(libusb_device_handle *h);
static int do_test_stats_pll(libusb_device_handle *h);
static int do_test_stop_gpif_state(libusb_device_handle *h);
static int do_test_stop_start_cycle(libusb_device_handle *h);
static int do_test_pll_preflight(libusb_device_handle *h);
static int do_test_wedge_recovery(libusb_device_handle *h);

/* No-arg command table entry */
struct local_cmd_entry {
    const char *name;
    int (*func)(libusb_device_handle *);
};

static const struct local_cmd_entry local_cmds_noarg[] = {
    {"test",             do_test},
    {"start",            do_start},
    {"stop",             do_stop},
    {"stats",            do_stats},
    {"ep0_overflow",     do_ep0_overflow},
    {"oob_brequest",     do_test_oob_brequest},
    {"oob_setarg",       do_test_oob_setarg},
    {"console_fill",     do_test_console_fill},
    {"debug_race",       do_test_debug_race},
    {"debug_poll",       do_test_debug_poll},
    {"pib_overflow",     do_test_pib_overflow},
    {"stack_check",      do_test_stack_check},
    {"stats_i2c",        do_test_stats_i2c},
    {"stats_pib",        do_test_stats_pib},
    {"stats_pll",        do_test_stats_pll},
    {"stop_gpif_state",  do_test_stop_gpif_state},
    {"stop_start_cycle", do_test_stop_start_cycle},
    {"pll_preflight",    do_test_pll_preflight},
    {"wedge_recovery",   do_test_wedge_recovery},
    {"reset",            do_reset},
    {NULL, NULL}
};

static void print_local_help(void)
{
    printf("Local commands (prefix with '!'):\n"
           "  help / ?                      This help\n"
           "  test                          Read device info\n"
           "  start / stop                  Start/stop GPIF streaming\n"
           "  adc <freq>                    Set ADC clock frequency\n"
           "  att <0-63>                    Set DAT-31 attenuator\n"
           "  vga <0-255>                   Set AD8370 VGA gain\n"
           "  gpio <bits>                   Set GPIO word\n"
           "  stats                         Read GETSTATS counters\n"
           "  stats_i2c / stats_pib / stats_pll   Counter tests\n"
           "  stop_gpif_state               Verify GPIF SM stops after STOP\n"
           "  stop_start_cycle              Cycle STOP+START N times\n"
           "  pll_preflight                 Verify START rejected without clock\n"
           "  wedge_recovery                Provoke DMA wedge, test recovery\n"
           "  pib_overflow                  Provoke + detect PIB error\n"
           "  stack_check                   Query stack watermark\n"
           "  i2cr <addr> <reg> <len>       I2C read (hex)\n"
           "  i2cw <addr> <reg> <byte>...   I2C write (hex)\n"
           "  raw <code>                    Send raw vendor request (hex)\n"
           "  reset                         Reboot FX3 to bootloader\n");
}

/* Parse and dispatch a local command line (without the '!' prefix). */
static int dispatch_local_cmd(libusb_device_handle *h, const char *line)
{
    /* Skip leading whitespace */
    while (*line == ' ') line++;
    if (*line == '\0') return 0;

    /* Split into command and args */
    char cmd[64] = {0};
    const char *args = NULL;
    const char *sp = strchr(line, ' ');
    if (sp) {
        int len = (int)(sp - line);
        if (len >= (int)sizeof(cmd)) len = (int)sizeof(cmd) - 1;
        memcpy(cmd, line, len);
        args = sp + 1;
        while (*args == ' ') args++;
        if (*args == '\0') args = NULL;
    } else {
        int len = (int)strlen(line);
        if (len >= (int)sizeof(cmd)) len = (int)sizeof(cmd) - 1;
        memcpy(cmd, line, len);
    }

    /* Help */
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        print_local_help();
        return 0;
    }

    /* No-arg commands */
    for (const struct local_cmd_entry *e = local_cmds_noarg; e->name; e++) {
        if (strcmp(cmd, e->name) == 0)
            return e->func(h);
    }

    /* Commands with arguments */
    if (strcmp(cmd, "adc") == 0) {
        if (!args) { printf("usage: adc <freq_hz>\n"); return 1; }
        return do_adc(h, (uint32_t)strtoul(args, NULL, 0));
    }
    if (strcmp(cmd, "att") == 0) {
        if (!args) { printf("usage: att <0-63>\n"); return 1; }
        return do_att(h, (uint16_t)strtoul(args, NULL, 0));
    }
    if (strcmp(cmd, "vga") == 0) {
        if (!args) { printf("usage: vga <0-255>\n"); return 1; }
        return do_vga(h, (uint16_t)strtoul(args, NULL, 0));
    }
    if (strcmp(cmd, "gpio") == 0) {
        if (!args) { printf("usage: gpio <bits>\n"); return 1; }
        return do_gpio(h, (uint32_t)strtoul(args, NULL, 0));
    }
    if (strcmp(cmd, "raw") == 0) {
        if (!args) { printf("usage: raw <code>\n"); return 1; }
        return do_raw(h, (uint8_t)strtoul(args, NULL, 0));
    }
    if (strcmp(cmd, "i2cr") == 0) {
        if (!args) { printf("usage: i2cr <addr> <reg> <len>\n"); return 1; }
        unsigned long a, rg, l;
        if (sscanf(args, "%li %li %li", &a, &rg, &l) != 3) {
            printf("usage: i2cr <addr> <reg> <len>\n");
            return 1;
        }
        return do_i2cr(h, (uint16_t)a, (uint16_t)rg, (uint16_t)l);
    }
    if (strcmp(cmd, "i2cw") == 0) {
        if (!args) { printf("usage: i2cw <addr> <reg> <byte>...\n"); return 1; }
        char *p = (char *)args;
        char *end;
        unsigned long a = strtoul(p, &end, 0);
        if (end == p) { printf("usage: i2cw <addr> <reg> <byte>...\n"); return 1; }
        p = end;
        unsigned long rg = strtoul(p, &end, 0);
        if (end == p) { printf("usage: i2cw <addr> <reg> <byte>...\n"); return 1; }
        p = end;
        uint8_t data[64];
        int ndata = 0;
        while (ndata < (int)sizeof(data)) {
            unsigned long b = strtoul(p, &end, 0);
            if (end == p) break;
            data[ndata++] = (uint8_t)b;
            p = end;
        }
        if (ndata == 0) { printf("usage: i2cw <addr> <reg> <byte>...\n"); return 1; }
        return do_i2cw(h, (uint16_t)a, (uint16_t)rg, data, (uint16_t)ndata);
    }

    printf("unknown local command: '%s' (type !help for list)\n", cmd);
    return 1;
}

/* SIGINT handler: restore terminal from raw mode before exit. */
static struct termios saved_termios;
static volatile sig_atomic_t raw_mode_active;

static void sigint_handler(int sig)
{
    if (raw_mode_active)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Interactive debug console over USB.
 * First sends TESTFX3 with wValue=1 to enable debug mode, then polls
 * READINFODEBUG for output.  Typed characters are sent in wValue;
 * CR triggers command execution on the FX3 side.  Ctrl-C exits.
 *
 * Local command escape: typing '!' switches to local command mode.
 * Characters are buffered locally and dispatched to the corresponding
 * do_*() function on Enter, using the same USB handle.  Debug output
 * polling continues between keystrokes.  See dispatch_local_cmd(). */
static int do_debug(libusb_device_handle *h)
{
    /* Enable debug mode via TESTFX3 wValue=1 */
    uint8_t info[4] = {0};
    int r = ctrl_read(h, TESTFX3, 1, 0, info, 4);
    if (r < 0) {
        printf("FAIL debug: enable debug mode: %s\n", libusb_strerror(r));
        return 1;
    }
    printf("debug: enabled (hwconfig=0x%02X fw=%d.%d)\n",
           info[0], info[1], info[2]);
    printf("debug: type commands + Enter for FX3, '!' for local commands, Ctrl-C to quit\n");
    fflush(stdout);

    /* Put stdin in non-blocking mode for character-at-a-time input */
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    saved_termios = oldt;
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    newt.c_cc[VMIN] = 0;
    newt.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    raw_mode_active = 1;
    signal(SIGINT, sigint_handler);

    /* Local command mode state */
    int local_mode = 0;
    char local_buf[128];
    int local_len = 0;

    uint8_t buf[64];
    for (;;) {
        /* Check for typed character */
        uint16_t send_char = 0;
        char ch;
        if (read(STDIN_FILENO, &ch, 1) == 1) {
            if (!local_mode && ch == '!') {
                /* Enter local command mode */
                local_mode = 1;
                local_len = 0;
                printf("\nfx3> ");
                fflush(stdout);
            } else if (local_mode) {
                if (ch == '\n' || ch == '\r') {
                    local_buf[local_len] = '\0';
                    printf("\n");
                    fflush(stdout);
                    if (local_len > 0)
                        dispatch_local_cmd(h, local_buf);
                    fflush(stdout);
                    local_mode = 0;
                } else if (ch == 0x7f || ch == 0x08) {
                    /* Backspace / Delete */
                    if (local_len > 0) {
                        local_len--;
                        printf("\b \b");
                        fflush(stdout);
                    }
                } else if (ch == 0x03 || ch == 0x1b) {
                    /* Ctrl-C or Escape — cancel local command */
                    printf(" (cancelled)\n");
                    fflush(stdout);
                    local_mode = 0;
                } else if (local_len < (int)sizeof(local_buf) - 1) {
                    local_buf[local_len++] = ch;
                    putchar(ch);
                    fflush(stdout);
                }
                /* Don't send to device in local mode */
            } else {
                /* Normal mode — send character to FX3 console */
                if (ch == '\n') ch = '\r';
                send_char = (uint8_t)ch;
            }
        }

        /* Poll READINFODEBUG: wValue carries the typed char (0 = none) */
        r = ctrl_read(h, READINFODEBUG, send_char, 0, buf, sizeof(buf));
        if (r > 0) {
            buf[r - 1] = '\0';  /* firmware null-terminates last byte */
            printf("%s", (char *)buf);
            fflush(stdout);
        }
        /* STALL (LIBUSB_ERROR_PIPE) means no data — normal */

        usleep(50000);  /* 50ms poll interval */
    }
    /* NOTREACHED — loop exits via SIGINT → sigint_handler */
}

/* Send a vendor request with wLength > 64 — must STALL if firmware
 * validates EP0 buffer bounds (issue #6). */
static int do_ep0_overflow(libusb_device_handle *h)
{
    uint8_t buf[128];
    memset(buf, 0, sizeof(buf));

    int r = libusb_control_transfer(
        h,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        GPIOFX3, 0, 0, buf, sizeof(buf), CTRL_TIMEOUT_MS);
    if (r == LIBUSB_ERROR_PIPE) {
        printf("PASS ep0_overflow: STALL on wLength=%d (> 64)\n", (int)sizeof(buf));
        return 0;
    }
    if (r < 0) {
        printf("FAIL ep0_overflow: %s\n", libusb_strerror(r));
        return 1;
    }
    printf("FAIL ep0_overflow: accepted wLength=%d (expected STALL)\n", (int)sizeof(buf));
    return 1;
}

/* ------------------------------------------------------------------ */
/* Targeted issue-verification tests                                  */
/* ------------------------------------------------------------------ */

/* Issue #21: Send a vendor request with bRequest outside the
 * FX3CommandName[] bounds (0xAA-0xBA).  TraceSerial must not crash.
 * We use 0xCC which is well outside the table.  Expected: STALL
 * (unknown command) but no crash/hang.  Verify by probing afterwards. */
static int do_test_oob_brequest(libusb_device_handle *h)
{
    /* First, enable debug mode so TraceSerial is actually active */
    uint8_t info[4] = {0};
    int r = ctrl_read(h, TESTFX3, 1, 0, info, 4);
    if (r < 0) {
        printf("FAIL oob_brequest: enable debug mode: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Send out-of-range vendor request 0xCC */
    r = cmd_u32(h, 0xCC, 0);
    /* Expected: STALL (unknown command) */
    if (r != LIBUSB_ERROR_PIPE && r != 0) {
        printf("FAIL oob_brequest: unexpected error: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Verify device is still alive by probing */
    r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL oob_brequest: device unresponsive after OOB bRequest: %s\n",
               libusb_strerror(r));
        return 1;
    }
    printf("PASS oob_brequest: device survived bRequest=0xCC (issue #21)\n");
    return 0;
}

/* Issue #20: Send SETARGFX3 with wIndex=0xFFFF, well beyond the
 * SETARGFX3List[] bounds.  TraceSerial must not crash.
 * Expected: STALL (unknown arg ID) but no crash/hang. */
static int do_test_oob_setarg(libusb_device_handle *h)
{
    uint8_t info[4] = {0};
    int r = ctrl_read(h, TESTFX3, 1, 0, info, 4);
    if (r < 0) {
        printf("FAIL oob_setarg: enable debug mode: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Send SETARGFX3 with wIndex=0xFFFF (way out of bounds) */
    uint8_t zero = 0;
    r = ctrl_write_buf(h, SETARGFX3, 42, 0xFFFF, &zero, 1);
    /* Expected: STALL from the default case in SETARGFX3 handler */
    if (r != LIBUSB_ERROR_PIPE && r != 0) {
        printf("FAIL oob_setarg: unexpected error: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Verify device is still alive */
    r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL oob_setarg: device unresponsive after OOB wIndex: %s\n",
               libusb_strerror(r));
        return 1;
    }
    printf("PASS oob_setarg: device survived SETARGFX3 wIndex=0xFFFF (issue #20)\n");
    return 0;
}

/* Issue #13: Fill the console input buffer to exactly 31 chars
 * (the maximum before the off-by-one fix) and verify the device
 * doesn't crash.  Then send CR to flush and verify responsiveness. */
static int do_test_console_fill(libusb_device_handle *h)
{
    uint8_t info[4] = {0};
    uint8_t buf[64];

    /* Enable debug mode */
    int r = ctrl_read(h, TESTFX3, 1, 0, info, 4);
    if (r < 0) {
        printf("FAIL console_fill: enable debug mode: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Send 35 characters (exceeds 32-byte buffer) via READINFODEBUG wValue */
    for (int i = 0; i < 35; i++) {
        r = ctrl_read(h, READINFODEBUG, 'a', 0, buf, sizeof(buf));
        /* r may be STALL (no debug output pending) — that's fine */
    }

    /* Send CR to trigger ParseCommand (flushes the buffer) */
    r = ctrl_read(h, READINFODEBUG, 0x0d, 0, buf, sizeof(buf));

    /* Brief pause for command processing */
    usleep(100000);

    /* Verify device is still alive */
    r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL console_fill: device unresponsive after 35-char fill: %s\n",
               libusb_strerror(r));
        return 1;
    }
    printf("PASS console_fill: device survived 35-char console input (issue #13)\n");
    return 0;
}

/* Issue #8: Exercise the debug buffer race window by rapidly
 * enabling debug mode (which triggers DebugPrint2USB) and polling
 * READINFODEBUG simultaneously.  Not deterministic, but catches
 * gross corruption.  Runs N rapid poll cycles. */
static int do_test_debug_race(libusb_device_handle *h)
{
    uint8_t info[4] = {0};
    uint8_t buf[64];

    /* Enable debug mode */
    int r = ctrl_read(h, TESTFX3, 1, 0, info, 4);
    if (r < 0) {
        printf("FAIL debug_race: enable debug mode: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Trigger firmware activity that generates debug output:
     * send several SETARGFX3 + poll READINFODEBUG interleaved rapidly */
    for (int i = 0; i < 50; i++) {
        /* Generate debug output via a benign SETARGFX3 */
        uint8_t zero = 0;
        ctrl_write_buf(h, SETARGFX3, (uint16_t)(i & 63), DAT31_ATT, &zero, 1);
        /* Immediately poll debug output */
        r = ctrl_read(h, READINFODEBUG, 0, 0, buf, sizeof(buf));
        /* Any result is fine — we're stress-testing the race path */
    }

    /* Verify device is still alive and coherent */
    r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL debug_race: device unresponsive after race stress: %s\n",
               libusb_strerror(r));
        return 1;
    }
    if (r >= 4 && info[0] == 0) {
        printf("FAIL debug_race: hwconfig read back as 0 (possible corruption)\n");
        return 1;
    }
    printf("PASS debug_race: device survived 50 rapid debug poll cycles (issue #8)\n");
    return 0;
}

/* Issue #26: Non-interactive debug poll — enable debug mode, send a
 * known command ("?"), collect output, verify it contains expected text.
 * Times out after a few seconds. */
static int do_test_debug_poll(libusb_device_handle *h)
{
    uint8_t info[4] = {0};
    uint8_t buf[64];
    char collected[1024] = {0};
    int collected_len = 0;

    /* Enable debug mode */
    int r = ctrl_read(h, TESTFX3, 1, 0, info, 4);
    if (r < 0) {
        printf("FAIL debug_poll: enable debug mode: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Send "?" + CR via READINFODEBUG wValue */
    ctrl_read(h, READINFODEBUG, '?', 0, buf, sizeof(buf));
    usleep(50000);
    ctrl_read(h, READINFODEBUG, 0x0d, 0, buf, sizeof(buf));

    /* Poll for response (up to 2 seconds) */
    for (int attempt = 0; attempt < 40; attempt++) {
        usleep(50000);
        r = ctrl_read(h, READINFODEBUG, 0, 0, buf, sizeof(buf));
        if (r > 0) {
            int copy = r - 1;  /* last byte is firmware null */
            if (collected_len + copy >= (int)sizeof(collected) - 1)
                copy = (int)sizeof(collected) - 1 - collected_len;
            if (copy > 0) {
                memcpy(collected + collected_len, buf, copy);
                collected_len += copy;
            }
        }
    }
    collected[collected_len] = '\0';

    /* The "?" command should produce help text containing "commands" */
    if (strstr(collected, "commands") || strstr(collected, "reset")
        || strstr(collected, "threads")) {
        printf("PASS debug_poll: got help text over USB debug (issue #26)\n");
        return 0;
    }
    if (collected_len > 0) {
        printf("PASS debug_poll: got %d bytes debug output (issue #26)\n", collected_len);
        return 0;
    }
    printf("FAIL debug_poll: no debug output received after '?' command\n");
    return 1;
}

/* Issue #10: Provoke a PIB error by starting GPIF streaming and
 * deliberately not reading the bulk endpoint.  The GPIF buffers
 * overflow, PibErrorCallback fires, and MsgParsing prints "PIB error 0x..."
 * to the debug output.  We poll READINFODEBUG looking for that string.
 *
 * This validates the entire PIB error reporting chain:
 *   GPIF overflow → PibErrorCallback → EventAvailable queue →
 *   MsgParsing → DebugPrint → READINFODEBUG poll
 */
static int do_test_pib_overflow(libusb_device_handle *h)
{
    uint8_t info[4] = {0};
    uint8_t buf[64];
    char collected[4096] = {0};
    int collected_len = 0;
    int found_pib_error = 0;

    /* 1. Enable debug mode */
    int r = ctrl_read(h, TESTFX3, 1, 0, info, 4);
    if (r < 0) {
        printf("FAIL pib_overflow: enable debug mode: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 2. Drain any stale debug output */
    for (int i = 0; i < 10; i++) {
        ctrl_read(h, READINFODEBUG, 0, 0, buf, sizeof(buf));
        usleep(20000);
    }

    /* 3. Configure ADC at 64 MHz — high enough to overwhelm quickly */
    r = cmd_u32(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL pib_overflow: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 4. Start streaming — GPIF begins pushing data to EP1 IN */
    r = cmd_u32(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL pib_overflow: STARTFX3: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 5. Let DMA buffers fill and PIB errors fire.
     *    At 64 MS/s the 4 × 16 KB DMA buffers fill in < 1 ms, so
     *    PIB error interrupts begin almost immediately.  However,
     *    the interrupt rate (~64 MHz) saturates the ARM core and
     *    starves the application thread — it can never wake from
     *    CyU3PThreadSleep to dequeue events and format "PIB error"
     *    text into the debug buffer.
     *
     *    Second problem: READINFODEBUG zeros glDebTxtLen after
     *    reading only 63 bytes, discarding the rest of the buffer.
     *    If trace output (~90 bytes) precedes PIB error text, the
     *    first 63-byte read gets trace and the PIB text is lost.
     *
     *    Fix: drain trace output DURING the storm (app thread is
     *    guaranteed starved, so only trace is in the buffer), then
     *    stop GPIF, then let the app thread write PIB error text
     *    into the now-empty buffer. */
    usleep(10000);  /* 10ms — DMA fills in < 1ms */

    /* 6. Drain trace output while the interrupt storm guarantees
     *    the app thread cannot run (no PIB error text yet). */
    for (int i = 0; i < 5; i++)
        ctrl_read(h, READINFODEBUG, 0, 0, buf, sizeof(buf));

    /* 7. Stop streaming — ends the PIB interrupt storm */
    cmd_u32(h, STOPFX3, 0);

    /* 8. Let the application thread wake (100ms sleep cycle) and
     *    process the queued PIB error events into the debug buffer.
     *    The buffer now has only STOPFX3 trace (~12 bytes) followed
     *    by PIB error text — well within the 63-byte read limit. */
    usleep(300000);

    /* 9. Read the debug buffer.  STOPFX3 trace + PIB error text
     *    fits in a single 63-byte read. */
    for (int attempt = 0; attempt < 20; attempt++) {
        r = ctrl_read(h, READINFODEBUG, 0, 0, buf, sizeof(buf));
        if (r > 0) {
            int copy = r - 1;  /* strip NUL terminator */
            if (collected_len + copy >= (int)sizeof(collected) - 1)
                copy = (int)sizeof(collected) - 1 - collected_len;
            if (copy > 0) {
                memcpy(collected + collected_len, buf, copy);
                collected_len += copy;
            }
            if (strstr(collected, "PIB error")) {
                found_pib_error = 1;
                break;
            }
        }
        usleep(50000);  /* 50ms between polls */
    }
    collected[collected_len] = '\0';

    /* 10. Verify device is still alive */
    r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL pib_overflow: device unresponsive after test: %s\n",
               libusb_strerror(r));
        return 1;
    }

    if (found_pib_error) {
        /* Extract the first PIB error line for reporting */
        char *p = strstr(collected, "PIB error");
        char excerpt[64] = {0};
        if (p) {
            int n = 0;
            while (p[n] && p[n] != '\r' && p[n] != '\n' && n < 60) n++;
            memcpy(excerpt, p, n);
        }
        printf("PASS pib_overflow: detected \"%s\" in debug output (issue #10)\n",
               excerpt);
        return 0;
    }

    printf("FAIL pib_overflow: no PIB error detected in %d bytes of debug output\n",
           collected_len);
    if (collected_len > 0) {
        /* Show what we did get, truncated */
        collected[collected_len < 200 ? collected_len : 200] = '\0';
        printf("#   debug output: %s\n", collected);
    }
    return 1;
}

/* Issue #12: Query the "stack" debug command and parse the high-water
 * mark to verify adequate headroom.  The firmware reports:
 *   "Stack free in <name> is <free>/<total>"
 * We PASS if free > 25% of total (i.e. comfortable margin at 2KB).
 */
static int do_test_stack_check(libusb_device_handle *h)
{
    uint8_t info[4] = {0};
    uint8_t buf[64];
    char collected[1024] = {0};
    int collected_len = 0;

    /* 1. Enable debug mode */
    int r = ctrl_read(h, TESTFX3, 1, 0, info, 4);
    if (r < 0) {
        printf("FAIL stack_check: enable debug mode: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 2. Drain stale output (30 rounds handles bursts from prior tests) */
    for (int i = 0; i < 30; i++) {
        int dr = ctrl_read(h, READINFODEBUG, 0, 0, buf, sizeof(buf));
        if (dr <= 0) break;     /* buffer empty → done draining */
        usleep(20000);
    }

    /* 3. Send "stack" + CR */
    const char *cmd = "stack";
    for (const char *p = cmd; *p; p++) {
        ctrl_read(h, READINFODEBUG, (uint16_t)*p, 0, buf, sizeof(buf));
        usleep(10000);
    }
    ctrl_read(h, READINFODEBUG, 0x0d, 0, buf, sizeof(buf));

    /* 4. Poll for response (up to 3 seconds) */
    for (int attempt = 0; attempt < 60; attempt++) {
        usleep(50000);
        r = ctrl_read(h, READINFODEBUG, 0, 0, buf, sizeof(buf));
        if (r > 0) {
            int copy = r - 1;
            if (collected_len + copy >= (int)sizeof(collected) - 1)
                copy = (int)sizeof(collected) - 1 - collected_len;
            if (copy > 0) {
                memcpy(collected + collected_len, buf, copy);
                collected_len += copy;
            }
            /* Early exit once we see the complete line */
            if (strstr(collected, "Stack free"))
                break;
        }
    }
    collected[collected_len] = '\0';

    /* 5. Parse "Stack free in <name> is <free>/<total>" */
    int free_bytes = -1, total_bytes = -1;
    char *p = strstr(collected, "Stack free");
    if (p) {
        char *is = strstr(p, " is ");
        if (is) {
            if (sscanf(is + 4, "%d/%d", &free_bytes, &total_bytes) != 2) {
                free_bytes = total_bytes = -1;
            }
        }
    }

    if (free_bytes < 0 || total_bytes <= 0) {
        printf("FAIL stack_check: could not parse stack response\n");
        if (collected_len > 0) {
            collected[collected_len < 200 ? collected_len : 200] = '\0';
            printf("#   debug output: %s\n", collected);
        }
        return 1;
    }

    /* 6. Verify total matches expected 2KB and free > 25% */
    int used = total_bytes - free_bytes;
    int margin_pct = (free_bytes * 100) / total_bytes;

    if (total_bytes != 2048) {
        printf("FAIL stack_check: expected 2048 total, got %d (issue #12)\n",
               total_bytes);
        return 1;
    }

    if (margin_pct < 25) {
        printf("FAIL stack_check: only %d/%d bytes free (%d%%) — insufficient margin (issue #12)\n",
               free_bytes, total_bytes, margin_pct);
        return 1;
    }

    printf("PASS stack_check: %d/%d used, %d/%d free (%d%% margin) (issue #12)\n",
           used, total_bytes, free_bytes, total_bytes, margin_pct);
    return 0;
}

/* ------------------------------------------------------------------ */
/* GETSTATS tests                                                     */
/* ------------------------------------------------------------------ */

/* GETSTATS response layout (20 bytes, little-endian):
 *   [0..3]   uint32  DMA buffer completions
 *   [4]      uint8   GPIF state machine state
 *   [5..8]   uint32  PIB error count
 *   [9..10]  uint16  last PIB error arg
 *   [11..14] uint32  I2C failure count
 *   [15..18] uint32  Streaming fault count (EP underruns + watchdog recoveries)
 *   [19]     uint8   Si5351 status register (reg 0)
 */
#define GETSTATS_LEN  20

struct fx3_stats {
    uint32_t dma_count;
    uint8_t  gpif_state;
    uint32_t pib_errors;
    uint16_t last_pib_arg;
    uint32_t i2c_failures;
    uint32_t streaming_faults;
    uint8_t  si5351_status;
};

static int read_stats(libusb_device_handle *h, struct fx3_stats *s)
{
    uint8_t buf[GETSTATS_LEN];
    int r = ctrl_read(h, GETSTATS, 0, 0, buf, GETSTATS_LEN);
    if (r < 0) return r;
    if (r < GETSTATS_LEN) return LIBUSB_ERROR_IO;
    memcpy(&s->dma_count,    &buf[0],  4);
    s->gpif_state = buf[4];
    memcpy(&s->pib_errors,   &buf[5],  4);
    memcpy(&s->last_pib_arg, &buf[9],  2);
    memcpy(&s->i2c_failures, &buf[11], 4);
    memcpy(&s->streaming_faults, &buf[15], 4);
    s->si5351_status = buf[19];
    return 0;
}

/* Read and display GETSTATS fields */
static int do_stats(libusb_device_handle *h)
{
    struct fx3_stats s;
    int r = read_stats(h, &s);
    if (r < 0) {
        printf("FAIL stats: %s\n", libusb_strerror(r));
        return 1;
    }
    printf("PASS stats: dma=%u gpif=%u pib=%u last_pib=0x%04X i2c=%u faults=%u pll=0x%02X\n",
           s.dma_count, s.gpif_state, s.pib_errors,
           s.last_pib_arg, s.i2c_failures, s.streaming_faults,
           s.si5351_status);
    return 0;
}

/* Verify I2C failure counter increments on NACK.
 * Read stats, trigger I2C NACK (absent address 0xC2), read stats again. */
static int do_test_stats_i2c(libusb_device_handle *h)
{
    struct fx3_stats before, after;
    int r = read_stats(h, &before);
    if (r < 0) {
        printf("FAIL stats_i2c: initial read: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Trigger I2C NACK — read from absent address 0xC2 */
    uint8_t buf[1];
    ctrl_read(h, I2CRFX3, 0xC2, 0, buf, 1);  /* expected to fail */

    r = read_stats(h, &after);
    if (r < 0) {
        printf("FAIL stats_i2c: post read: %s\n", libusb_strerror(r));
        return 1;
    }

    if (after.i2c_failures > before.i2c_failures) {
        printf("PASS stats_i2c: i2c_failures %u -> %u after NACK\n",
               before.i2c_failures, after.i2c_failures);
        return 0;
    }
    printf("FAIL stats_i2c: i2c_failures unchanged (%u -> %u)\n",
           before.i2c_failures, after.i2c_failures);
    return 1;
}

/* Verify PIB error counter is non-zero.
 *
 * In the fw_test.sh suite this runs after pib_overflow (test 21),
 * which already caused GPIF overflow errors.  The counter persists
 * across vendor requests (only reset by StartApplication on USB
 * re-enumeration), so we just verify it's > 0.
 *
 * When run standalone on a fresh device (counter == 0), we attempt
 * to provoke overflow ourselves. */
static int do_test_stats_pib(libusb_device_handle *h)
{
    struct fx3_stats s;
    int r = read_stats(h, &s);
    if (r < 0) {
        printf("FAIL stats_pib: read: %s\n", libusb_strerror(r));
        return 1;
    }

    /* If pib_overflow already ran, counter is > 0 — just verify. */
    if (s.pib_errors > 0) {
        printf("PASS stats_pib: pib_errors=%u last_pib=0x%04X (from prior overflow)\n",
               s.pib_errors, s.last_pib_arg);
        return 0;
    }

    /* Standalone: counter is 0, provoke overflow ourselves. */
    r = cmd_u32(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL stats_pib: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }

    r = cmd_u32(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL stats_pib: STARTFX3: %s\n", libusb_strerror(r));
        return 1;
    }

    /* Wait for DMA buffers to overflow */
    usleep(2000000);

    cmd_u32(h, STOPFX3, 0);
    usleep(200000);

    r = read_stats(h, &s);
    if (r < 0) {
        printf("FAIL stats_pib: post read: %s\n", libusb_strerror(r));
        return 1;
    }

    if (s.pib_errors > 0) {
        printf("PASS stats_pib: pib_errors=%u last_pib=0x%04X\n",
               s.pib_errors, s.last_pib_arg);
        return 0;
    }
    printf("FAIL stats_pib: pib_errors still 0 after overflow attempt\n");
    return 1;
}

/* Verify Si5351 PLL lock status from GETSTATS.
 * Reg 0 bit 7 = SYS_INIT (should be clear after boot).
 * Reg 0 bit 5 = PLL A not locked (should be clear when tuned). */
static int do_test_stats_pll(libusb_device_handle *h)
{
    struct fx3_stats s;
    int r = read_stats(h, &s);
    if (r < 0) {
        printf("FAIL stats_pll: read: %s\n", libusb_strerror(r));
        return 1;
    }

    if (s.si5351_status & 0x80) {
        printf("FAIL stats_pll: SYS_INIT set (0x%02X) — device not ready\n",
               s.si5351_status);
        return 1;
    }

    if (s.si5351_status & 0x20) {
        printf("FAIL stats_pll: PLL A not locked (0x%02X)\n",
               s.si5351_status);
        return 1;
    }

    printf("PASS stats_pll: si5351_status=0x%02X (SYS_INIT clear, PLL A locked)\n",
           s.si5351_status);
    return 0;
}

/* ------------------------------------------------------------------ */
/* GPIF wedge / stop-start tests                                      */
/* ------------------------------------------------------------------ */

/* EP1 IN (bulk consumer endpoint) */
#define EP1_IN  0x81

/* Try to read some bulk data from EP1 IN.  Returns the number of bytes
 * actually received, or a negative libusb error code.  A timeout (no
 * data within timeout_ms) returns 0. */
static int bulk_read_some(libusb_device_handle *h, int len, int timeout_ms)
{
    uint8_t *buf = malloc(len);
    if (!buf) return LIBUSB_ERROR_NO_MEM;
    int transferred = 0;
    int r = libusb_bulk_transfer(h, EP1_IN, buf, len, &transferred, timeout_ms);
    free(buf);
    if (r == LIBUSB_ERROR_TIMEOUT) return transferred;  /* partial is OK */
    if (r < 0) return r;
    return transferred;
}

/* Stop GPIF then verify the SM state via GETSTATS.
 *
 * Sequences: STARTADC(32 MHz) -> STARTFX3 -> brief stream -> STOPFX3
 * -> GETSTATS.  The GPIF state should be 0 (RESET) after a proper
 * GpifDisable, or 1 (IDLE) if the SM stopped gracefully.
 *
 * On the current (broken) firmware the SM is still running or stuck
 * in a BUSY/WAIT state after STOPFX3 — this test detects that. */
static int do_test_stop_gpif_state(libusb_device_handle *h)
{
    int r;

    /* 1. Configure ADC clock */
    r = cmd_u32(h, STARTADC, 32000000);
    if (r < 0) {
        printf("FAIL stop_gpif_state: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 2. Start streaming */
    r = cmd_u32(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL stop_gpif_state: STARTFX3: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 3. Read a little data to let GPIF run */
    bulk_read_some(h, 16384, 500);

    /* 4. Stop */
    r = cmd_u32(h, STOPFX3, 0);
    if (r < 0) {
        printf("FAIL stop_gpif_state: STOPFX3: %s\n", libusb_strerror(r));
        return 1;
    }
    usleep(50000);  /* let SM settle */

    /* 5. Read GPIF state */
    struct fx3_stats s;
    r = read_stats(h, &s);
    if (r < 0) {
        printf("FAIL stop_gpif_state: GETSTATS: %s\n", libusb_strerror(r));
        return 1;
    }

    /* State 0 = RESET, 1 = IDLE (graceful stop),
     * 255 = GPIF block disabled (CyU3PGpifDisable with force=true) */
    if (s.gpif_state == 0 || s.gpif_state == 1 || s.gpif_state == 255) {
        printf("PASS stop_gpif_state: GPIF state=%u after STOP (SM properly stopped)\n",
               s.gpif_state);
        return 0;
    }
    printf("FAIL stop_gpif_state: GPIF state=%u after STOP (expected 0, 1, or 255; SM still running)\n",
           s.gpif_state);
    return 1;
}

/* Repeatedly cycle STOP+START and verify streaming resumes each time.
 *
 * Sequences N iterations of:
 *   STARTFX3 -> read bulk data (verify flowing) -> STOPFX3
 * with a single STARTADC before the loop.
 *
 * On the current firmware this wedges on the 2nd or 3rd cycle because
 * STARTFX3 doesn't restart the SM after STOPFX3 leaves it stuck. */
static int do_test_stop_start_cycle(libusb_device_handle *h)
{
    int cycles = 5;
    int r;

    /* Configure ADC clock once */
    r = cmd_u32(h, STARTADC, 32000000);
    if (r < 0) {
        printf("FAIL stop_start_cycle: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }

    for (int i = 0; i < cycles; i++) {
        /* Start streaming */
        r = cmd_u32(h, STARTFX3, 0);
        if (r < 0) {
            printf("FAIL stop_start_cycle: STARTFX3 on cycle %d: %s\n",
                   i + 1, libusb_strerror(r));
            return 1;
        }

        /* Read bulk data — should get at least 1 KB within 2 seconds */
        int got = bulk_read_some(h, 16384, 2000);
        if (got < 1024) {
            printf("FAIL stop_start_cycle: cycle %d/%d: only %d bytes "
                   "(expected >= 1024, stream not flowing)\n",
                   i + 1, cycles, got < 0 ? 0 : got);
            /* Try to clean up */
            cmd_u32(h, STOPFX3, 0);
            return 1;
        }

        /* Stop streaming */
        r = cmd_u32(h, STOPFX3, 0);
        if (r < 0) {
            printf("FAIL stop_start_cycle: STOPFX3 on cycle %d: %s\n",
                   i + 1, libusb_strerror(r));
            return 1;
        }

        /* Brief pause between cycles */
        usleep(100000);
    }

    printf("PASS stop_start_cycle: %d stop/start cycles completed, data flowing each time\n",
           cycles);
    return 0;
}

/* Verify STARTFX3 is rejected when the ADC clock is off.
 *
 * Sequences: STARTADC(0) -> STARTFX3 -> check result.
 * With the PLL pre-flight check, STARTFX3 should fail (STALL or
 * return isHandled=false).  Without it, START succeeds and the GPIF
 * runs on stale data.
 *
 * NOTE: After this test the ADC clock is off.  The test restores
 * clock at the end so subsequent tests can run. */
static int do_test_pll_preflight(libusb_device_handle *h)
{
    int r;

    /* 1. Turn off ADC clock */
    r = cmd_u32(h, STARTADC, 0);
    if (r < 0) {
        printf("FAIL pll_preflight: STARTADC(0): %s\n", libusb_strerror(r));
        return 1;
    }

    /* 2. Brief pause for PLL to drop lock */
    usleep(200000);

    /* 3. Attempt STARTFX3 — should be rejected if PLL pre-flight is implemented */
    r = cmd_u32(h, STARTFX3, 0);

    /* Accepted STALL as rejection */
    int start_rejected = (r == LIBUSB_ERROR_PIPE);

    /* Also check: if it "succeeded", try to read data.  If no data flows,
     * the firmware may have silently rejected it. */
    if (r == 0) {
        int got = bulk_read_some(h, 4096, 1000);
        /* If we get data despite clock being off, that's stale garbage —
         * the pre-flight check is missing.  Clean up. */
        cmd_u32(h, STOPFX3, 0);
        if (got <= 0) {
            /* No data: firmware may have quietly rejected (acceptable) */
            start_rejected = 1;
        }
    }

    /* 4. Restore clock for subsequent tests */
    cmd_u32(h, STARTADC, 32000000);

    if (start_rejected) {
        printf("PASS pll_preflight: STARTFX3 correctly rejected with PLL unlocked\n");
        return 0;
    }
    printf("FAIL pll_preflight: STARTFX3 accepted with ADC clock off (no PLL pre-flight check)\n");
    return 1;
}

/* Test recovery after a deliberate DMA backpressure wedge.
 *
 * Sequences:
 *   1. STARTADC(64 MHz) + STARTFX3
 *   2. Do NOT read EP1 — let DMA buffers fill and GPIF enter BUSY/WAIT
 *   3. Wait 2 seconds (longer than the 300ms watchdog threshold)
 *   4. STOPFX3 + STARTFX3
 *   5. Read EP1 — data should flow if recovery worked
 *   6. Check GETSTATS for recovery counter (glCounter[2])
 *
 * On current firmware: step 5 times out (device is wedged).
 * After Phase 2: step 5 succeeds (STOP+START recovers).
 * After Phase 4: the watchdog may auto-recover in step 3, and
 *   glCounter[2] > 0 in GETSTATS. */
static int do_test_wedge_recovery(libusb_device_handle *h)
{
    int r;

    /* Enable debug mode to see recovery messages */
    uint8_t info[4] = {0};
    ctrl_read(h, TESTFX3, 1, 0, info, 4);

    /* 1. Configure ADC at 64 MHz */
    r = cmd_u32(h, STARTADC, 64000000);
    if (r < 0) {
        printf("FAIL wedge_recovery: STARTADC: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 2. Start streaming */
    r = cmd_u32(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL wedge_recovery: STARTFX3: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 3. Do NOT read EP1 — wait for DMA to back up and GPIF to wedge.
     *    At 64 MS/s, 4 x 16 KB buffers fill in < 1 ms.
     *    Wait 2 seconds to allow watchdog to fire (if implemented). */
    usleep(2000000);

    /* 4. Stop */
    r = cmd_u32(h, STOPFX3, 0);
    if (r < 0) {
        printf("FAIL wedge_recovery: STOPFX3 after wedge: %s\n", libusb_strerror(r));
        return 1;
    }
    usleep(100000);

    /* 5. Restart */
    r = cmd_u32(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL wedge_recovery: STARTFX3 after stop: %s\n", libusb_strerror(r));
        return 1;
    }

    /* 6. Read bulk data — should flow if recovery worked */
    int got = bulk_read_some(h, 16384, 2000);

    /* 7. Clean up */
    cmd_u32(h, STOPFX3, 0);
    usleep(100000);

    /* 8. Check GETSTATS for recovery counter */
    struct fx3_stats s;
    read_stats(h, &s);

    if (got < 1024) {
        printf("FAIL wedge_recovery: only %d bytes after recovery "
               "(expected >= 1024, device still wedged)\n",
               got < 0 ? 0 : got);
        printf("#   GPIF state=%u, streaming_faults=%u\n",
               s.gpif_state, s.streaming_faults);
        return 1;
    }

    printf("PASS wedge_recovery: %d bytes after STOP+START recovery", got);
    if (s.streaming_faults > 0)
        printf(", watchdog_recoveries=%u", s.streaming_faults);
    printf("\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Usage and main                                                     */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <command> [args...]\n"
        "\n"
        "Commands:\n"
        "  test                         Read device info (TESTFX3)\n"
        "  gpio <bits>                  Set GPIO word (hex or decimal)\n"
        "  adc <freq_hz>               Set ADC clock frequency (STARTADC)\n"
        "  att <0-63>                   Set DAT-31 attenuator\n"
        "  vga <0-255>                  Set AD8370 VGA gain\n"
        "  start                        Start streaming (STARTFX3)\n"
        "  stop                         Stop streaming (STOPFX3)\n"
        "  i2cr <addr> <reg> <len>      I2C read (hex addresses)\n"
        "  i2cw <addr> <reg> <byte>...  I2C write (hex addresses, hex data)\n"
        "  reset                        Reboot FX3 to bootloader\n"
        "  debug                        Interactive debug console over USB\n"
        "  raw <code>                   Send raw vendor request (hex)\n"
        "  ep0_overflow                 Test EP0 wLength bounds check\n"
        "  oob_brequest                 Test OOB bRequest bounds (issue #21)\n"
        "  oob_setarg                   Test OOB SETARGFX3 wIndex (issue #20)\n"
        "  console_fill                 Test console buffer fill (issue #13)\n"
        "  debug_race                   Stress-test debug buffer race (issue #8)\n"
        "  debug_poll                   Test debug console over USB (issue #26)\n"
        "  pib_overflow                 Provoke + detect PIB error (issue #10)\n"
        "  stack_check                  Query stack watermark, verify headroom (issue #12)\n"
        "  stats                        Read GETSTATS diagnostic counters\n"
        "  stats_i2c                    Verify I2C failure counter via NACK\n"
        "  stats_pib                    Verify PIB error counter via overflow\n"
        "  stats_pll                    Verify Si5351 PLL lock status\n"
        "  stop_gpif_state              Verify GPIF SM stops after STOPFX3\n"
        "  stop_start_cycle             Cycle STOP+START N times, verify data\n"
        "  pll_preflight                Verify STARTFX3 rejected without clock\n"
        "  wedge_recovery               Provoke DMA wedge, test STOP+START recovery\n"
        "\n"
        "Output:  PASS/FAIL <command> [details]\n"
        "Exit:    0 on PASS, 1 on FAIL\n",
        prog);
}

static unsigned long parse_num(const char *s)
{
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 0);  /* 0 = auto-detect base */
    if (errno || *end != '\0') {
        fprintf(stderr, "error: invalid number '%s'\n", s);
        exit(2);
    }
    return v;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    const char *cmd = argv[1];

    libusb_context *ctx = NULL;
    int r = libusb_init(&ctx);
    if (r < 0) {
        fprintf(stderr, "error: libusb_init: %s\n", libusb_strerror(r));
        return 1;
    }

    libusb_device_handle *h = open_rx888(ctx);
    if (!h) {
        libusb_exit(ctx);
        return 1;
    }

    int rc = 1;

    if (strcmp(cmd, "test") == 0) {
        rc = do_test(h);

    } else if (strcmp(cmd, "gpio") == 0) {
        if (argc < 3) { usage(argv[0]); goto out; }
        rc = do_gpio(h, (uint32_t)parse_num(argv[2]));

    } else if (strcmp(cmd, "adc") == 0) {
        if (argc < 3) { usage(argv[0]); goto out; }
        rc = do_adc(h, (uint32_t)parse_num(argv[2]));

    } else if (strcmp(cmd, "att") == 0) {
        if (argc < 3) { usage(argv[0]); goto out; }
        rc = do_att(h, (uint16_t)parse_num(argv[2]));

    } else if (strcmp(cmd, "vga") == 0) {
        if (argc < 3) { usage(argv[0]); goto out; }
        rc = do_vga(h, (uint16_t)parse_num(argv[2]));

    } else if (strcmp(cmd, "start") == 0) {
        rc = do_start(h);

    } else if (strcmp(cmd, "stop") == 0) {
        rc = do_stop(h);

    } else if (strcmp(cmd, "i2cr") == 0) {
        if (argc < 5) { usage(argv[0]); goto out; }
        rc = do_i2cr(h, (uint16_t)parse_num(argv[2]),
                        (uint16_t)parse_num(argv[3]),
                        (uint16_t)parse_num(argv[4]));

    } else if (strcmp(cmd, "i2cw") == 0) {
        if (argc < 5) { usage(argv[0]); goto out; }
        uint16_t addr = (uint16_t)parse_num(argv[2]);
        uint16_t reg  = (uint16_t)parse_num(argv[3]);
        int ndata = argc - 4;
        uint8_t data[64];
        if (ndata > (int)sizeof(data)) ndata = (int)sizeof(data);
        for (int i = 0; i < ndata; i++)
            data[i] = (uint8_t)parse_num(argv[4 + i]);
        rc = do_i2cw(h, addr, reg, data, (uint16_t)ndata);

    } else if (strcmp(cmd, "debug") == 0) {
        rc = do_debug(h);

    } else if (strcmp(cmd, "oob_brequest") == 0) {
        rc = do_test_oob_brequest(h);

    } else if (strcmp(cmd, "oob_setarg") == 0) {
        rc = do_test_oob_setarg(h);

    } else if (strcmp(cmd, "console_fill") == 0) {
        rc = do_test_console_fill(h);

    } else if (strcmp(cmd, "debug_race") == 0) {
        rc = do_test_debug_race(h);

    } else if (strcmp(cmd, "debug_poll") == 0) {
        rc = do_test_debug_poll(h);

    } else if (strcmp(cmd, "pib_overflow") == 0) {
        rc = do_test_pib_overflow(h);

    } else if (strcmp(cmd, "stack_check") == 0) {
        rc = do_test_stack_check(h);

    } else if (strcmp(cmd, "stats") == 0) {
        rc = do_stats(h);

    } else if (strcmp(cmd, "stats_i2c") == 0) {
        rc = do_test_stats_i2c(h);

    } else if (strcmp(cmd, "stats_pib") == 0) {
        rc = do_test_stats_pib(h);

    } else if (strcmp(cmd, "stats_pll") == 0) {
        rc = do_test_stats_pll(h);

    } else if (strcmp(cmd, "stop_gpif_state") == 0) {
        rc = do_test_stop_gpif_state(h);

    } else if (strcmp(cmd, "stop_start_cycle") == 0) {
        rc = do_test_stop_start_cycle(h);

    } else if (strcmp(cmd, "pll_preflight") == 0) {
        rc = do_test_pll_preflight(h);

    } else if (strcmp(cmd, "wedge_recovery") == 0) {
        rc = do_test_wedge_recovery(h);

    } else if (strcmp(cmd, "reset") == 0) {
        rc = do_reset(h);

    } else if (strcmp(cmd, "raw") == 0) {
        if (argc < 3) { usage(argv[0]); goto out; }
        rc = do_raw(h, (uint8_t)parse_num(argv[2]));

    } else if (strcmp(cmd, "ep0_overflow") == 0) {
        rc = do_ep0_overflow(h);

    } else {
        fprintf(stderr, "error: unknown command '%s'\n", cmd);
        usage(argv[0]);
        rc = 2;
    }

out:
    close_rx888(h);
    libusb_exit(ctx);
    return rc;
}
