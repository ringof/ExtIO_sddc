/*
 * fx3_stats.c — GETSTATS decoding for the SDDC_FX3 host tools.
 * Extracted verbatim from fx3_cmd.c (issue #139).
 */
#include "fx3_stats.h"
#include "fx3_usb.h"
#include "fx3_proto.h"

#include <stdio.h>
#include <string.h>

int read_stats(libusb_device_handle *h, struct fx3_stats *s)
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
    memcpy(&s->boot_count, &buf[20], 4);
    s->clk0_reg16  = buf[24];
    s->clk0_result = buf[25];
    memcpy(&s->gpio_state, &buf[26], 4);
    return 0;
}

/* Read and display GETSTATS fields */
int do_stats(libusb_device_handle *h)
{
    struct fx3_stats s;
    int r = read_stats(h, &s);
    if (r < 0) {
        printf("FAIL stats: %s\n", libusb_strerror(r));
        return 1;
    }
    printf("PASS stats: dma=%u gpif=%u pib=%u last_pib=0x%04X i2c=%u faults=%u pll=0x%02X boot=%u clk0_reg16=0x%02X clk0_result=%u gpio=0x%05X\n",
           s.dma_count, s.gpif_state, s.pib_errors,
           s.last_pib_arg, s.i2c_failures, s.streaming_faults,
           s.si5351_status, s.boot_count, s.clk0_reg16, s.clk0_result,
           s.gpio_state);
    return 0;
}
