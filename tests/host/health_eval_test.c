/*
 * health_eval_test.c — host unit test for SDDC_FX3/health.c's
 * health_evaluate() cascade-ordering invariant.
 *
 * Guards the #115 review fix (and the threading risk it addressed):
 *
 *   When an EP0 vendor handler is already overdue, health_evaluate() MUST
 *   return HEALTH_WEDGED_EP0 *without* invoking any streaming-sampler
 *   SDK/debug call (CyU3PGpifGetSMState / DebugPrint).  The wedged vendor
 *   callback may itself be stuck inside an SDK call holding a lock, so the
 *   Level-4 reset path must make no such call first — otherwise it can
 *   contend and stall the reset.
 *
 * This is a black-box-proof-resistant, firmware-internal ordering bug that
 * the on-hardware soak cannot reliably reach; a host unit test pins the
 * invariant deterministically.  Builds the *real* health.c (no FX3 SDK /
 * hardware needed) via the BladeRF-style LOGGER_HOST pattern — see #117.
 *
 *   make -C tests check-health
 */
#define HEALTH_HOST_TEST 1

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

#include "health_host_shim.h"

/* ---- controllable mock state + instrumentation ------------------------ */
static uint32_t g_now_ms;          /* value CyU3PGetTime() returns          */
static uint8_t  g_sm_state;        /* value CyU3PGpifGetSMState() writes     */
static int      g_getsmstate_calls;
static int      g_debugprint_calls;
static int      g_devicereset_calls;
static int      g_clk_ok = 1;      /* si5351_clk0_enabled() returns this */
static int      g_pll_ok = 1;      /* si5351_pll_locked()   returns this */

/* ---- globals health.c declares extern (defined here for the host link) - */
uint32_t glDMACount;
uint32_t glCounter[20];
CyU3PDmaMultiChannel glMultiChHandleSlFifoPtoU;
CyBool_t glIsApplnActive;

/* ---- mock SDK surface -------------------------------------------------- */
uint32_t CyU3PGetTime(void) { return g_now_ms; }

CyU3PReturnStatus_t CyU3PGpifGetSMState(uint8_t *state_p)
{
    g_getsmstate_calls++;
    if (state_p) *state_p = g_sm_state;
    return CY_U3P_SUCCESS;
}

void host_debug_print(int prio, const char *fmt, ...)
{
    (void)prio; (void)fmt;
    g_debugprint_calls++;
}

void CyU3PDeviceReset(CyBool_t warmReset) { (void)warmReset; g_devicereset_calls++; }

CyU3PReturnStatus_t CyU3PGpifControlSWInput(CyBool_t s) { (void)s; return CY_U3P_SUCCESS; }
CyU3PReturnStatus_t CyU3PGpifDisable(CyBool_t f) { (void)f; return CY_U3P_SUCCESS; }
CyU3PReturnStatus_t CyU3PGpifSMStart(uint8_t a, uint8_t b) { (void)a; (void)b; return CY_U3P_SUCCESS; }
CyU3PReturnStatus_t CyU3PDmaMultiChannelReset(CyU3PDmaMultiChannel *h) { (void)h; return CY_U3P_SUCCESS; }
CyU3PReturnStatus_t CyU3PDmaMultiChannelSetXfer(CyU3PDmaMultiChannel *h, uint32_t c, uint16_t s)
{ (void)h; (void)c; (void)s; return CY_U3P_SUCCESS; }
CyU3PReturnStatus_t CyU3PUsbFlushEp(uint8_t e) { (void)e; return CY_U3P_SUCCESS; }
CyU3PReturnStatus_t CyU3PThreadSleep(uint32_t m) { (void)m; return CY_U3P_SUCCESS; }
CyU3PReturnStatus_t CyU3PSysWatchDogConfigure(CyBool_t e, uint32_t p) { (void)e; (void)p; return CY_U3P_SUCCESS; }
void CyU3PSysWatchDogClear(void) {}
CyU3PReturnStatus_t CyU3PTimerCreate(CyU3PTimer *t, void (*cb)(uint32_t),
                                     uint32_t a, uint32_t i, uint32_t r, uint32_t o)
{ (void)t; (void)cb; (void)a; (void)i; (void)r; (void)o; return CY_U3P_SUCCESS; }
CyBool_t si5351_clk0_enabled(void) { return g_clk_ok; }
CyBool_t si5351_pll_locked(void) { return g_pll_ok; }

/* ---- unit under test --------------------------------------------------- */
#include "health.c"

