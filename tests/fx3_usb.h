/*
 * fx3_usb.h — USB transport + device lifecycle for the SDDC_FX3 host tools.
 * Extracted from fx3_cmd.c (issue #139).
 */
#pragma once

#include <stdint.h>
#include <libusb-1.0/libusb.h>

/* Global libusb context — needed by primed_start_and_read() and the soak
 * re-acquire path for libusb_handle_events*.  Set once in main(). */
extern libusb_context *g_ctx;
extern const char *g_firmware_path;   /* set from -F option in main() */

int ctrl_write_u32(libusb_device_handle *h, uint8_t request,
                   uint16_t wValue, uint16_t wIndex, uint32_t val);
int ctrl_write_buf(libusb_device_handle *h, uint8_t request,
                   uint16_t wValue, uint16_t wIndex,
                   const uint8_t *buf, uint16_t len);
int ctrl_read(libusb_device_handle *h, uint8_t request,
              uint16_t wValue, uint16_t wIndex, uint8_t *buf, uint16_t len);
int cmd_u32(libusb_device_handle *h, uint8_t cmd, uint32_t val);
int set_arg(libusb_device_handle *h, uint16_t arg_id, uint16_t arg_val);
int cmd_u32_retry(libusb_device_handle *h, uint8_t cmd, uint32_t val);
int ep0_alive_after_stall(libusb_device_handle *h);
libusb_device_handle *open_rx888(libusb_context *ctx);
void close_rx888(libusb_device_handle *h);
