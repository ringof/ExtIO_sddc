/*
 * health_host_shim.h — host-build shim for SDDC_FX3/health.c.
 *
 * Lets the *real* health.c compile and link on a workstation with no FX3
 * SDK, by stubbing the SDK / DebugPrint surface it uses.  The mock
 * *implementations* (and their call-counting instrumentation) live in the
 * test program (tests/host/health_eval_test.c).
 *
 * Mirrors the BladeRF LOGGER_HOST pattern (see issue #117).  health.c
 * includes this only under -DHEALTH_HOST_TEST; the firmware build is
 * unaffected (it pulls the real SDK headers instead).
 */
#ifndef HEALTH_HOST_SHIM_H
#define HEALTH_HOST_SHIM_H

#include "cyu3types.h"    /* CyBool_t, CyTrue/CyFalse, CyU3PReturnStatus_t */
#include "protocol.h"     /* WDG_MAX_RECOVERY_DEFAULT (pure macros) */
#include "gpif_states.h"  /* GPIF_* state indices (pure macros) */

/* Opaque SDK handle types health.c only ever takes the address of. */
typedef struct { int _unused; } CyU3PTimer;
typedef struct { int _unused; } CyU3PDmaMultiChannel;

#define CYU3P_AUTO_ACTIVATE 1

/* Surface normally provided by Application.h. */
#define FIFO_DMA_RX_SIZE   0
#define CY_FX_EP_CONSUMER  0x81

/* SDK calls health.c makes — declared here, mocked in the test program. */
uint32_t            CyU3PGetTime(void);
CyU3PReturnStatus_t CyU3PGpifGetSMState(uint8_t *state_p);
CyU3PReturnStatus_t CyU3PGpifControlSWInput(CyBool_t set);
CyU3PReturnStatus_t CyU3PGpifDisable(CyBool_t forceReload);
CyU3PReturnStatus_t CyU3PGpifSMStart(uint8_t startState, uint8_t alpha);
CyU3PReturnStatus_t CyU3PDmaMultiChannelReset(CyU3PDmaMultiChannel *h);
CyU3PReturnStatus_t CyU3PDmaMultiChannelSetXfer(CyU3PDmaMultiChannel *h,
                                                uint32_t count, uint16_t sckId);
CyU3PReturnStatus_t CyU3PUsbFlushEp(uint8_t ep);
CyU3PReturnStatus_t CyU3PThreadSleep(uint32_t ms);
void                CyU3PDeviceReset(CyBool_t warmReset);
CyU3PReturnStatus_t CyU3PSysWatchDogConfigure(CyBool_t enable, uint32_t periodMs);
void                CyU3PSysWatchDogClear(void);
CyU3PReturnStatus_t CyU3PTimerCreate(CyU3PTimer *timer, void (*cb)(uint32_t),
                                     uint32_t cbArg, uint32_t initialTicks,
                                     uint32_t rescheduleTicks, uint32_t option);

/* si5351 recovery-gate helpers (normally SDDC_FX3/driver/Si5351.h). */
CyBool_t si5351_clk0_enabled(void);
CyBool_t si5351_pll_locked(void);

/* DebugPrint — route to the instrumented mock in the test program. */
void host_debug_print(int prio, const char *fmt, ...);
#define DebugPrint(prio, ...) host_debug_print((prio), __VA_ARGS__)

#include "health.h"   /* the contract health.c implements */

#endif /* HEALTH_HOST_SHIM_H */
