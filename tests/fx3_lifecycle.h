/*
 * fx3_lifecycle.h — host-side enumeration / open-close-claim race tests
 * for the SDDC_FX3 firmware (issue #143).
 *
 *   reopen_race_storm — tight close/reopen churn (kernel URB-dequeue + XHCI
 *                       ring restart + firmware CLEAR_FEATURE).
 *   two_actor_open    — a second process hammers EP0 while the first streams.
 *
 * Both assert the firmware never resets (boot_count steady) and keeps
 * streaming.  The host permission-settle window itself is absorbed by
 * open_rx888()'s bounded retry (fx3_usb).
 */
#pragma once

#include <libusb-1.0/libusb.h>

/* Takes **h: each close/reopen replaces the handle, propagated back for the
 * caller's cleanup. */
int do_test_reopen_race_storm(libusb_device_handle **h_inout);
int do_test_two_actor_open(libusb_device_handle *h);
