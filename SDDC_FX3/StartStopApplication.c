/*
 * StartStopApplication.c
 *
 *  Created on: 21/set/2020
 *
 *  HF103_FX3 project
 *  Author: Oscar Steila
 *  modified from: SuperSpeed Device Design By Example - John Hyde
 *
 *  https://sdr-prototypes.blogspot.com/
 *
 */

#include "SDDC_GPIF.h" // GPIFII include once
#include "Application.h"
#include "Si5351.h"
uint32_t glDMACount;
uint16_t glLastPibArg;

// Declare external functions
extern void CheckStatus(char* StringPtr, CyU3PReturnStatus_t Status);


// Declare external data
extern CyBool_t glIsApplnActive;				// Set true once device is enumerated
extern uint32_t glCounter[20];

// Global data owned by this module


void StartApplication(void);
void StopApplication(void);

const char* glBusSpeed[] = { "Not Connected", "Full ", "High ", "Super" };
char* glCyFxGpifName = { "HF103.h" };

CyU3PDmaMultiChannel glMultiChHandleSlFifoPtoU;   /* DMA Channel handle for P2U transfer. */

extern CyU3PQueue glEventAvailable;

void PibErrorCallback(CyU3PPibIntrType cbType, uint16_t cbArg) {
	if (cbType == CYU3P_PIB_INTR_ERROR)
	{
		glCounter[0]++;
		glLastPibArg = cbArg;
		uint32_t evt = (2 << 24) | cbArg;
		CyU3PQueueSend(&glEventAvailable, &evt, CYU3P_NO_WAIT);
	}
}
/* Callback funtion for the DMA event notification. */
void DmaCallback (
        CyU3PDmaChannel   *chHandle, /* Handle to the DMA channel. */
        CyU3PDmaCbType_t  type,      /* Callback type.             */
        CyU3PDmaCBInput_t *input)    /* Callback status.           */
{
    if (type == CY_U3P_DMA_CB_PROD_EVENT)
    {
        /* This is a produce event notification to the CPU. This notification is
         * received upon reception of every buffer. The DMA transfer will not wait
         * for the commit from CPU. Increment the counter. */
        glDMACount++;
    }
}


/*
 * GpifPreflightCheck — verify hardware preconditions before starting
 * the GPIF state machine.
 *
 * The GPIF II state machine in this design is synchronous: it clocks
 * data in on edges of the external ADC sample clock, which is generated
 * by the Si5351 PLL.  If the SM is started without that clock, it will
 * stall in a read state waiting for DATA_CNT_HIT, which never fires.
 * The SM has no state-count timeout (STATE_COUNT_CONFIG = 0), so the
 * wedge is permanent — only CyU3PGpifDisable(CyTrue) can recover it.
 *
 * This function is called from any code path that is about to assert
 * FW_TRG and begin data flow (currently: the STARTFX3 vendor command).
 * It is intentionally NOT called from StartApplication(), because that
 * path loads the SM into IDLE where it waits for FW_TRG — the external
 * clock is not needed until FW_TRG transitions the SM into read states.
 *
 * Today this only checks the Si5351 PLL lock.  Future checks (e.g. DMA
 * health, VBUS level) can be added here without changing call sites.
 *
 * Returns CyTrue  if all checks pass and GPIF may be started.
 * Returns CyFalse if any check fails — caller must NOT start GPIF.
 */
CyBool_t GpifPreflightCheck(void)
{
	if (!si5351_clk0_enabled()) {
		DebugPrint(4, "\r\nPreflight FAIL: ADC clock not enabled");
		return CyFalse;
	}
	if (!si5351_pll_locked()) {
		DebugPrint(4, "\r\nPreflight FAIL: Si5351 PLL_A not locked");
		return CyFalse;
	}
	return CyTrue;
}

CyU3PReturnStatus_t StartGPIF(void)
{
	CyU3PReturnStatus_t Status;
	DebugPrint(4, "\r\nGPIF file %s", glCyFxGpifName);
	Status = CyU3PGpifLoad(&CyFxGpifConfig);
	CheckStatus("GpifLoad", Status);
	Status = CyU3PGpifSMStart (0, 0); //START, ALPHA_START);
	return Status;
}

