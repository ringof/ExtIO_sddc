/*
 * fx3_bulk.h — bulk (EP1-IN) read helpers for the SDDC_FX3 host tools.
 * Extracted from fx3_cmd.c (issue #139).
 */
#pragma once

#include <libusb-1.0/libusb.h>

struct primed_xfer_state {
    int completed;       /* set to 1 by callback */
    int actual_length;   /* bytes transferred     */
    int status;          /* libusb_transfer_status */
};

/* Completion callback for async bulk transfers; user_data must point at a
 * struct primed_xfer_state.  Exposed so scenarios that build their own
 * libusb_transfer (e.g. dma_count_monotonic) can reuse it. */
void LIBUSB_CALL primed_xfer_cb(struct libusb_transfer *xfer);

int bulk_read_some(libusb_device_handle *h, int len, int timeout_ms);
int primed_start_and_read(libusb_device_handle *h, int len, int timeout_ms);
int primed_start_and_read_retry(libusb_device_handle *h, int len, int timeout_ms);
