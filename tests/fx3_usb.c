/*
 * fx3_usb.c — USB transport + device lifecycle for the SDDC_FX3 host tools.
 * Extracted verbatim from fx3_cmd.c (issue #139).
 */
#include "fx3_usb.h"
#include "fx3_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

libusb_context *g_ctx;
const char *g_firmware_path = NULL;

int ctrl_write_u32(libusb_device_handle *h, uint8_t request,
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

int ctrl_write_buf(libusb_device_handle *h, uint8_t request,
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

int ctrl_read(libusb_device_handle *h, uint8_t request,
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
int cmd_u32(libusb_device_handle *h, uint8_t cmd, uint32_t val)
{
    return ctrl_write_u32(h, cmd, 0, 0, val);
}

/* Convenience: send SETARGFX3 with arg_id in wIndex, arg_val in wValue,
 * and a 1-byte zero payload (matches rx888_stream encoding). */
int set_arg(libusb_device_handle *h, uint16_t arg_id, uint16_t arg_val)
{
    uint8_t zero = 0;
    return ctrl_write_buf(h, SETARGFX3, arg_val, arg_id, &zero, 1);
}

/* Retry a command on a transient USB error with escalating backoff.
 * When a soak scenario starts right after a prior scenario triggered
 * heavy watchdog activity, the device may still be mid-recovery and
 * unable to service control transfers.  This manifests as either:
 *
 *   LIBUSB_ERROR_TIMEOUT  — transfer completed but device didn't ACK
 *                           within CTRL_TIMEOUT_MS
 *   LIBUSB_ERROR_IO       — low-level USB I/O failure (broken pipe,
 *                           NAK flood, etc.) while the FX3 is
 *                           resetting its DMA/GPIF state
 *
 * The helper retries up to twice with escalating backoff (500 ms then
 * 1 s, worst-case 1.5 s total).  STARTFX3 is especially sensitive
 * because it restarts the GPIF state machine — unlike simple EP0
 * reads (TESTFX3) which succeed sooner.  The 1.5 s budget matches the
 * observed watchdog recovery window (~2 s) while still catching a
 * genuinely wedged device within a few seconds.
 *
 * Convention: use cmd_u32_retry for the FIRST STARTADC + STARTFX3 in
 * every soak scenario (the "entry point" calls most exposed to
 * inter-scenario timing).  Use plain cmd_u32 for mid-scenario calls
 * (STOP→START transitions, recovery verification, etc.) so genuine
 * firmware failures are caught immediately. */
int cmd_u32_retry(libusb_device_handle *h, uint8_t cmd, uint32_t val)
{
    int r = cmd_u32(h, cmd, val);
    if (r != LIBUSB_ERROR_TIMEOUT && r != LIBUSB_ERROR_IO)
        return r;
    usleep(500000);                    /* 500 ms backoff */
    r = cmd_u32(h, cmd, val);
    if (r != LIBUSB_ERROR_TIMEOUT && r != LIBUSB_ERROR_IO)
        return r;
    usleep(1000000);                   /* 1 s backoff */
    return cmd_u32(h, cmd, val);
}

/* Survival check after an operation that *intentionally* STALLs EP0 —
 * an out-of-range vendor wIndex (SETARGFX3), a bad-I2C-address write
 * (I2CWFX3), etc.  A USB control endpoint auto-clears its stall on the
 * next SETUP packet, but the FX3 occasionally races that clear, so the
 * immediately-following control transfer can return LIBUSB_ERROR_PIPE
 * even though the device is alive and answers the very next request
 * (issue #135).  These scenarios verify device *survival*, not EP0 stall
 * timing, so tolerate a transient PIPE and retry — each retry's own SETUP
 * clears the stale stall.
 *
 * A single 2 ms retry cleared oob_setarg, but a 3-hour soak still caught
 * i2c_write_bad_addr once: its stall comes via isHandled=CyFalse (SDK
 * auto-stall *after* the handler returns) plus the failed I2cTransfer's
 * bus latency, a longer/later stall window than oob_setarg's explicit
 * CyU3PUsbStall.  So retry up to 3 times with escalating backoff
 * (2/4/8 ms).  A genuinely wedged EP0 stays stalled and fails every
 * attempt, so this still does not mask a real hang.  Returns the libusb
 * rc of a TESTFX3 read (>= 0 means the device is alive). */
int ep0_alive_after_stall(libusb_device_handle *h)
{
    uint8_t info[4] = {0};
    int r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    for (int i = 0; r == LIBUSB_ERROR_PIPE && i < 3; i++) {
        usleep(2000 << i);  /* 2, 4, 8 ms — let the controller settle the stall-clear */
        r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Device open / close                                                */
/* ------------------------------------------------------------------ */

/* Is an RX888 currently enumerated?  Returns 1 and sets *pid_out if a device
 * is present at either the app or bootloader PID.  Used to distinguish a
 * udev/permission-settle race (device present but libusb_open fails with
 * EACCES) from a genuinely absent device, so open_rx888() only pays retry
 * latency when the device is actually there. */
static int rx888_present(libusb_context *ctx, uint16_t *pid_out)
{
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0) return 0;
    int found = 0;
    uint16_t pid = 0;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) != 0)
            continue;
        if (d.idVendor == RX888_VID &&
            (d.idProduct == RX888_PID_APP || d.idProduct == RX888_PID_BOOT)) {
            found = 1; pid = d.idProduct; break;
        }
    }
    libusb_free_device_list(list, 1);
    if (found && pid_out) *pid_out = pid;
    return found;
}

libusb_device_handle *open_rx888(libusb_context *ctx)
{
    /* Bounded retry to absorb a udev/libusb enumeration race (#143): right
     * after the device (re-)enumerates, the node can exist before udev has
     * applied the access rule, so libusb_open fails with EACCES for a short
     * window.  Retry only while the device is actually present at the APP PID
     * (present-but-unopenable = the permission-settle race); fail fast when it
     * is absent or in the bootloader.  ~2 s budget by default; override with
     * RX888_OPEN_RETRY_MS=0 to restore the old fail-fast behaviour. */
    int budget_ms = 2000;
    const char *env = getenv("RX888_OPEN_RETRY_MS");
    if (env) { int v = atoi(env); if (v >= 0) budget_ms = v; }

    libusb_device_handle *h = NULL;
    for (int waited = 0; ; waited += 200) {
        h = libusb_open_device_with_vid_pid(ctx, RX888_VID, RX888_PID_APP);
        if (h) break;

        uint16_t pid = 0;
        int present = rx888_present(ctx, &pid);
        if (present && pid == RX888_PID_APP && waited < budget_ms) {
            usleep(200000);   /* 200 ms — permission rule still settling */
            continue;
        }
        /* Absent, in bootloader, or budget exhausted — report and give up. */
        if (present && pid == RX888_PID_BOOT) {
            fprintf(stderr, "error: device found in bootloader mode (PID 0x%04X) — flash firmware first\n",
                    RX888_PID_BOOT);
        } else {
            fprintf(stderr, "error: no RX888 device found (VID 0x%04X, PID 0x%04X)\n",
                    RX888_VID, RX888_PID_APP);
        }
        return NULL;
    }

    /* Claim interface 0, retrying briefly on BUSY/ACCESS: a second host actor
     * (a udev-spawned prober, a stale process, or another tool) may hold the
     * interface for a moment during the same enumeration window (#143). */
    int r = 0;
    for (int attempt = 0; ; attempt++) {
        if (libusb_kernel_driver_active(h, 0) == 1)
            libusb_detach_kernel_driver(h, 0);
        r = libusb_claim_interface(h, 0);
        if (r == 0) break;
        if ((r == LIBUSB_ERROR_BUSY || r == LIBUSB_ERROR_ACCESS) && attempt < 10) {
            usleep(200000);   /* 200 ms — let the other actor release */
            continue;
        }
        fprintf(stderr, "error: claim interface: %s\n", libusb_strerror(r));
        libusb_close(h);
        return NULL;
    }

    /* Restart the XHCI endpoint ring for EP1-IN.
     *
     * When the previous process closed its USB fd, the kernel killed
     * pending URBs via xhci_urb_dequeue → Set TR Dequeue Pointer,
     * which leaves the XHCI endpoint in the "stopped" state.  New TDs
     * submitted by this process won't be processed until the endpoint
     * is restarted.  libusb_clear_halt sends CLEAR_FEATURE(ENDPOINT_HALT)
     * to the device AND calls usb_hcd_reset_endpoint which issues a
     * Reset Endpoint command to the XHCI — clearing the stopped state.
     *
     * The firmware CLEAR_FEATURE handler now just ACKs the setup
     * (no stall-clear, no DMA teardown), so this is safe. */
    libusb_clear_halt(h, 0x81);  /* EP1-IN — restart XHCI endpoint ring */

    return h;
}

void close_rx888(libusb_device_handle *h)
{
    if (h) {
        libusb_release_interface(h, 0);
        libusb_close(h);
    }
}
