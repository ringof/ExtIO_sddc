/*
 * fx3_core.c — shared diagnostic/operator command core for the SDDC_FX3
 * host tools.  Extracted from fx3_cmd.c (issue #139 modularization; fx3_cmd
 * split into a diagnostics CLI + firmware harness).
 *
 * Contains the operator-diagnostics primitives — probe, poke, recover, and
 * the read-only status reads — plus firmware upload via rx888_stream.  No
 * dependency on the regression/fuzz/soak harness, so both the diagnostics
 * CLI and the firmware test binary can link it.
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
#include <time.h>
#include <sys/wait.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#include <libusb-1.0/libusb.h>

#include "fx3_proto.h"
#include "fx3_usb.h"
#include "fx3_stats.h"
#include "fx3_core.h"

/* Locate the rx888_stream binary.  Search order:
 *   1. Same directory as fx3_cmd (symlink created by tests/Makefile)
 *   2. rx888_tools/rx888_stream in the same directory
 *   3. Fall back to bare name (let exec search PATH)
 * Returns 1 if a verified path was found, 0 if falling back to PATH. */
int find_rx888_stream(char *out, size_t out_size)
{
    /* Leave room for longest suffix ("rx888_tools/rx888_stream" = 24) */
    char self_dir[PATH_MAX - 32];
    ssize_t len = readlink("/proc/self/exe", self_dir, sizeof(self_dir) - 1);
    if (len > 0) {
        self_dir[len] = '\0';
        char *slash = strrchr(self_dir, '/');
        if (slash) {
            *(slash + 1) = '\0';

            snprintf(out, out_size, "%srx888_stream", self_dir);
            if (access(out, X_OK) == 0)
                return 1;

            snprintf(out, out_size, "%srx888_tools/rx888_stream", self_dir);
            if (access(out, X_OK) == 0)
                return 1;
        }
    }

    /* Fall back to PATH */
    snprintf(out, out_size, "rx888_stream");
    return 0;
}

/* Upload firmware to an FX3 device in bootloader mode by forking
 * rx888_stream.  Mirrors the upload sequence in soak_test.sh:
 *   1. Fork rx888_stream -f <fw_path> -s 32000000
 *   2. Wait 4 s for upload + enumeration
 *   3. Kill rx888_stream (it would otherwise stream forever)
 *   4. Wait 2 s for USB re-enumeration
 *   5. Verify device appeared at app PID (0x00F1)
 * Returns 0 on success, -1 on failure. */
int upload_firmware(libusb_context *ctx, const char *fw_path)
{
    char stream_bin[PATH_MAX];
    find_rx888_stream(stream_bin, sizeof(stream_bin));

    if (access(fw_path, R_OK) != 0) {
        fprintf(stderr, "error: firmware file not readable: %s\n", fw_path);
        return -1;
    }

    fprintf(stderr, "uploading firmware: %s\n", fw_path);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        /* Child: suppress output, exec rx888_stream */
        if (!freopen("/dev/null", "w", stdout)) _exit(126);
        if (!freopen("/dev/null", "w", stderr)) _exit(126);
        execl(stream_bin, "rx888_stream",
              "-f", fw_path, "-s", "32000000", (char *)NULL);
        /* execl only returns on error — try PATH as last resort */
        execlp("rx888_stream", "rx888_stream",
               "-f", fw_path, "-s", "32000000", (char *)NULL);
        _exit(127);
    }

    /* Parent: wait 4 s for upload, then kill */
    sleep(4);
    kill(pid, SIGTERM);
    int status;
    waitpid(pid, &status, 0);

    /* Wait for USB re-enumeration */
    sleep(2);

    /* Verify device appeared at app PID */
    libusb_device_handle *h =
        libusb_open_device_with_vid_pid(ctx, RX888_VID, RX888_PID_APP);
    if (!h) {
        fprintf(stderr, "error: device not found at PID 0x%04X "
                "after firmware upload\n", RX888_PID_APP);
        return -1;
    }
    libusb_close(h);

    fprintf(stderr, "firmware uploaded — device ready at PID 0x%04X\n",
            RX888_PID_APP);
    return 0;
}

