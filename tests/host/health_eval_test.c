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
CyBool_t si5351_clk0_enabled(void) { return CyTrue; }
CyBool_t si5351_pll_locked(void) { return CyTrue; }

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

    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "all checks passed", g_fail);
    return g_fail ? 1 : 0;
}