/* ---- tiny TAP-ish harness ---------------------------------------------- */
static int g_fail;
#define CHECK(cond, msg) do {                              \
        if (cond) { printf("ok   - %s\n", (msg)); }        \
        else      { printf("FAIL - %s\n", (msg)); g_fail++; } \
    } while (0)

static void reset_counts(void)
{
    g_getsmstate_calls = 0;
    g_debugprint_calls = 0;
    g_devicereset_calls = 0;
}

/* Clear all streaming-session health state + mocks for a fresh test group. */
static void reset_session(void)
{
    health_record_event(HEALTH_EVENT_EP0_HANDLER_EXIT);  /* clear EP0 in-progress */
    health_reset_recovery_count();   /* recovery count + both counters + fault + escalated + prevDMA */
    health_set_reset_escalation(CyTrue);
    g_clk_ok = 1; g_pll_ok = 1;
    glIsApplnActive = CyTrue;
    glDMACount = 0;
    g_now_ms = 0;
    reset_counts();
}

/* Drive a cold-start detection (SM active, DMA frozen at 0) to completion;
 * returns the final status (should be HEALTH_WEDGED_STREAMING) and leaves
 * the internal fault kind = COLD_START. */
static health_status_t detect_cold_start(void)
{
    glIsApplnActive = CyTrue; g_sm_state = GPIF_TH0_WAIT; glDMACount = 0;
    health_status_t st = HEALTH_OK;
    for (int i = 0; i < COLD_START_WEDGE_TICKS; i++) st = health_evaluate();
    return st;
}

/* Drive a post-stream stall detection (DMA>0 frozen in BUSY/WAIT); leaves
 * the internal fault kind = PROGRESS_STALL. */
static health_status_t detect_progress_stall(void)
{
    glIsApplnActive = CyTrue; g_sm_state = GPIF_TH1_WAIT; glDMACount = 7;
    health_evaluate();                 /* prime prevDMACount -> 7 */
    health_status_t st = HEALTH_OK;
    for (int i = 0; i < STREAMING_STALL_TICKS; i++) st = health_evaluate();
    return st;
}