int do_test(libusb_device_handle *h)
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

int do_gpio(libusb_device_handle *h, uint32_t bits)
{
    int r = cmd_u32(h, GPIOFX3, bits);
    if (r < 0) {
        printf("FAIL gpio 0x%08X: %s\n", bits, libusb_strerror(r));
        return 1;
    }
    printf("PASS gpio 0x%08X\n", bits);
    return 0;
}

int do_adc(libusb_device_handle *h, uint32_t freq)
{
    int r = cmd_u32(h, STARTADC, freq);
    if (r < 0) {
        printf("FAIL adc %u: %s\n", freq, libusb_strerror(r));
        return 1;
    }
    printf("PASS adc %u Hz\n", freq);
    return 0;
}

int do_att(libusb_device_handle *h, uint16_t val)
{
    int r = set_arg(h, DAT31_ATT, val);
    if (r < 0) {
        printf("FAIL att %u: %s\n", val, libusb_strerror(r));
        return 1;
    }
    printf("PASS att %u\n", val);
    return 0;
}

int do_vga(libusb_device_handle *h, uint16_t val)
{
    int r = set_arg(h, AD8370_VGA, val);
    if (r < 0) {
        printf("FAIL vga %u: %s\n", val, libusb_strerror(r));
        return 1;
    }
    printf("PASS vga %u\n", val);
    return 0;
}

int do_wdg_max(libusb_device_handle *h, uint16_t val)
{
    int r = set_arg(h, WDG_MAX_RECOV, val);
    if (r < 0) {
        printf("FAIL wdg_max %u: %s\n", val, libusb_strerror(r));
        return 1;
    }
    printf("PASS wdg_max %u\n", val);
    return 0;
}

int do_start(libusb_device_handle *h)
{
    int r = cmd_u32(h, STARTFX3, 0);
    if (r < 0) {
        printf("FAIL start: %s\n", libusb_strerror(r));
        return 1;
    }
    printf("PASS start\n");
    return 0;
}

int do_stop(libusb_device_handle *h)
{
    int r = cmd_u32(h, STOPFX3, 0);
    if (r < 0) {
        printf("FAIL stop: %s\n", libusb_strerror(r));
        return 1;
    }
    printf("PASS stop\n");
    return 0;
}

int do_i2cr(libusb_device_handle *h, uint16_t addr, uint16_t reg, uint16_t len)
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

