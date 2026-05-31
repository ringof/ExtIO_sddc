/*
 * fx3_bulk.c — bulk (EP1-IN) read helpers for the SDDC_FX3 host tools.
 * Extracted verbatim from fx3_cmd.c (issue #139).
 */
#include "fx3_bulk.h"
#include "fx3_usb.h"
#include "fx3_proto.h"

#include <stdlib.h>
#include <unistd.h>

/* Try to read some bulk data from EP1 IN.  Returns the number of bytes
 * actually received, or a negative libusb error code.  A timeout (no
 * data within timeout_ms) returns 0. */
int bulk_read_some(libusb_device_handle *h, int len, int timeout_ms)
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

/* ---- Primed (async) start-and-read --------------------------------
 *
 * Race-free alternative to cmd_u32(STARTFX3) + bulk_read_some().
 *
 * At 32 MS/s the four 16 KB DMA buffers fill in ~1 ms.  If the host
 * hasn't submitted a bulk TD by then, PIB overflows force the xHCI
 * endpoint into an error state and all subsequent reads return -EIO.
 * rx888_stream avoids this by pre-submitting 32 async transfers BEFORE
 * sending STARTFX3.  This helper does the same thing for the test
 * harness:
 *
 *   1. libusb_submit_transfer()   — queue one async bulk read TD
 *   2. cmd_u32(STARTFX3, 0)       — start GPIF; data lands in the TD
 *   3. libusb_handle_events()     — wait for completion
 *
 * Note: libusb_clear_halt is NOT called here.  Between clean stop/start
 * cycles the endpoint is not halted, and clear_halt on a non-stalled EP
 * corrupts USB controller ERDY state via the firmware's CLEAR_FEATURE
 * handler.  The retry variant (primed_start_and_read_retry) calls
 * clear_halt between attempts to recover from genuine endpoint errors.
 *
 * Returns bytes received (>= 0) or a negative libusb error code.
 * On success the caller still owns the STOPFX3 responsibility. */


void LIBUSB_CALL primed_xfer_cb(struct libusb_transfer *xfer)
{
    struct primed_xfer_state *st = xfer->user_data;
    st->actual_length = xfer->actual_length;
    st->status        = xfer->status;
    st->completed     = 1;
}

int primed_start_and_read(libusb_device_handle *h,
                                 int len, int timeout_ms)
{
    uint8_t *buf = malloc(len);
    if (!buf) return LIBUSB_ERROR_NO_MEM;

    struct libusb_transfer *xfer = libusb_alloc_transfer(0);
    if (!xfer) { free(buf); return LIBUSB_ERROR_NO_MEM; }

    struct primed_xfer_state st = { .completed = 0 };

    /* Note: do NOT call libusb_clear_halt here.  Between clean stop/start
     * cycles the endpoint is not halted, and clear_halt on a non-stalled
     * endpoint triggers CyU3PUsbStall(CyFalse, CyTrue) in the firmware's
     * CLEAR_FEATURE handler, which corrupts USB controller ERDY state
     * after data has flowed.  The clear_halt at device open (open_rx888)
     * handles the initial xHCI endpoint reset; error recovery is handled
     * by the retry variant below. */

    /* 1. Fill and submit async bulk transfer BEFORE starting GPIF */
    libusb_fill_bulk_transfer(xfer, h, EP1_IN, buf, len,
                              primed_xfer_cb, &st, timeout_ms);
    int r = libusb_submit_transfer(xfer);
    if (r < 0) {
        libusb_free_transfer(xfer);
        free(buf);
        return r;
    }

    /* 2. Start GPIF — data flows into the already-queued TD */
    r = cmd_u32(h, STARTFX3, 0);
    if (r < 0) {
        libusb_cancel_transfer(xfer);
        /* Drain the cancelled transfer so libusb doesn't leak it */
        while (!st.completed)
            libusb_handle_events_completed(g_ctx, &st.completed);
        libusb_free_transfer(xfer);
        free(buf);
        return r;
    }

    /* 3. Wait for the bulk transfer to complete */
    while (!st.completed)
        libusb_handle_events_completed(g_ctx, &st.completed);

    libusb_free_transfer(xfer);
    free(buf);

    if (st.status == LIBUSB_TRANSFER_COMPLETED)
        return st.actual_length;

    /* H4 fix: timeout with 0 bytes means the device didn't produce any
     * data — return LIBUSB_ERROR_TIMEOUT so the retry variant fires its
     * STOP + clear_halt + retry recovery.  Previously this returned 0,
     * which looked like "success" (r >= 0) and bypassed recovery entirely.
     * Timeout with partial data (actual_length > 0) is a valid short
     * read — return the byte count so callers can use the data. */
    if (st.status == LIBUSB_TRANSFER_TIMED_OUT)
        return (st.actual_length > 0) ? st.actual_length
                                      : LIBUSB_ERROR_TIMEOUT;

    /* Map transfer status to a libusb error code */
    switch (st.status) {
    case LIBUSB_TRANSFER_ERROR:    return LIBUSB_ERROR_IO;
    case LIBUSB_TRANSFER_STALL:    return LIBUSB_ERROR_PIPE;
    case LIBUSB_TRANSFER_OVERFLOW: return LIBUSB_ERROR_OVERFLOW;
    case LIBUSB_TRANSFER_NO_DEVICE:return LIBUSB_ERROR_NO_DEVICE;
    case LIBUSB_TRANSFER_CANCELLED:return LIBUSB_ERROR_INTERRUPTED;
    default:                       return LIBUSB_ERROR_OTHER;
    }
}

/* Retry variant: retries primed_start_and_read on transient USB errors
 * (timeout / IO), same escalation as cmd_u32_retry.  Use for the first
 * STARTFX3 in a scenario after potential watchdog recovery. */
int primed_start_and_read_retry(libusb_device_handle *h,
                                       int len, int timeout_ms)
{
    int r = primed_start_and_read(h, len, timeout_ms);
    if (r >= 0 || (r != LIBUSB_ERROR_TIMEOUT && r != LIBUSB_ERROR_IO))
        return r;
    /* First retry — previous attempt may have started GPIF (STARTFX3
     * succeeded but bulk read failed).  Stop streaming, clear the
     * xHCI endpoint error state, then retry. */
    cmd_u32(h, STOPFX3, 0);
    usleep(500000);
    libusb_clear_halt(h, EP1_IN);
    r = primed_start_and_read(h, len, timeout_ms);
    if (r >= 0 || (r != LIBUSB_ERROR_TIMEOUT && r != LIBUSB_ERROR_IO))
        return r;
    /* Second retry */
    cmd_u32(h, STOPFX3, 0);
    usleep(1000000);
    libusb_clear_halt(h, EP1_IN);
    return primed_start_and_read(h, len, timeout_ms);
}
