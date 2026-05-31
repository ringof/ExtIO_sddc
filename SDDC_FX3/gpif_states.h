/*
 * gpif_states.h — symbolic names for the GPIF II state-machine indices.
 *
 * These MUST mirror the state-index #defines emitted into the generated
 * SDDC_GPIF.h by the Cypress GPIF II Designer (see SDDC_GPIF.h, the
 * "Mapping of user defined state names to state indices" block).
 *
 * SDDC_GPIF.h also defines the waveform / register tables, so it may be
 * #included only once in the firmware (StartStopApplication.c). This
 * sibling header carries ONLY the state-index macros, so any module that
 * inspects CyU3PGpifGetSMState() — the streaming watchdog in
 * RunApplication.c, the STOPFX3 / STARTADC stop paths in USBHandler.c —
 * can name the states without dragging in those table definitions.
 *
 * A GPIF_ prefix is used (rather than the bare RESET/IDLE/... names in
 * SDDC_GPIF.h) to avoid colliding with the generic identifiers other SDK
 * headers may define.
 *
 * CROSS-CHECK: these values are NOT auto-synced with SDDC_GPIF.h. After
 * every GPIF II Designer regeneration of SDDC_GPIF.h, verify the indices
 * below still match its state-name block.
 */
#ifndef _INCLUDED_GPIF_STATES_H_
#define _INCLUDED_GPIF_STATES_H_

#define GPIF_RESET     0
#define GPIF_IDLE      1
#define GPIF_TH0_RD    2
#define GPIF_TH1_RD_LD 3
#define GPIF_TH0_RD_LD 4
#define GPIF_TH0_BUSY  5
#define GPIF_TH1_RD    6
#define GPIF_TH1_BUSY  7
#define GPIF_TH1_WAIT  8
#define GPIF_TH0_WAIT  9

#endif /* _INCLUDED_GPIF_STATES_H_ */
