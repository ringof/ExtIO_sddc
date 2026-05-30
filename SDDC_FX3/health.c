/*
 * health.c — recovery state machine implementation
 *
 * PR 2: EP0 vendor-callback liveness tracking and Level 4
 * (CyU3PDeviceReset) backstop.  Addresses issues #104, #105.
 *
 * PR 3: Level 5 (FX3 hardware watchdog) catastrophic backstop.
 * Catches the failure mode where the main thread itself stops running
 * and Levels 1-4 (which all depend on the main thread to fire) become
 * unreachable.  Health.c is the only place HWDT is configured/petted.
 *
 * See health.h for the contract and docs/archive/PLAN_RECOVERY.md for design notes.
 *
 * The discipline rule: new recovery code MUST extend one of these
 * functions.  Do not invent parallel mechanisms.  Do not add inline
 * cleanup in handlers — record an event here and let health_recover()
 * handle it.  Do not pet HWDT from anywhere outside health_pet().
 */

#include "health.h"
#include "cyu3os.h"
#include "cyu3system.h"
#include "Application.h"   /* DebugPrint, FIFO_DMA_RX_SIZE, CY_FX_EP_CONSUMER, SDK GPIF/DMA/USB APIs */
#include "protocol.h"      /* WDG_MAX_RECOVERY_DEFAULT */
#include "Si5351.h"        /* si5351_clk0_enabled / si5351_pll_locked (recovery gate) */
#include "gpif_states.h"   /* GPIF_TH0_BUSY etc. */

/* Streaming-pipeline globals owned elsewhere, inspected and driven by the
 * Level-1 streaming watchdog migrated into health_recover() (issue #115). */
extern uint32_t glDMACount;                            /* StartStopApplication.c */
extern uint32_t glCounter[20];                         /* DebugConsole.c */
extern CyU3PDmaMultiChannel glMultiChHandleSlFifoPtoU; /* StartStopApplication.c */
extern CyBool_t glIsApplnActive;                       /* RunApplication.c */

/* If an EP0 vendor callback runs longer than this, declare it wedged
 * and reset the device.  Legitimate handlers complete in <100 ms
 * (longest observed: STOPFX3 soft-stop ~5 ms incl. 1 ms ThreadSleep).
 * Host's libusb_control_transfer times out at 1000 ms (CTRL_TIMEOUT_MS
 * in tests/fx3_cmd.c), so 2000 ms gives ~1 s after host gives up
 * before we hard-reset. */
#define EP0_HANDLER_TIMEOUT_MS  2000

/* Phase 3 of the HWDT bisection.
 *
 * Phase 1 (caabaf4, both off): 1-hour soak clean.  Confirmed touching
 *   the WD register is the trigger.
 * Phase 2 (178f93d, Configure off / Clear from main loop): planned but
 *   superseded — Infineon KB223337 documents a fundamentally different
 *   pattern than what we were testing.
 * Phase 3 (this commit): implement the KB-documented pattern verbatim.
 *
 * Per KB223337 the watchdog is petted by a ThreadX timer callback,
 * NOT by the main thread.  Pet frequency is ~3 sec, WD period ~5 sec
 * (our previous broken approach used 100 ms / 10 sec — pet was 30x
 * more often than recommended).  The clock-domain story explains why
 * register-write frequency might matter: with no external 32 KHz,
 * the WD clock is derived from SYS_CLK, the same clock driving
 * GPIF/DMA.  Per-pet contention scales with pet frequency.
 *
 * Set HEALTH_HWDT_ENABLED=0 to skip all WD activity (matches the
 * Phase 1 clean baseline). */
#define HEALTH_HWDT_ENABLED      1

#define HWDT_PERIOD_MS           5000   /* Per KB223337. */
#define HWDT_CLEAR_PERIOD_MS     3000   /* Per KB223337; must be < HWDT_PERIOD_MS. */

/* Accumulated state inspected by health_evaluate().  Written by the
 * USB callback thread, read by the main loop.  Single-byte/word
 * accesses are atomic on ARM926; volatile prevents compiler reordering.
 * Ordering rule: writers update timestamp BEFORE setting in_progress,
 * so a reader never sees in_progress=1 with a stale enter_ms. */
