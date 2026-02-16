/*
 * USB_handler.c
 *
 *  Created on: 21/set/2020
 *
 *  HF103_FX3 project
 *  Author: Oscar Steila
 *  modified from: SuperSpeed Device Design By Example - John Hyde
 *
 *  https://sdr-prototypes.blogspot.com/
 */

#include "Application.h"

#include "Si5351.h"

#include "radio.h"

// Declare external functions
extern void CheckStatus(char* StringPtr, CyU3PReturnStatus_t Status);
extern void StartApplication(void);
extern void StopApplication(void);
extern CyU3PReturnStatus_t StartGPIF(void);
extern CyBool_t GpifPreflightCheck(void);  /* StartStopApplication.c */
extern const CyU3PGpifConfig_t CyFxGpifConfig;
extern CyU3PReturnStatus_t SetUSBdescriptors(uint8_t hwconfig);

// Declare external data
extern CyU3PQueue glEventAvailable;			  	// Used for threads communications
extern CyBool_t glIsApplnActive;				// Set true once device is enumerated
extern uint8_t  glHWconfig;       			    // Hardware config
extern uint16_t  glFWconfig;       			    // Firmware config hb.lb

extern CyBool_t glFlagDebug;
extern volatile uint16_t glDebTxtLen;
extern uint8_t glBufDebug[MAXLEN_D_USB];
extern uint32_t glCounter[20];
extern uint16_t glLastPibArg;
extern uint32_t glDMACount;



#define CYFX_SDRAPP_MAX_EP0LEN  64      /* Max. data length supported for EP0 requests. */

extern CyU3PDmaMultiChannel glMultiChHandleSlFifoPtoU;
// Global data owned by this module
uint8_t  *glEp0Buffer = 0;              /* Buffer used to handle vendor specific control requests. */
uint8_t  glVendorRqtCnt = 0;

#ifdef TRACESERIAL

extern CyU3PQueue glEventAvailable;	  	// Used for threads communications
extern uint32_t glQevent __attribute__ ((aligned (32)));
extern void ConsoleAccumulateChar(char ch);

/* Trace function */
void
TraceSerial( uint8_t  bRequest, uint8_t * pdata, uint16_t wValue, uint16_t wIndex)
{
	if ( bRequest != READINFODEBUG)
	{
		if (bRequest >= FX3_CMD_BASE && (bRequest - FX3_CMD_BASE) < FX3_CMD_COUNT)
			DebugPrint(4, "%s\t", FX3CommandName[bRequest - FX3_CMD_BASE]);
		else
			DebugPrint(4, "0x%x\t", bRequest);
		switch(bRequest)
		{
		case SETARGFX3:
			if (wIndex < SETARGFX3_LIST_COUNT)
				DebugPrint(4, "%s\t%d", SETARGFX3List[wIndex],  wValue );
			else
				DebugPrint(4, "%d\t%d", wIndex, wValue);
			break;

		case GPIOFX3:
			DebugPrint(4, "\t0x%x", * (uint32_t *) pdata);
			break;


		case STARTADC:
			DebugPrint(4, "%d", * (uint32_t *) pdata);
			break;

		case STARTFX3:
		case STOPFX3:
		case RESETFX3:
			break;

		default:
			DebugPrint(4, "0x%x\t0x%x", pdata[0] , pdata[1]);
			break;
		}
		DebugPrint(4, "\r\n\n");
	}
}
#endif

