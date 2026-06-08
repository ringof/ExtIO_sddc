/*
 * fx3_test_proto.h — harness-only protocol bits NOT in the shared public header.
 *
 * The production FX3 protocol (command codes, SETARG IDs, GPIO bits, USB IDs,
 * CTRL_TIMEOUT_MS) lives in the rx888-tools submodule's include/rx888.h and
 * src/fx3_cmd/fx3_usb.h, which the firmware harness now links as the single
 * source of truth (see docs/fx3_cmd-split-plan.md, Phase 3).
 *
 * These symbols are deliberately kept out of rx888.h because they are specific
 * to the firmware regression/fuzz harness and have no place in a user-facing
 * tools header:
 *   - HANGFX3/HANGMAIN/HANGCOLDSTART  TEST-ONLY firmware fault injectors.
 *   - EP1_IN                          bulk consumer endpoint, used only by the
 *                                     harness's own bulk reader (rx888-tools
 *                                     streams via librx888/rx888_stream, so its
 *                                     core has no fx3_bulk).
 *   - FX3_MAX_EP0LEN                  EP0 wLength ceiling, used by the fuzzer to
 *                                     label oversize attempts.
 */
#ifndef FX3_TEST_PROTO_H
#define FX3_TEST_PROTO_H

/* TEST-ONLY vendor request codes (mirror SDDC_FX3/protocol.h). */
#define HANGFX3       0xCE   /* sleep wValue ms in EP0 handler; trips the
                              * firmware health watchdog (#104, #105). */
#define HANGMAIN      0xCF   /* signal main thread to spin forever on next
                              * iteration; trips the FX3 hardware watchdog. */
#define HANGCOLDSTART 0xD0   /* suppress DMA-progress accounting so the SM runs
                              * but glDMACount stays 0; trips #137 cold-start
                              * detection + reset escalation. */

/* EP1 IN (bulk consumer endpoint). */
#define EP1_IN  0x81

/* Max EP0 data length the firmware accepts (USBHandler.c CYFX_SDRAPP_MAX_EP0LEN).
 * A vendor request with wLength above this must STALL — the protocol fuzzer
 * uses it to label "oversize" attempts. */
#define FX3_MAX_EP0LEN  64

#endif /* FX3_TEST_PROTO_H */