int do_i2cw(libusb_device_handle *h, uint16_t addr, uint16_t reg,
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

int do_reset(libusb_device_handle *h)
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

/* Host-side USB port reset via the raw USBDEVFS_RESET ioctl.
 *
 * Deliberately does NOT use libusb_reset_device(): that requires opening
 * and claiming the device, which can fail on exactly the wedged state this
 * is meant to recover.  We only enumerate (read descriptors — no claim) to
 * locate the bus/address, then issue the ioctl on the /dev/bus/usb node,
 * so it works even when the firmware is too wedged for libusb to claim.
 *
 * Linux-only (USBDEVFS_RESET), which matches the Docker-on-Linux harness
 * and the external `usbreset` utility it replaces.  Unlike RESETFX3 this
 * does not power-cycle the FX3, so a running device may re-enumerate still
 * loaded — pair it with `reset` (RESETFX3) when a fresh image is required. */
int do_usbreset(libusb_context *ctx)
{
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0) {
        fprintf(stderr, "FAIL usbreset: libusb_get_device_list: %s\n",
                libusb_strerror((int)n));
        return 1;
    }

    uint8_t bus = 0, addr = 0;
    uint16_t pid = 0;
    int found = 0;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) != 0)
            continue;
        if (d.idVendor == RX888_VID &&
            (d.idProduct == RX888_PID_APP || d.idProduct == RX888_PID_BOOT)) {
            bus = libusb_get_bus_number(list[i]);
            addr = libusb_get_device_address(list[i]);
            pid = d.idProduct;
            found = 1;
            break;
        }
    }
    libusb_free_device_list(list, 1);

    if (!found) {
        fprintf(stderr, "FAIL usbreset: no RX888 found "
                "(VID 0x%04X, PID 0x%04X/0x%04X)\n",
                RX888_VID, RX888_PID_APP, RX888_PID_BOOT);
        return 1;
    }

    char path[64];
    snprintf(path, sizeof(path), "/dev/bus/usb/%03u/%03u", bus, addr);

    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "FAIL usbreset: open %s: %s\n", path, strerror(errno));
        return 1;
    }
    int ret = ioctl(fd, USBDEVFS_RESET, 0);
    int saved = errno;
    close(fd);
    if (ret < 0 && saved != ENODEV) {
        fprintf(stderr, "FAIL usbreset: USBDEVFS_RESET %s: %s\n",
                path, strerror(saved));
        return 1;
    }
    if (ret < 0) {
        /* ENODEV is expected, not an error: the port reset reboots the
         * FX3, which re-enumerates as a *new* device (on this hardware it
         * drops to the bootloader, PID 0x00F1 -> 0x00F3), so the original
         * /dev/bus/usb node disappears before the ioctl returns.  Same
         * "device disconnected on success" semantics as RESETFX3.  The
         * device now needs a firmware re-upload before it is usable. */
        printf("PASS usbreset %s (PID 0x%04X reset; device re-enumerated — "
               "re-upload firmware)\n", path, pid);
    } else {
        printf("PASS usbreset %s (PID 0x%04X reset)\n", path, pid);
    }
    return 0;
}

/* Force a full firmware reload: reset the FX3 to the bootloader using BOTH
 * the in-band RESETFX3 vendor command and a host-side usbreset (so a wedged
 * device that can't accept RESETFX3 is still recovered), re-upload the
 * image, and verify the device returns healthy at the app PID.
 *
 * This is the force_reload() primitive the ka9q-radio soak fires every few
 * minutes; it also stands alone as `fx3_cmd [-F img] reload`.  Both the
 * vendor reset and the bus reset leave the device in DFU, so a re-upload is
 * mandatory afterward — that's the whole point. */
int do_reload(libusb_context *ctx, const char *fw)
{
    if (access(fw, R_OK) != 0) {
        fprintf(stderr, "FAIL reload: firmware not readable: %s\n", fw);
        return 1;
    }

    /* In-band reset first: if the device is in app mode and claimable, send
     * RESETFX3 (the path nothing else exercises).  Best-effort — a wedged
     * device may refuse it, which is what the usbreset below covers. */
    libusb_device_handle *app =
        libusb_open_device_with_vid_pid(ctx, RX888_VID, RX888_PID_APP);
    if (app) {
        if (libusb_kernel_driver_active(app, 0) == 1)
            libusb_detach_kernel_driver(app, 0);
        if (libusb_claim_interface(app, 0) == 0) {
            cmd_u32(app, RESETFX3, 0);   /* device disconnects; result ignored */
            printf("# reload: RESETFX3 sent\n");
        }
        libusb_close(app);
        sleep(2);                        /* let it re-enumerate to bootloader */
    }

    /* Host-side kick: always issue a usbreset too.  If RESETFX3 already
     * dropped the device to the bootloader this just resets it again
     * (harmless); if RESETFX3 couldn't be delivered, this recovers it.
     * Non-fatal — the bootloader-wait below is the real gate. */
    do_usbreset(ctx);
    sleep(2);

    /* Gate: confirm the device is in the bootloader before re-uploading. */
    int in_boot = 0;
    for (int waited_ms = 0; waited_ms < 6000; waited_ms += 250) {
        libusb_device_handle *bl =
            libusb_open_device_with_vid_pid(ctx, RX888_VID, RX888_PID_BOOT);
        if (bl) { libusb_close(bl); in_boot = 1; break; }
        usleep(250000);
    }
    if (!in_boot) {
        fprintf(stderr, "FAIL reload: device did not enter bootloader "
                "(PID 0x%04X) within timeout\n", RX888_PID_BOOT);
        return 1;
    }
    printf("# reload: device in bootloader (PID 0x%04X)\n", RX888_PID_BOOT);

    /* Re-upload (verifies the device returns at the app PID). */
    if (upload_firmware(ctx, fw) != 0) {
        printf("FAIL reload: firmware re-upload failed\n");
        return 1;
    }

    /* Verify the freshly-loaded firmware answers TESTFX3. */
    libusb_device_handle *h = open_rx888(ctx);
    if (!h) {
        printf("FAIL reload: device not usable after re-upload\n");
        return 1;
    }
    int rc = do_test(h);
    close_rx888(h);
    if (rc != 0) {
        printf("FAIL reload: device unhealthy after re-upload\n");
        return 1;
    }
    printf("PASS reload (device re-flashed and healthy at PID 0x%04X)\n",
           RX888_PID_APP);
    return 0;
}