static struct {
    volatile uint32_t ep0_handler_enter_ms;
    volatile uint8_t  ep0_handler_in_progress;
} glHealthState = { 0, 0 };

/* Boot counter — incremented once per firmware boot in health_init().
 * Exposed via GETSTATS so the host can detect mid-test device resets
 * (Level 4 CyU3PDeviceReset, Level 5 HWDT, manual RESETFX3, or power
 * cycle).  No NVRAM persistence; cold boot resets to 0. */
static uint32_t glBootCount = 0;

/* Main-loop heartbeat — bumped by health_main_heartbeat() on every
 * RunApplication iteration.  The WD-clear timer callback reads this
 * to decide whether to pet HWDT.  If main hangs (e.g. via HANGMAIN),
 * heartbeat freezes, timer stops petting, HWDT fires within
 * HWDT_PERIOD_MS. */
static volatile uint32_t glMainHeartbeat = 0;

/* HANGMAIN test flag — set by the vendor handler in USBHandler.c,
 * read at the top of each main-loop iteration in RunApplication.c. */
volatile uint8_t glHealthHangMain = 0;

/* Level-1 streaming-watchdog recovery cap (migrated from RunApplication.c,
 * issue #115).  glWdgMaxRecovery: 0 = unlimited, else stop attempting after
 * N consecutive recoveries until health_reset_recovery_count() (STARTFX3 /
 * STOPFX3) begins a fresh session. */
static uint8_t glWdgMaxRecovery = WDG_MAX_RECOVERY_DEFAULT;
static uint8_t glWdgRecoveryCount = 0;

/* Streaming-stall sampler state — was static locals in the inline watchdog.
 * glStallCount counts consecutive ~100 ms ticks with frozen DMA in a
 * BUSY/WAIT state; glPrevDMACount is the previous glDMACount sample. */
static uint32_t glPrevDMACount = 0;
static uint8_t  glStallCount = 0;

/* ThreadX timer used to pet the HWDT.  Per KB223337 the pet must
 * happen from a timer callback context, not from the main thread. */
#if HEALTH_HWDT_ENABLED
static CyU3PTimer glWdClearTimer;
static uint32_t glLastHeartbeatSeen = 0;

static void health_wd_clear_cb(uint32_t param)
{
    (void)param;
    /* Only pet HWDT if the main thread has made progress since last
     * check.  If the heartbeat is stale, deliberately skip the pet —
     * eventually HWDT will fire and reset the device.  This is what
     * makes Level 5 catch main-thread death. */
    uint32_t now = glMainHeartbeat;
    if (now != glLastHeartbeatSeen) {
        glLastHeartbeatSeen = now;
        CyU3PSysWatchDogClear();
    }
}
#endif

void health_main_heartbeat(void)
{
    /* Single-word increment; volatile prevents compiler reorder.
     * Wraparound is fine — the timer callback only checks for
     * advance-since-last-call, which is wraparound-safe. */
    glMainHeartbeat++;
}

void health_init(void)
{
    glHealthState.ep0_handler_enter_ms = 0;
    glHealthState.ep0_handler_in_progress = 0;

    /* Increment first thing so the count reflects "this is boot N".
     * Exposed via health_boot_count() / GETSTATS for reset detection. */
    glBootCount++;

#if HEALTH_HWDT_ENABLED
    /* Level 5 — enable the FX3 hardware watchdog and create a
     * ThreadX timer to pet it.  Per Infineon KB223337 the pet must
     * come from a timer callback (not the main thread), and at a
     * rate well below the WD period.  Requires CyU3PDeviceInit to
     * have already run (it has — StartUp.c invokes it before us). */
    CyU3PSysWatchDogConfigure(CyTrue, HWDT_PERIOD_MS);
    (void)CyU3PTimerCreate(&glWdClearTimer,
                           health_wd_clear_cb,
                           0,                       /* callback arg (unused) */
                           HWDT_CLEAR_PERIOD_MS,    /* initialTicks */
                           HWDT_CLEAR_PERIOD_MS,    /* rescheduleTicks (periodic) */
                           CYU3P_AUTO_ACTIVATE);
#endif
}

uint32_t health_boot_count(void)
{
    return glBootCount;
}

