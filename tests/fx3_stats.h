/*
 * fx3_stats.h — GETSTATS decoding for the SDDC_FX3 host tools.
 * Extracted from fx3_cmd.c (issue #139).
 */
#pragma once

#include <stdint.h>
#include <libusb-1.0/libusb.h>

#define GETSTATS_LEN  30    /* bumped 26 -> 30: adds gpio_state [26..29] for
                             * #131 observability. Strict-length check means
                             * flashing the new firmware is required after a
                             * host update — paired flash. */

/* GETSTATS response layout (firmware: SDDC_FX3/USBHandler.c GETSTATS handler,
 * GETSTATS_PAYLOAD_LEN is its single source of truth — keep in sync):
 *   [0..3]   uint32  DMA buffer completions
 *   [4]      uint8   GPIF state machine state
 *   [5..8]   uint32  PIB error count
 *   [9..10]  uint16  last PIB error arg
 *   [11..14] uint32  I2C failure count
 *   [15..18] uint32  Streaming fault count (EP underruns + watchdog recoveries)
 *   [19]     uint8   Si5351 status register (reg 0)
 *   [20..23] uint32  Firmware boot counter (mismatch = reset between reads)
 *   [24]     uint8   Si5351 CLK0_CONTROL reg (bit 7 = CLK0_PDN: 0=on, 1=off)
 *   [25]     uint8   si5351_clk0_enabled() result (1 = firmware sees CLK0 on)
 *   [26..29] uint32  GPIO state — bit positions match enum GPIOPin in
 *                    protocol.h (SHDWN, DITH, RANDO, BIAS_HF, BIAS_VHF,
 *                    LED_BLUE, VHF_EN, PGA_EN). PGA is un-inverted here so
 *                    the readback matches control-word semantics.
 */
struct fx3_stats {
    uint32_t dma_count;
    uint8_t  gpif_state;
    uint32_t pib_errors;
    uint16_t last_pib_arg;
    uint32_t i2c_failures;
    uint32_t streaming_faults;
    uint8_t  si5351_status;
    uint32_t boot_count;       /* Increments once per firmware boot.  Mismatch
                                * across two snapshots = device reset between
                                * them (HWDT, CyU3PDeviceReset, RESETFX3, or
                                * power cycle). */
    uint8_t  clk0_reg16;       /* Si5351 CLK0_CONTROL register (16) raw value.
                                * Bit 7 = CLK0_PDN (0=on, 1=off). */
    uint8_t  clk0_result;      /* Boolean returned by si5351_clk0_enabled():
                                * 1 if firmware sees CLK0 as enabled (i.e.
                                * reg16 bit 7 == 0), 0 otherwise. */
    uint32_t gpio_state;       /* Steady-state GPIO snapshot, bit positions
                                * match the GPIOFX3 control word (enum
                                * GPIOPin in protocol.h). PGA is un-inverted
                                * — a host can do `if (gs & SHDWN)` etc. */
};

int read_stats(libusb_device_handle *h, struct fx3_stats *s);
int do_stats(libusb_device_handle *h);