/* Send a raw vendor command code — for testing stale/removed commands */
int do_raw(libusb_device_handle *h, uint8_t code)
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

/* Issue #12: Query the "stack" debug command and parse the high-water
 * mark to verify adequate headroom.  The firmware reports:
 *   "Stack free in <name> is <free>/<total>"
 * We PASS if free > 25% of total (i.e. comfortable margin at 2KB).
 */
int do_test_stack_check(libusb_device_handle *h)
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

/* Verify Si5351 PLL lock status from GETSTATS.
 * Reg 0 bit 7 = SYS_INIT (should be clear after boot).
 * Reg 0 bit 5 = PLL A not locked (should be clear when tuned). */
int do_test_stats_pll(libusb_device_handle *h)
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

/* Verify the firmware parks the ADC in SHDN standby after STOPFX3
 * (issue #131). Sequence: STARTADC -> STARTFX3 (wakes SHDN) -> STOPFX3
 * (firmware asserts SHDN in the stop path) -> read gpio_state from
 * GETSTATS and check the SHDWN bit is set. The gpio_state bit positions
 * mirror enum GPIOPin in protocol.h, so we mask with the same SHDWN
 * constant the host uses for the GPIOFX3 control word. */
int do_test_stats_shdn(libusb_device_handle *h)
{
    struct fx3_stats s;
    int r;

    /* Bring the ADC into a known-awake state: STARTADC + STARTFX3. */
    cmd_u32(h, STARTADC, 32000000);
    if (cmd_u32(h, STARTFX3, 0) != 0) {
        printf("FAIL stats_shdn: STARTFX3 failed\n");
        return 1;
    }
    usleep(50000);  /* let the wake settle */

    /* Stop. Firmware's STOPFX3 handler asserts SHDN at the end. */
    cmd_u32(h, STOPFX3, 0);
    usleep(50000);

    r = read_stats(h, &s);
    if (r < 0) {
        printf("FAIL stats_shdn: GETSTATS: %s\n", libusb_strerror(r));
        return 1;
    }
    if (!s.has_gpio_state) {
        printf("FAIL stats_shdn: this firmware's GETSTATS is %d bytes with no "
               "gpio_state field; SHDN cannot be observed (needs the 30-byte "
               "payload, #131)\n", s.payload_len);
        return 1;
    }
    if (!(s.gpio_state & SHDWN)) {
        printf("FAIL stats_shdn: SHDWN not asserted after STOPFX3 (gpio_state=0x%05X)\n",
               s.gpio_state);
        return 1;
    }
    printf("PASS stats_shdn: SHDWN asserted after STOPFX3 (gpio_state=0x%05X)\n",
           s.gpio_state);
    return 0;
}