void health_record_event(health_event_t event)
{
    /* O(1).  Safe from any thread context including USB callbacks
     * and timer ISRs.  All branches must remain non-blocking. */
    switch (event) {
        case HEALTH_EVENT_EP0_HANDLER_ENTER:
            /* Order matters: stamp time first, then raise flag, so a
             * reader observing in_progress=1 always sees a fresh
             * enter_ms (volatile blocks compiler reorder). */
            glHealthState.ep0_handler_enter_ms = CyU3PGetTime();
            glHealthState.ep0_handler_in_progress = 1;
            break;
        case HEALTH_EVENT_EP0_HANDLER_EXIT:
            glHealthState.ep0_handler_in_progress = 0;
            break;
    }
}

void health_set_max_recovery(uint8_t max)
{
    glWdgMaxRecovery = max;
}

void health_reset_recovery_count(void)
{
    glWdgRecoveryCount = 0;
}

/* Level-1 streaming-stall detector.  Runs once per health_evaluate() tick
 * (~10 Hz).  Samples glDMACount and the GPIF SM state, maintaining a
 * 3-strike counter; returns CyTrue when the pipeline has been stuck in a
 * BUSY/WAIT state with no DMA progress for 3 consecutive samples (~300 ms).
 * The remedy lives in health_recover(HEALTH_WEDGED_STREAMING).  Migrated
 * verbatim from the inline watchdog in RunApplication.c (issue #115); the
 * `curDMA > 0` guard means a cold-start wedge (first buffer never arrives)
 * is not yet detected — see #119 / #137. */
static CyBool_t streaming_wedge_detected(void)
{
    uint32_t curDMA = glDMACount;

    if (curDMA == glPrevDMACount && curDMA > 0)
    {
        uint8_t gpifState = 0xFF;
        CyU3PGpifGetSMState(&gpifState);

        if (gpifState == GPIF_TH0_BUSY || gpifState == GPIF_TH1_BUSY ||
            gpifState == GPIF_TH1_WAIT || gpifState == GPIF_TH0_WAIT)
        {
            glStallCount++;
            DebugPrint(4, "\r\nWDG: stall %d/3 SM=%d DMA=%u",
                glStallCount, gpifState, curDMA);
            if (glStallCount >= 3)  /* 300ms in BUSY/WAIT */
            {
                glStallCount = 0;
                return CyTrue;
            }
        }
        else
        {
            if (glStallCount > 0)
                DebugPrint(4, "\r\nWDG: stall cleared SM=%d (was %d/3)",
                    gpifState, glStallCount);
            glStallCount = 0;
        }
    }
    else
    {
        if (glStallCount > 0)
            DebugPrint(4, "\r\nWDG: DMA resumed (%u->%u), stall cleared",
                glPrevDMACount, curDMA);
        glStallCount = 0;
        glPrevDMACount = curDMA;
    }
    return CyFalse;
}

health_status_t health_evaluate(void)
{
    /* Level 1 — advance the streaming-stall sampler every tick (only while
     * streaming is active).  Sampled before the EP0 check so its state
     * machine keeps advancing regardless of EP0 status. */
    CyBool_t streaming_wedged =
        glIsApplnActive ? streaming_wedge_detected() : CyFalse;

    /* Level 4 — EP0 handler duration vs timeout.  Takes precedence: an EP0
     * deadlock resets the whole device, so it wins over a streaming wedge. */
    if (glHealthState.ep0_handler_in_progress) {
        uint32_t now = CyU3PGetTime();
        uint32_t enter = glHealthState.ep0_handler_enter_ms;
        /* Unsigned subtraction is wraparound-safe for any handler that
         * actually completes within ~24 days (CyU3PGetTime ms rollover). */
        if ((now - enter) >= EP0_HANDLER_TIMEOUT_MS) {
            return HEALTH_WEDGED_EP0;
        }
    }

    if (streaming_wedged) {
        return HEALTH_WEDGED_STREAMING;
    }

    return HEALTH_OK;
}