/* USB driver thread context — blocking calls (e.g. CyU3PThreadSleep) are safe. */
CyBool_t
CyFxSlFifoApplnUSBSetupCB (
        uint32_t setupdat0,
        uint32_t setupdat1
    )
{
    /* Fast enumeration is used. Only requests addressed to the interface, class,
     * vendor and unknown control requests are received by this function.
     * This application does not support any class or vendor requests. */

    uint8_t  bRequest, bReqType;
    uint8_t  bType, bTarget;
    uint16_t wValue, wIndex;
    uint16_t wLength;
    CyBool_t isHandled = CyFalse;
    CyU3PReturnStatus_t apiRetStatus;

    /* Decode the fields from the setup request. */
    bReqType = (setupdat0 & CY_U3P_USB_REQUEST_TYPE_MASK);
    bType    = (bReqType & CY_U3P_USB_TYPE_MASK);
    bTarget  = (bReqType & CY_U3P_USB_TARGET_MASK);
    bRequest = ((setupdat0 & CY_U3P_USB_REQUEST_MASK) >> CY_U3P_USB_REQUEST_POS);
    wValue   = ((setupdat0 & CY_U3P_USB_VALUE_MASK)   >> CY_U3P_USB_VALUE_POS);
    wIndex   = ((setupdat1 & CY_U3P_USB_INDEX_MASK)   >> CY_U3P_USB_INDEX_POS);
    wLength   = ((setupdat1 & CY_U3P_USB_LENGTH_MASK)   >> CY_U3P_USB_LENGTH_POS);

    if (bType == CY_U3P_USB_STANDARD_RQT)
    {
        /* Handle SET_FEATURE(FUNCTION_SUSPEND) and CLEAR_FEATURE(FUNCTION_SUSPEND)
         * requests here. It should be allowed to pass if the device is in configured
         * state and failed otherwise. */
        if ((bTarget == CY_U3P_USB_TARGET_INTF) && ((bRequest == CY_U3P_USB_SC_SET_FEATURE)
                    || (bRequest == CY_U3P_USB_SC_CLEAR_FEATURE)) && (wValue == 0))
        {
            if (glIsApplnActive)
                CyU3PUsbAckSetup ();
            else
            {
                CyU3PUsbStall (0, CyTrue, CyFalse);
            }
            isHandled = CyTrue;
        }

        /* CLEAR_FEATURE request for endpoint is always passed to the setup callback
         * regardless of the enumeration model used. When a clear feature is received,
         * the previous transfer has to be flushed and cleaned up. This is done at the
         * protocol level. So flush the EP memory and reset the DMA channel associated
         * with it. If there are more than one EP associated with the channel reset both
         * the EPs. The endpoint stall and toggle / sequence number is also expected to be
         * reset. Return CyFalse to make the library clear the stall and reset the endpoint
         * toggle. Or invoke the CyU3PUsbStall (ep, CyFalse, CyTrue) and return CyTrue.
         * Here we are clearing the stall. */
        if ((bTarget == CY_U3P_USB_TARGET_ENDPT) && (bRequest == CY_U3P_USB_SC_CLEAR_FEATURE)
                && (wValue == CY_U3P_USBX_FS_EP_HALT))
        {
            if (glIsApplnActive)
            {
                if (wIndex == CY_FX_EP_CONSUMER)
                {
                    CyU3PDmaMultiChannelReset (&glMultiChHandleSlFifoPtoU);
                    CyU3PUsbFlushEp(CY_FX_EP_CONSUMER);
                    CyU3PUsbResetEp (CY_FX_EP_CONSUMER);
                    CyU3PDmaMultiChannelSetXfer (&glMultiChHandleSlFifoPtoU,FIFO_DMA_RX_SIZE,0);
                }
                CyU3PUsbStall (wIndex, CyFalse, CyTrue);
                isHandled = CyTrue;
            }
        }
    } else if (bType == CY_U3P_USB_VENDOR_RQT) {

    	isHandled = CyFalse;

    	/* Reject oversized EP0 data before any GetEP0Data call. */
    	if (wLength > CYFX_SDRAPP_MAX_EP0LEN) {
    		CyU3PUsbStall(0, CyTrue, CyFalse);
    		return CyTrue;
    	}

    	switch (bRequest)
    	 {
			case GPIOFX3:
					if(CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL)== CY_U3P_SUCCESS)
					{
						uint32_t mdata = * (uint32_t *) &glEp0Buffer[0];
						rx888r2_GpioSet(mdata);
						isHandled = CyTrue;
					}
					break;

			case STARTADC:
					if(CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL)== CY_U3P_SUCCESS)
					{
						uint32_t freq;
						freq = *(uint32_t *) &glEp0Buffer[0];
						apiRetStatus = si5351aSetFrequencyA(freq);
						if (apiRetStatus == CY_U3P_SUCCESS) {
							CyU3PThreadSleep(1000);
							isHandled = CyTrue;
						} else {
							DebugPrint(4, "STARTADC si5351 failed: %d", apiRetStatus);
							isHandled = CyFalse;
						}
					}
					break;

			case GETSTATS:
				{
					CyU3PMemSet(glEp0Buffer, 0, CYFX_SDRAPP_MAX_EP0LEN);
					uint8_t gpifState = 0xFF;
					CyU3PGpifGetSMState(&gpifState);
					uint16_t off = 0;
					memcpy(&glEp0Buffer[off], &glDMACount, 4);   off += 4;
					glEp0Buffer[off++] = gpifState;
					memcpy(&glEp0Buffer[off], &glCounter[0], 4); off += 4;
					memcpy(&glEp0Buffer[off], &glLastPibArg, 2); off += 2;
					memcpy(&glEp0Buffer[off], &glCounter[1], 4); off += 4;
					memcpy(&glEp0Buffer[off], &glCounter[2], 4); off += 4;
					{
						uint8_t si_status = 0;
						I2cTransfer(0x00, 0xC0, 1, &si_status, CyTrue); /* Si5351 reg 0 */
						glEp0Buffer[off++] = si_status;                  /* [19] */
					}
					CyU3PUsbSendEP0Data(off, glEp0Buffer);
					isHandled = CyTrue;
				}
				break;

		case I2CWFX3:
					apiRetStatus  = CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL);
					if (apiRetStatus == CY_U3P_SUCCESS)
						{
							apiRetStatus = I2cTransfer ( wIndex, wValue, wLength, glEp0Buffer, CyFalse);
							if (apiRetStatus == CY_U3P_SUCCESS)
								isHandled = CyTrue;
							else
							{
								CyU3PDebugPrint (4, "I2cwrite Error %d\n", apiRetStatus);
								isHandled = CyFalse;
							}
						}
					break;

			case I2CRFX3:
					CyU3PMemSet (glEp0Buffer, 0, CYFX_SDRAPP_MAX_EP0LEN);
					apiRetStatus = I2cTransfer (wIndex, wValue, wLength, glEp0Buffer, CyTrue);
					if (apiRetStatus == CY_U3P_SUCCESS)
					{
						apiRetStatus = CyU3PUsbSendEP0Data(wLength, glEp0Buffer);
						isHandled = CyTrue;
					}
					break;
			case SETARGFX3:
					CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL);
					switch(wIndex) {
						case DAT31_ATT:
							rx888r2_SetAttenuator(wValue);
							glVendorRqtCnt++;
							isHandled = CyTrue;
							break;
						case AD8370_VGA:
							rx888r2_SetGain(wValue);
							glVendorRqtCnt++;
							isHandled = CyTrue;
							break;
						default:
							/* Data phase already ACKed; stall status to
							   signal the unrecognized wIndex to the host. */
							CyU3PUsbStall(0, CyTrue, CyFalse);
							isHandled = CyTrue;
							break;
					}
				break;

    	 	case STARTFX3:
					CyU3PUsbLPMDisable();
    	 		    CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL);
				/*
				 * Preflight: verify the ADC clock is running before
				 * starting the GPIF state machine.  The SM is clocked
				 * by the Si5351 output; without it the SM will wedge
				 * in a read state with no timeout-based recovery.
				 * See GpifPreflightCheck() in StartStopApplication.c
				 * and si5351_pll_locked() in Si5351.c for details.
				 */
				if (!GpifPreflightCheck()) {
					/* Data phase already ACKed by GetEP0Data;
					 * stall status phase so host sees rejection. */
					CyU3PUsbStall(0, CyTrue, CyFalse);
					isHandled = CyTrue;
					break;
				}
				CyU3PGpifDisable(CyTrue);   /* force-stop SM in case it's stuck */
    	 		 	CyU3PDmaMultiChannelReset (&glMultiChHandleSlFifoPtoU);
					apiRetStatus = CyU3PDmaMultiChannelSetXfer (&glMultiChHandleSlFifoPtoU, FIFO_DMA_RX_SIZE,0);
					if (apiRetStatus == CY_U3P_SUCCESS)
					{
						apiRetStatus = StartGPIF();  /* reload waveform + SMStart */
					if (apiRetStatus == CY_U3P_SUCCESS)
					{
						CyU3PGpifControlSWInput ( CyTrue );
						isHandled = CyTrue;
					}
					}
					{ uint8_t _s=0xFF; CyU3PGpifGetSMState(&_s);
					  DebugPrint(4,"\r\nGO s=%d r=%d",_s,apiRetStatus); }
					break;

			case STOPFX3:
					CyU3PUsbLPMEnable();
				    CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL);
					CyU3PGpifControlSWInput(CyFalse);  /* deassert FW_TRG before disable */
					CyU3PGpifDisable(CyTrue);   /* force-stop GPIF SM immediately */
					/* Do NOT call CyU3PGpifLoad() here — it re-enables the GPIF
					 * block, causing the SM to auto-advance.  STARTFX3 will reload
					 * the waveform via StartGPIF() when streaming resumes. */
					CyU3PDmaMultiChannelReset (&glMultiChHandleSlFifoPtoU);
					CyU3PUsbFlushEp(CY_FX_EP_CONSUMER);
					{ uint8_t _s=0xFF; CyU3PGpifGetSMState(&_s);
					  DebugPrint(4,"\r\nSTP s=%d",_s); }
					isHandled = CyTrue;
					break;

			case RESETFX3:	// RESETTING CPU BY PC APPLICATION
				    CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL);
					DebugPrint(4, "\r\n\r\nHOST RESETTING CPU \r\n");
					CyU3PThreadSleep(100);
					CyU3PDeviceReset(CyFalse);
					break;

            		case TESTFX3:
					glEp0Buffer[0] =  glHWconfig;
					glEp0Buffer[1] = (uint8_t) (glFWconfig >> 8);
					glEp0Buffer[2] = (uint8_t) glFWconfig;
					glEp0Buffer[3] = glVendorRqtCnt;
					CyU3PUsbSendEP0Data (4, glEp0Buffer);
					glFlagDebug = (wValue == 1); // debug mode
					glVendorRqtCnt++;
					isHandled = CyTrue;
					break;


	   case READINFODEBUG:
					{
					if (wValue >0)
					{
						char InputChar = (char) wValue;
					 	if (InputChar  == 0x0d)
						{
							glQevent = USER_COMMAND_AVAILABLE << 24;
							CyU3PQueueSend(&glEventAvailable, &glQevent, CYU3P_NO_WAIT);
						}
						else
						{
							ConsoleAccumulateChar(InputChar);
						}
					}
					if (glDebTxtLen > 0)
						{
							uint32_t intMask = CyU3PVicDisableAllInterrupts();
							uint16_t len = glDebTxtLen;
							if (len > CYFX_SDRAPP_MAX_EP0LEN - 1)
								len = CYFX_SDRAPP_MAX_EP0LEN - 1;
							memcpy(glEp0Buffer, glBufDebug, len);
							uint16_t remain = glDebTxtLen - len;
							if (remain > 0)
								memmove(glBufDebug, glBufDebug + len, remain);
							glDebTxtLen = remain;
							CyU3PVicEnableInterrupts(intMask);
							glEp0Buffer[len] = '\0';
							CyU3PUsbSendEP0Data (len + 1, glEp0Buffer);
							glVendorRqtCnt++;
							isHandled = CyTrue;
						}
					else
						{
							isHandled = CyTrue;
							CyU3PUsbStall (0, CyTrue, CyFalse);
						}
					}

					break;

            default: /* unknown request, stall the endpoint. */

					isHandled = CyFalse;
					CyU3PDebugPrint (4, "STALL EP0 V.REQ %x\n",bRequest);
					CyU3PUsbStall (0, CyTrue, CyFalse);
					break;
    	}
    	TraceSerial( bRequest, (uint8_t *) &glEp0Buffer[0], wValue, wIndex);
    }
    return isHandled;
}


