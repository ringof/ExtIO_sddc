/*
 * fx3_proto.h — SDDC_FX3 vendor-protocol constants for the host tools.
 *
 * Mirrors SDDC_FX3/protocol.h.  Extracted from fx3_cmd.c (issue #139) so
 * the test modules (fx3_usb, fx3_stats, fx3_bulk, fx3_fuzz, fx3_cmd) share
 * one definition.
 */
#pragma once

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
#define HANGFX3       0xCE   /* TEST-ONLY: sleep wValue ms in EP0 handler;
                              * used by test_health_recovery to trip the
                              * firmware health watchdog (#104, #105). */
#define HANGMAIN      0xCF   /* TEST-ONLY: signal main thread to spin
                              * forever on next iteration; used by
                              * test_main_recovery to trip the FX3
                              * hardware watchdog (Level 5). */

/* GPIOFX3 control-word bit positions (subset — mirrors enum GPIOPin in
 * SDDC_FX3/protocol.h). GETSTATS gpio_state field [26..29] uses the same
 * positions, so the same constants apply to both write (GPIOFX3) and
 * read (GETSTATS.gpio_state). */
#define SHDWN         (1U << 5)
#define LED_BLUE      (1U << 11)

/* SETARGFX3 argument IDs */
#define DAT31_ATT     10
#define AD8370_VGA    11
#define WDG_MAX_RECOV 14

/* Timeouts */
#define CTRL_TIMEOUT_MS  1000

/* EP1 IN (bulk consumer endpoint) */
#define EP1_IN  0x81

/* Max EP0 data length the firmware accepts (USBHandler.c
 * CYFX_SDRAPP_MAX_EP0LEN).  A vendor request with wLength above this must
 * STALL — the protocol fuzzer uses it to label "oversize" attempts. */
#define FX3_MAX_EP0LEN  64
