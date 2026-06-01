/*
 * fx3_lifecycle.c — host-side enumeration / open-close-claim race tests.
 * See fx3_lifecycle.h (issue #143).
 */
#include "fx3_lifecycle.h"
#include "fx3_proto.h"
#include "fx3_usb.h"
#include "fx3_stats.h"
#include "fx3_bulk.h"

#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

/* reopen_race_storm — tight close/reopen storm on the running device.
 *
 * Repeatedly close the USB handle and immediately reopen it (open_rx888's
 * bounded retry absorbs the udev permission-settle window), generating bulk
 * traffic each cycle so the close has live URBs to dequeue.  Each close fires
 * the kernel's URB-dequeue (Set TR Dequeue Pointer) and each reopen the XHCI
 * endpoint-ring restart + firmware CLEAR_FEATURE path — the host-controller
 * edge behaviour a host enumeration race exercises.  Verifies the firmware
 * never resets (boot_count steady), stays RX888r2 (hwconfig 0x04), and still
 * streams afterward.
 *
 * The per-cycle read is a *bounded synchronous* bulk_read_some, never an async
 * primed_start_and_read: a churn-induced endpoint wedge (e.g. the documented
 * clear_halt-on-non-stalled-EP corruption that every reopen risks) would leave
 * an async transfer's completion pending forever, hanging the test.  The sync
 * read always returns within its timeout, so a wedge surfaces as a clean FAIL
 * (no data / unresponsive) rather than a hang.  Progress dots localize any
 * stall to a specific cycle.
 *
 * Takes **h because each reopen replaces the handle; the final live handle is
 * propagated back so main()'s cleanup closes it exactly once.  CLI-only
 * (handle churn makes it unsafe inside the `!` console or the soak rotation). */
int do_test_reopen_race_storm(libusb_device_handle **h_inout)
{
    libusb_device_handle *h = *h_inout;

    struct fx3_stats before;
    int have_before = (read_stats(h, &before) == 0);

    /* Program the ADC clock once — the Si5351 keeps running across USB
     * close/reopen, so STARTFX3's preflight stays satisfied each cycle. */
    cmd_u32(h, STARTADC, 32000000);

    const int CYCLES = 20;
    printf("# reopen_race_storm: %d close/reopen cycles ", CYCLES);
    fflush(stdout);
    for (int i = 0; i < CYCLES; i++) {
        /* Generate live URBs for the close to dequeue.  Bounded sync read. */
        cmd_u32(h, STARTFX3, 0);
        bulk_read_some(h, 16384, 150);          /* result raced — ignore */
        cmd_u32(h, STOPFX3, 0);

        close_rx888(h);
        h = open_rx888(g_ctx);                   /* bounded EACCES/BUSY retry */
        if (!h) {
            printf("\nFAIL reopen_race_storm: reopen failed at cycle %d "
                   "(device gone or stuck mid-enumeration)\n", i);
            *h_inout = NULL;
            return 1;
        }
        *h_inout = h;
        putchar('.');
        fflush(stdout);
    }
    printf("\n");

    /* Survival: EP0 alive, hwconfig intact, no firmware reset. */
    uint8_t info[4] = {0};
    int r = ctrl_read(h, TESTFX3, 0, 0, info, 4);
    if (r < 0) {
        printf("FAIL reopen_race_storm: device unresponsive after %d cycles: %s\n",
               CYCLES, libusb_strerror(r));
        return 1;
    }
    if (info[0] != 0x04) {
        printf("FAIL reopen_race_storm: hwconfig=0x%02X (expected 0x04)\n", info[0]);
        return 1;
    }
    struct fx3_stats after;
    if (read_stats(h, &after) < 0) {
        printf("FAIL reopen_race_storm: GETSTATS failed after churn\n");
        return 1;
    }
    if (have_before && after.boot_count != before.boot_count) {
        printf("FAIL reopen_race_storm: boot_count %u->%u (device reset during churn)\n",
               before.boot_count, after.boot_count);
        return 1;
    }

    /* Streaming still flows? */
    cmd_u32(h, STARTADC, 32000000);
    int got = primed_start_and_read_retry(h, 16384, 2000);
    cmd_u32(h, STOPFX3, 0);
    if (got <= 0) {
        printf("FAIL reopen_race_storm: no data after churn (got=%d)\n", got);
        return 1;
    }

    printf("PASS reopen_race_storm: %d close/reopen cycles, device healthy, "
           "%d bytes flowed (boot=%u steady)\n", CYCLES, got, after.boot_count);
    return 0;
}