int main(void)
{
    /* EP0_HANDLER_TIMEOUT_MS (2000) comes from the included health.c. */

    /* ---- TEST 1: EP0 overdue must short-circuit before the sampler ----
     * Set up state where, IF the streaming sampler ran, it WOULD call
     * CyU3PGpifGetSMState() (frozen DMA, count > 0) and DebugPrint() (SM in
     * a BUSY/WAIT state).  Then assert neither is touched once EP0 is
     * overdue. */
    glIsApplnActive = CyTrue;
    g_sm_state = GPIF_TH0_WAIT;   /* would trip the stall path if sampled */
    glDMACount = 5;

    /* Prime the sampler's prevDMACount to 5 via one EP0-healthy tick
     * (else-branch: no GetSMState call yet). */
    g_now_ms = 0;
    reset_counts();
    (void)health_evaluate();
    CHECK(g_getsmstate_calls == 0, "prime tick takes the else-branch (no GetSMState yet)");

    /* Make an EP0 handler overdue; keep DMA frozen at the primed value so
     * the sampler's stall branch would fire if it were reached. */
    g_now_ms = 0;
    health_record_event(HEALTH_EVENT_EP0_HANDLER_ENTER);  /* enter_ms = 0 */
    g_now_ms = 2001;                                      /* 2001 >= 2000 */
    glDMACount = 5;                                       /* == prevDMACount, > 0 */
    reset_counts();
    health_status_t st = health_evaluate();
    CHECK(st == HEALTH_WEDGED_EP0,  "EP0 overdue -> returns HEALTH_WEDGED_EP0");
    CHECK(g_getsmstate_calls == 0,  "EP0 overdue -> CyU3PGpifGetSMState NOT called (no SDK call before reset)");
    CHECK(g_debugprint_calls == 0,  "EP0 overdue -> DebugPrint NOT called (no debug call before reset)");
    CHECK(g_devicereset_calls == 0, "health_evaluate() never resets by itself");

    /* ---- TEST 2: sampler still detects a streaming wedge when EP0 is ok -
     * Guards against 'fixed the ordering but broke detection'. */
    health_record_event(HEALTH_EVENT_EP0_HANDLER_EXIT);   /* clear EP0 */
    glIsApplnActive = CyTrue;
    g_sm_state = GPIF_TH1_WAIT;
    glDMACount = 9;
    g_now_ms = 3000;
    reset_counts();
    (void)health_evaluate();               /* else-branch: prevDMACount -> 9 */
    health_status_t s1 = health_evaluate();/* frozen, stall 1/3 */
    health_status_t s2 = health_evaluate();/* frozen, stall 2/3 */
    health_status_t s3 = health_evaluate();/* frozen, stall 3/3 -> WEDGED */
    CHECK(s1 == HEALTH_OK && s2 == HEALTH_OK, "stalls 1,2 -> HEALTH_OK");
    CHECK(s3 == HEALTH_WEDGED_STREAMING,      "3rd frozen tick in BUSY/WAIT -> HEALTH_WEDGED_STREAMING");
    CHECK(g_getsmstate_calls >= 3,            "sampler DID call CyU3PGpifGetSMState while EP0 healthy");

    /* ---- TEST 3: DMA progressing -> healthy, no wedge ---- */
    glDMACount = 100;
    g_now_ms = 4000;
    reset_counts();
    CHECK(health_evaluate() == HEALTH_OK, "advancing DMA -> HEALTH_OK");

    /* ========== #137: GPIF state classification (per-state) ==========
     * Lock the two classifier helpers against a typo or a gpif_states.h
     * regeneration drift: every one of the 10 SM states must land in the
     * intended class.  streaming-active = "left RESET/IDLE" (2..9);
     * busy/wait = the four back-pressure states (5,7,8,9). */
    {
        struct { uint8_t st; const char *name; int active; int busy_wait; } S[] = {
            { GPIF_RESET,     "RESET",     0, 0 },
            { GPIF_IDLE,      "IDLE",      0, 0 },
            { GPIF_TH0_RD,    "TH0_RD",    1, 0 },
            { GPIF_TH1_RD_LD, "TH1_RD_LD", 1, 0 },
            { GPIF_TH0_RD_LD, "TH0_RD_LD", 1, 0 },
            { GPIF_TH0_BUSY,  "TH0_BUSY",  1, 1 },
            { GPIF_TH1_RD,    "TH1_RD",    1, 0 },
            { GPIF_TH1_BUSY,  "TH1_BUSY",  1, 1 },
            { GPIF_TH1_WAIT,  "TH1_WAIT",  1, 1 },
            { GPIF_TH0_WAIT,  "TH0_WAIT",  1, 1 },
        };
        int n = (int)(sizeof(S) / sizeof(S[0]));
        CHECK(n == 10, "classifier: all 10 GPIF states enumerated");
        int active_ok = 1, bw_ok = 1;
        for (int i = 0; i < n; i++) {
            if (gpif_state_is_streaming_active(S[i].st) != S[i].active) {
                printf("#   gpif_state_is_streaming_active(%s) wrong\n", S[i].name);
                active_ok = 0;
            }
            if (gpif_state_is_busy_wait(S[i].st) != S[i].busy_wait) {
                printf("#   gpif_state_is_busy_wait(%s) wrong\n", S[i].name);
                bw_ok = 0;
            }
        }
        CHECK(active_ok, "classifier: gpif_state_is_streaming_active matches all 10 states");
        CHECK(bw_ok,     "classifier: gpif_state_is_busy_wait matches all 10 states");
        /* The two invalid/sentinel reads must classify as neither. */
        CHECK(!gpif_state_is_streaming_active(0xFF) && !gpif_state_is_busy_wait(0xFF),
              "classifier: 0xFF (GetSMState failure sentinel) is neither");
    }

    /* ================= #137: cold-start detection ================= */

    /* C1: DMA==0 but SM IDLE (not streaming) -> never wedges. */
    reset_session();
    g_sm_state = GPIF_IDLE; glDMACount = 0;
    {
        int wedged = 0;
        for (int i = 0; i < COLD_START_WEDGE_TICKS + 2; i++)
            if (health_evaluate() != HEALTH_OK) wedged = 1;
        CHECK(!wedged, "cold-start: DMA==0 while SM IDLE -> never wedges");
    }

    /* C2/C3: DMA==0 + SM active -> OK for the grace window, WEDGED on the Nth tick. */
    reset_session();
    g_sm_state = GPIF_TH0_WAIT; glDMACount = 0;
    {
        int ok_before = 1;
        for (int i = 0; i < COLD_START_WEDGE_TICKS - 1; i++)
            if (health_evaluate() != HEALTH_OK) ok_before = 0;
        CHECK(ok_before, "cold-start: HEALTH_OK during the grace window");
        CHECK(health_evaluate() == HEALTH_WEDGED_STREAMING,
              "cold-start: DMA==0 + active SM past grace -> HEALTH_WEDGED_STREAMING");
    }

    /* C4: cold-start counter clears when DMA finally advances. */
    reset_session();
    g_sm_state = GPIF_TH0_WAIT; glDMACount = 0;
    health_evaluate(); health_evaluate(); health_evaluate();  /* 3/5 */
    glDMACount = 1;                                            /* progress */
    CHECK(health_evaluate() == HEALTH_OK, "cold-start: DMA progress clears the counter");

    /* C5: cold-start counter clears when the SM returns to IDLE. */
    reset_session();
    g_sm_state = GPIF_TH0_WAIT; glDMACount = 0;
    health_evaluate(); health_evaluate(); health_evaluate();  /* 3/5 */
    g_sm_state = GPIF_IDLE;
    CHECK(health_evaluate() == HEALTH_OK, "cold-start: SM->IDLE clears the counter");

    /* C6: EP0 overdue short-circuits the cold-start sampling path too
     * (no GPIF/DebugPrint call before the Level-4 reset). */
    reset_session();
    g_sm_state = GPIF_TH0_WAIT; glDMACount = 0;
    g_now_ms = 0;
    health_record_event(HEALTH_EVENT_EP0_HANDLER_ENTER);   /* enter_ms = 0 */
    g_now_ms = 2001;
    reset_counts();
    {
        health_status_t st = health_evaluate();
        CHECK(st == HEALTH_WEDGED_EP0, "EP0 overdue + DMA==0/active SM -> HEALTH_WEDGED_EP0");
        CHECK(g_getsmstate_calls == 0, "EP0 overdue (cold-start setup) -> no CyU3PGpifGetSMState");
        CHECK(g_debugprint_calls == 0, "EP0 overdue (cold-start setup) -> no DebugPrint");
    }

    /* ================= #137: reset escalation ================= */
    /* cap=2 so 3 health_recover() calls reach cap exhaustion. */

    /* E1: not yet at cap -> no reset. */
    reset_session(); health_set_max_recovery(2);
    detect_cold_start(); reset_counts();
    health_recover(HEALTH_WEDGED_STREAMING);   /* count 0->1 */
    health_recover(HEALTH_WEDGED_STREAMING);   /* count 1->2 */
    CHECK(g_devicereset_calls == 0, "escalation: no reset before cap exhausted");

    /* E5: cap exhausted + HW healthy + enabled + COLD_START -> exactly one reset. */
    health_recover(HEALTH_WEDGED_STREAMING);   /* count 2 >= cap -> escalate */
    CHECK(g_devicereset_calls == 1, "escalation: cap + HW healthy + enabled + cold-start -> device reset");

    /* E6: once per session. */
    health_recover(HEALTH_WEDGED_STREAMING);
    health_recover(HEALTH_WEDGED_STREAMING);
    CHECK(g_devicereset_calls == 1, "escalation: only once per session");

    /* E2: escalation disabled -> no reset. */
    reset_session(); health_set_max_recovery(2); health_set_reset_escalation(CyFalse);
    detect_cold_start(); reset_counts();
    for (int i = 0; i < 4; i++) health_recover(HEALTH_WEDGED_STREAMING);
    CHECK(g_devicereset_calls == 0, "escalation: disabled via knob -> no reset");

    /* E3: clock down -> no reset (fail-closed). */
    reset_session(); health_set_max_recovery(2);
    detect_cold_start(); reset_counts(); g_clk_ok = 0;
    for (int i = 0; i < 4; i++) health_recover(HEALTH_WEDGED_STREAMING);
    CHECK(g_devicereset_calls == 0, "escalation: CLK0 down -> no reset (fail-closed)");

    /* E4: PLL unlocked -> no reset. */
    reset_session(); health_set_max_recovery(2);
    detect_cold_start(); reset_counts(); g_pll_ok = 0;
    for (int i = 0; i < 4; i++) health_recover(HEALTH_WEDGED_STREAMING);
    CHECK(g_devicereset_calls == 0, "escalation: PLL unlocked -> no reset");

    /* E7: post-stream stall (PROGRESS_STALL) never escalates — backpressure
     * / abandoned-stream behaviour is unchanged (cap-and-wait). */
    reset_session(); health_set_max_recovery(2);
    detect_progress_stall(); reset_counts();
    for (int i = 0; i < 4; i++) health_recover(HEALTH_WEDGED_STREAMING);
    CHECK(g_devicereset_calls == 0, "escalation: post-stream stall caps-and-waits, NO reset");

    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "all checks passed", g_fail);
    return g_fail ? 1 : 0;
}