void health_recover(health_status_t status)
{
    /* See docs/archive/PLAN_RECOVERY.md §4 for the documented cascade levels.
     * PR 2 implements Level 4 only.  Future PRs add Levels 2, 3, 5
     * and migrate Level 1 (streaming watchdog). */
    switch (status) {
        case HEALTH_WEDGED_EP0:
            /* Level 4 — full device reset.  The vendor callback thread
             * is deadlocked inside an SDK call; only escape is to drop
             * USB entirely and re-enumerate.  Host will see disconnect
             * + bootloader-PID device; firmware must be re-uploaded.
             * No DebugPrint before reset — UART output is fine but the
             * SDK call may itself be the thing hung, so avoid waiting. */
            CyU3PDeviceReset(CyFalse);
            /* Should not return. */
            break;

        case HEALTH_WEDGED_STREAMING:
            /* Level 1 — streaming pipeline wedged in BUSY/WAIT with no DMA
             * progress.  Soft-stop (force-stop fallback), reset the DMA
             * channel + flush EP, and restart the GPIF SM if the ADC clock
             * is healthy.  Migrated verbatim from RunApplication.c (#115). */
            if (glWdgMaxRecovery > 0 && glWdgRecoveryCount >= glWdgMaxRecovery)
            {
                DebugPrint(4, "\r\nWDG: recovery limit (%d), waiting for STARTFX3",
                    glWdgMaxRecovery);
                return;
            }
            {
                CyU3PReturnStatus_t rc_reset, rc_flush;
                CyU3PReturnStatus_t rc_xfer = 0, rc_sm = 0;
                CyBool_t hw_ok;
                uint8_t smState = 0xFF;
                uint32_t curDMA = glDMACount;

                CyU3PGpifGetSMState(&smState);
                glWdgRecoveryCount++;
                DebugPrint(4, "\r\nWDG: === RECOVERY START === SM=%d DMA=%u recov=%d/%d",
                    smState, curDMA, glWdgRecoveryCount, glWdgMaxRecovery);
                /* Soft-stop: deassert FW_TRG so the SM exits to IDLE via the
                 * !FW_TRG transitions.  Caveat: soft-stop needs the external
                 * clock to advance the SM; if the Si5351 is dead or PLL
                 * unlocked the SM can't transition and soft-stop fails — fall
                 * back to force-stop. */
                CyU3PGpifControlSWInput(CyFalse);
                CyU3PThreadSleep(1);
                smState = 0xFF;
                CyU3PGpifGetSMState(&smState);
                if (smState == GPIF_IDLE) {
                    CyU3PGpifDisable(CyFalse);
                } else {
                    DebugPrint(4, "\r\nWDG: soft-stop fail SM=%d, forcing", smState);
                    CyU3PGpifDisable(CyTrue);
                }

                rc_reset = CyU3PDmaMultiChannelReset(&glMultiChHandleSlFifoPtoU);
                rc_flush = CyU3PUsbFlushEp(CY_FX_EP_CONSUMER);

                /* Only restart streaming if the ADC clock is healthy — a
                 * soft-stop with a dead clock leaves the SM unable to move. */
                hw_ok = si5351_clk0_enabled() && si5351_pll_locked();
                if (hw_ok)
                {
                    rc_xfer = CyU3PDmaMultiChannelSetXfer(
                        &glMultiChHandleSlFifoPtoU, FIFO_DMA_RX_SIZE, 0);
                    rc_sm = CyU3PGpifSMStart(0, 0);
                    CyU3PGpifControlSWInput(CyTrue);
                }
                glCounter[2]++;  /* streaming fault counter — GETSTATS [15..18];
                                  * also incremented by EP_UNDERRUN in USBHandler.c */
                DebugPrint(4, "\r\nWDG: === RECOVERY %s === rst=%d flush=%d xfer=%d sm=%d",
                    hw_ok ? "DONE" : "WAIT", rc_reset, rc_flush, rc_xfer, rc_sm);
                glPrevDMACount = 0;
                glDMACount = 0;
            }
            break;

        case HEALTH_OK:
        case HEALTH_WEDGED_UNKNOWN:
        default:
            /* No remedy for unknown wedge — deliberately do nothing
             * rather than guess.  Future PRs may add a catch-all
             * remedy (e.g. cascade through Levels 2 -> 3 -> 4) once
             * we have signals that can produce HEALTH_WEDGED_UNKNOWN. */
            return;
    }
}