/* two_actor_open — a second process opens the device and hammers EP0 while
 * this process streams, modeling a udev-spawned prober / second daemon racing
 * the streamer.  The child uses its OWN libusb context (never share one across
 * fork) and does not claim interface 0 (the parent holds it — a claim would
 * get BUSY); it issues EP0 control reads, which the kernel routes without an
 * interface claim.  Verifies the firmware survives concurrent host access:
 * the parent's stream/EP0 stay alive and the device does not reset. */
int do_test_two_actor_open(libusb_device_handle *h)
{
    struct fx3_stats before;
    int have_before = (read_stats(h, &before) == 0);

    /* Parent brings up the stream and keeps GPIF running. */
    cmd_u32(h, STARTADC, 32000000);
    int got0 = primed_start_and_read_retry(h, 16384, 2000);
    if (got0 <= 0) {
        printf("FAIL two_actor_open: parent stream did not start (got=%d)\n", got0);
        cmd_u32(h, STOPFX3, 0);
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        cmd_u32(h, STOPFX3, 0);
        return 1;
    }
    if (pid == 0) {
        /* Child: fresh context, second handle, hammer EP0, then exit. */
        libusb_context *cctx = NULL;
        if (libusb_init(&cctx) != 0) _exit(2);
        libusb_device_handle *ch =
            libusb_open_device_with_vid_pid(cctx, RX888_VID, RX888_PID_APP);
        if (!ch) { libusb_exit(cctx); _exit(3); }
        int fails = 0;
        for (int i = 0; i < 150; i++) {
            uint8_t b[GETSTATS_LEN];
            int got = (i & 1) ? GETSTATS_LEN : 4;
            int r = libusb_control_transfer(ch,
                LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
                (i & 1) ? GETSTATS : TESTFX3, 0, 0, b, got, 500);
            if (r < 0) fails++;
        }
        /* Claim attempt: expected BUSY while the parent holds interface 0. */
        if (libusb_claim_interface(ch, 0) == 0)
            libusb_release_interface(ch, 0);
        libusb_close(ch);
        libusb_exit(cctx);
        _exit(fails > 120 ? 4 : 0);   /* mostly-failing 2nd-actor EP0 = problem */
    }

    /* Parent: keep reading the live stream + polling EP0 while the child runs. */
    int parent_misses = 0;
    for (int i = 0; i < 30; i++) {
        if (bulk_read_some(h, 16384, 500) <= 0) parent_misses++;
        struct fx3_stats s;
        if (read_stats(h, &s) < 0) parent_misses++;
    }

    int status = 0;
    waitpid(pid, &status, 0);
    cmd_u32(h, STOPFX3, 0);

    /* Verify the parent + device survived the concurrent access. */
    int r = ep0_alive_after_stall(h);
    if (r < 0) {
        printf("FAIL two_actor_open: parent EP0 dead after concurrent access: %s\n",
               libusb_strerror(r));
        return 1;
    }
    struct fx3_stats after;
    if (read_stats(h, &after) < 0) {
        printf("FAIL two_actor_open: GETSTATS failed after concurrent access\n");
        return 1;
    }
    if (have_before && after.boot_count != before.boot_count) {
        printf("FAIL two_actor_open: boot_count %u->%u (device reset under contention)\n",
               before.boot_count, after.boot_count);
        return 1;
    }
    int child_rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (child_rc == 4) {
        printf("FAIL two_actor_open: 2nd-actor EP0 access mostly failed "
               "(child rc=4) — concurrent open not survivable\n");
        return 1;
    }

    printf("PASS two_actor_open: survived concurrent 2nd-actor EP0 access "
           "(child rc=%d, parent_stream_misses=%d/30, boot=%u steady)\n",
           child_rc, parent_misses, after.boot_count);
    return 0;
}