/* USB driver thread context — blocking calls are safe. */
void USBEventCallback ( CyU3PUsbEventType_t evtype, uint16_t evdata)
{
	uint32_t event = evtype;
	CyU3PQueueSend(&glEventAvailable, &event, CYU3P_NO_WAIT);
    switch (evtype)
    {
        case CY_U3P_USB_EVENT_SETCONF:
            /* Stop the application before re-starting. */
            if (glIsApplnActive) StopApplication ();
            	StartApplication ();
            break;

        case CY_U3P_USB_EVENT_CONNECT:
        	break;
        case CY_U3P_USB_EVENT_RESET:
        case CY_U3P_USB_EVENT_DISCONNECT:
            if (glIsApplnActive)
            {
            	StopApplication ();
              	CyU3PUsbLPMEnable ();
            }
            break;

        case CY_U3P_USB_EVENT_EP_UNDERRUN:
        	glCounter[2]++;
        	DebugPrint (4, "\r\nEP Underrun on %d", evdata);
            break;

        case CY_U3P_USB_EVENT_EP0_STAT_CPLT:
               /* Make sure the bulk pipe is resumed once the control transfer is done. */
            break;

        default:
            break;
    }
}
/* USB driver thread context.
   Invoked on U0 -> U1/U2 state change.  Return CyTrue to stay in low-power
   state, CyFalse to immediately exit back to U0.  This application always
   allows U1/U2 transitions. */
