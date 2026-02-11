
/*
 *  StartUp.c - set up the CPU environment then start the RTOS
 *  Created on: 21/set/2020
 *
 *  HF103_FX3 project
 *  Author: Oscar Steila
 *  modified from: SuperSpeed Device Design By Example - John Hyde
 *
 *  https://sdr-prototypes.blogspot.com/
 */

#include "Application.h"

#define GPIO_LED_RED_PIN	21	/* FX3 GPIO pin for red LED (RX888mk2) */

void IndicateError(uint16_t ErrorCode)
{
	/*
	 * Try to drive LED_RED via GPIO 21.  This is best-effort:
	 * if the GPIO block hasn't been clocked yet the calls will
	 * fail silently, which is harmless.
	 */
	CyU3PGpioSimpleConfig_t gpioConfig;

	CyU3PDeviceGpioOverride(GPIO_LED_RED_PIN, CyTrue);
	gpioConfig.outValue    = (ErrorCode != 0) ? CyTrue : CyFalse;
	gpioConfig.driveLowEn  = CyTrue;
	gpioConfig.driveHighEn = CyTrue;
	gpioConfig.inputEn     = CyFalse;
	gpioConfig.intrMode    = CY_U3P_GPIO_NO_INTR;
	CyU3PGpioSetSimpleConfig(GPIO_LED_RED_PIN, &gpioConfig);
}



// Main sets up the CPU environment then starts the RTOS
int main (void)
{
    CyU3PIoMatrixConfig_t ioConfig;
    CyU3PReturnStatus_t Status;
    CyU3PSysClockConfig_t clkCfg;

    // The default clock runs at 384MHz, adjust it up to 403MHz so that GPIF can be "100MHz"
	clkCfg.setSysClk400 = CyTrue;   /* FX3 device's master clock is set to a frequency > 400 MHz */
	clkCfg.cpuClkDiv = 2;           /* CPU clock divider */
	clkCfg.dmaClkDiv = 2;           /* DMA clock divider */
	clkCfg.mmioClkDiv = 2;          /* MMIO clock divider */
	clkCfg.useStandbyClk = CyFalse; /* device has no 32KHz clock supplied */
	clkCfg.clkSrc = CY_U3P_SYS_CLK; /* Clock source for a peripheral block  */

	Status = CyU3PDeviceInit(&clkCfg); // Start with the clock at 403 MHz
	if (Status == CY_U3P_SUCCESS)
    {
		Status = CyU3PDeviceCacheControl(CyTrue, CyTrue, CyTrue);
		if (Status == CY_U3P_SUCCESS)
		{
			/* Configure the IO matrix for the device. On the FX3 DVK board, the COM port
			 * is connected to the IO(53:56). This means that either DQ32 mode should be
			 * selected or lppMode should be set to UART_ONLY. Here we are choosing
			 * UART_ONLY configuration for 16 bit slave FIFO configuration and setting
			 * isDQ32Bit for 32-bit slave FIFO configuration. */
			ioConfig.useUart   = CyTrue;
			ioConfig.useI2C    = CyTrue;
			ioConfig.useI2S    = CyFalse;
			ioConfig.useSpi    = CyFalse;
			ioConfig.isDQ32Bit = CyFalse; //CY_FX_SLFIFO_GPIF_16_32BIT_CONF_SELECT == 0 -> 16 bit
			ioConfig.lppMode   = CY_U3P_IO_MATRIX_LPP_UART_ONLY;
			/* No GPIOs are enabled. */
			ioConfig.gpioSimpleEn[0]  = 0;
			ioConfig.gpioSimpleEn[1]  = 0;
			ioConfig.gpioComplexEn[0] = 0x00000000;  //
			ioConfig.gpioComplexEn[1] = 0x00000000;  // 0

		//	if (CyU3PDeviceConfigureIOMatrix (&ioConfig) == CY_U3P_SUCCESS)   CyU3PKernelEntry ();
			Status = CyU3PDeviceConfigureIOMatrix(&ioConfig);
			if (Status == CY_U3P_SUCCESS)
			{
				CyU3PKernelEntry();			// Start RTOS, this does not return
			}
		}
	}
    while (1);			// Get here on a failure, can't recover, just hang here
}