void StartApplication ( void ) {

    CyU3PEpConfig_t epCfg;
    CyU3PPibClock_t pibClock;
    CyU3PDmaMultiChannelConfig_t dmaMultiConfig;
    CyU3PReturnStatus_t Status = CY_U3P_SUCCESS;
    CyU3PUSBSpeed_t usbSpeed = CyU3PUsbGetSpeed();
    // Display the enumerated device bus speed
    DebugPrint(4, "\r\n@StartApplication, running at %sSpeed", glBusSpeed[usbSpeed]);

    // Start GPIF clocks, they need to be running before we attach a DMA channel to GPIF
    pibClock.clkDiv = 2;
    pibClock.clkSrc = CY_U3P_SYS_CLK;
    pibClock.isHalfDiv = CyFalse;
    pibClock.isDllEnable = CyFalse;  // Disable Dll since this application is synchronous
    Status = CyU3PPibInit(CyTrue, &pibClock);
    CheckStatus("Start GPIF Clock", Status);

    CyU3PMemSet ((uint8_t *)&epCfg, 0, sizeof (epCfg));
    epCfg.enable = CyTrue;
    epCfg.epType = CY_U3P_USB_EP_BULK;
    epCfg.burstLen = ENDPOINT_BURST_LENGTH;
    epCfg.streams = 0;
    epCfg.pcktSize = ENDPOINT_BURST_SIZE;
    epCfg.isoPkts = 0;   /* bulk endpoint — isoPkts is for ISO EPs only */

    glDMACount= 0;
    glCounter[0] = glCounter[1] = glCounter[2] = 0;
    glLastPibArg = 0;
    /* Consumer endpoint configuration */
    Status = CyU3PSetEpConfig(CY_FX_EP_CONSUMER, &epCfg);
    CheckStatus("CyU3PSetEpConfig Consumer", Status);
    CyU3PUsbFlushEp(CY_FX_EP_CONSUMER);


	dmaMultiConfig.size           = DMA_BUFFER_SIZE;
	dmaMultiConfig.count          = DMA_BUFFER_COUNT;
	dmaMultiConfig.validSckCount  = 2;
	dmaMultiConfig.prodSckId [0]  = (CyU3PDmaSocketId_t)PING_PRODUCER_SOCKET;
	dmaMultiConfig.prodSckId [1]  = (CyU3PDmaSocketId_t)PONG_PRODUCER_SOCKET;
	dmaMultiConfig.consSckId [0]  = (CyU3PDmaSocketId_t)CONSUMER_USB_SOCKET;
	dmaMultiConfig.dmaMode        = CY_U3P_DMA_MODE_BYTE;
	//     dmaMultiConfig.notification   = CY_U3P_DMA_CB_CONS_EVENT;

    /* Create a DMA AUTO channel for P2U transfer. */
	dmaMultiConfig.notification   = CY_U3P_DMA_CB_PROD_EVENT;
	dmaMultiConfig.cb = (CyU3PDmaMultiCallback_t) DmaCallback;
	Status = CyU3PDmaMultiChannelCreate (&glMultiChHandleSlFifoPtoU,
		   CY_U3P_DMA_TYPE_AUTO_MANY_TO_ONE, &dmaMultiConfig);
	CheckStatus("CyU3PDmaMultiChannelCreate", Status);
    Status = CyU3PDmaMultiChannelSetXfer (&glMultiChHandleSlFifoPtoU,
    	   FIFO_DMA_RX_SIZE,0);  /* DMA transfer size is set to infinite */
    CheckStatus("CyU3PDmaMultiChannelSetXfer", Status);

    /* callback to see if there is any overflow of data on the GPIF II side*/
    CyU3PPibRegisterCallback(PibErrorCallback, CYU3P_PIB_INTR_ERROR);

	// Load, configure and start the GPIF state machine
    Status = StartGPIF();
	CheckStatus("GpifStart", Status);
    glIsApplnActive = CyTrue;
}



/* This function stops the slave FIFO loop application. This shall be called
 * whenever a RESET or DISCONNECT event is received from the USB host. The
 * endpoints are disabled and the DMA pipe is destroyed by this function. */
void StopApplication ( void )
{
    CyU3PEpConfig_t epConfig;
    CyU3PReturnStatus_t Status;

    // Disable GPIF, close the DMA channel, flush and disable the endpoint
    CyU3PGpifDisable(CyTrue);
    Status = CyU3PPibDeInit();
    CheckStatus("Stop GPIF Block", Status);
    Status = CyU3PDmaMultiChannelDestroy (&glMultiChHandleSlFifoPtoU);
    CheckStatus("DmaMultiChannelDestroy", Status);

    Status = CyU3PUsbFlushEp(CY_FX_EP_CONSUMER);
    CheckStatus("FlushEndpoint", Status);
    CyU3PMemSet((uint8_t *)&epConfig, 0, sizeof(epConfig));
    Status = CyU3PSetEpConfig(CY_FX_EP_CONSUMER, &epConfig);
	CheckStatus("SetEndpointConfig_Disable", Status);
    // OK, Application is now stopped
	glIsApplnActive  = CyFalse;
}