CyBool_t  LPMRequestCallback ( CyU3PUsbLinkPowerMode link_mode)
{
	return CyTrue;
}



// Spin up USB, let the USB driver handle enumeration
CyU3PReturnStatus_t InitializeUSB(uint8_t hwconfig)
{
	CyU3PReturnStatus_t Status;
	CyBool_t NeedToRenumerate = CyTrue;
    /* Allocate a buffer for handling control requests. */
    glEp0Buffer = (uint8_t *)CyU3PDmaBufferAlloc (CYFX_SDRAPP_MAX_EP0LEN);

	Status = CyU3PUsbStart();

    if (Status == CY_U3P_ERROR_NO_REENUM_REQUIRED)
    {
    	NeedToRenumerate = CyFalse;
    	Status = CY_U3P_SUCCESS;
    	DebugPrint(4,"\r\nNeedToRenumerate = CyFalse");
    }
    CheckStatus("Start USB Driver", Status);

  // Setup callbacks to handle the setup requests, USB Events and LPM Requests (for USB 3.0)
    CyU3PUsbRegisterSetupCallback(CyFxSlFifoApplnUSBSetupCB, CyTrue);
    CyU3PUsbRegisterEventCallback(USBEventCallback);
    CyU3PUsbRegisterLPMRequestCallback( LPMRequestCallback );

    // Driver needs all of the descriptors so it can supply them to the host when requested
    Status = SetUSBdescriptors(hwconfig);
    CheckStatus("Set USB Descriptors", Status);
    // Connect the USB Pins with SuperSpeed operation enabled
    if (NeedToRenumerate)
    {
		  Status = CyU3PConnectState(CyTrue, CyTrue);
		  CheckStatus("ConnectUSB", Status);

    }
    else	// USB connection already exists, restart the Application
    {
    	if (glIsApplnActive) StopApplication();
    	StartApplication();
    }

	return Status;
}
