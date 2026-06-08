/*
 * fx3_core.h — shared diagnostic/operator command core for the SDDC_FX3
 * host tools.
 *
 * These are the "operator diagnostics" primitives: probe, poke, recover, and
 * the read-only status reads.  They depend only on the USB transport
 * (fx3_usb.h), the GETSTATS decoder (fx3_stats.h), and the vendor-protocol
 * constants (fx3_proto.h) — never on the firmware regression/fuzz/soak
 * harness.  Both the diagnostics CLI and the firmware test harness link this
 * core (issue #139 modularization; fx3_cmd split).
 */
#ifndef FX3_CORE_H
#define FX3_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <libusb-1.0/libusb.h>

/* --- Firmware upload (via rx888_stream) --- */
int find_rx888_stream(char *out, size_t out_size);
int upload_firmware(libusb_context *ctx, const char *fw_path);

/* --- Vendor-command primitives --- */
int do_test(libusb_device_handle *h);
int do_gpio(libusb_device_handle *h, uint32_t bits);
int do_adc(libusb_device_handle *h, uint32_t freq);
int do_att(libusb_device_handle *h, uint16_t val);
int do_vga(libusb_device_handle *h, uint16_t val);
int do_wdg_max(libusb_device_handle *h, uint16_t val);
int do_start(libusb_device_handle *h);
int do_stop(libusb_device_handle *h);
int do_i2cr(libusb_device_handle *h, uint16_t addr, uint16_t reg, uint16_t len);
int do_i2cw(libusb_device_handle *h, uint16_t addr, uint16_t reg,
            const uint8_t *data, uint16_t len);
int do_reset(libusb_device_handle *h);
int do_usbreset(libusb_context *ctx);
int do_reload(libusb_context *ctx, const char *fw);
int do_raw(libusb_device_handle *h, uint8_t code);

/* --- Read-only diagnostic status reads (borderline #3: useful on the
 *     bench, also exercised by the harness). --- */
int do_test_stack_check(libusb_device_handle *h);
int do_test_stats_pll(libusb_device_handle *h);
int do_test_stats_shdn(libusb_device_handle *h);

#endif /* FX3_CORE_H */
