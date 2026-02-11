/*
 * fx3_cmd.c — Vendor command exerciser for SDDC_FX3 firmware.
 *
 * Sends individual USB vendor requests to an RX888mk2 and reports
 * success/failure.  Designed for scripted hardware testing.
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
#define TUNERINIT     0xB4
#define TUNERTUNE     0xB5
#define SETARGFX3     0xB6
#define TUNERSTDBY    0xB8

/* SETARGFX3 argument IDs */
#define DAT31_ATT     10
#define AD8340_VGA    11

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
    int r = set_arg(h, AD8340_VGA, val);
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
        "  vga <0-255>                  Set AD8340 VGA gain\n"
        "  start                        Start streaming (STARTFX3)\n"
        "  stop                         Stop streaming (STOPFX3)\n"
        "  i2cr <addr> <reg> <len>      I2C read (hex addresses)\n"
        "  i2cw <addr> <reg> <byte>...  I2C write (hex addresses, hex data)\n"
        "  reset                        Reboot FX3 to bootloader\n"
        "  raw <code>                   Send raw vendor request (hex)\n"
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

    } else if (strcmp(cmd, "reset") == 0) {
        rc = do_reset(h);

    } else if (strcmp(cmd, "raw") == 0) {
        if (argc < 3) { usage(argv[0]); goto out; }
        rc = do_raw(h, (uint8_t)parse_num(argv[2]));

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
