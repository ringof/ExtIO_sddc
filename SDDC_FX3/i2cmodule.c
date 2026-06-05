/*
 * i2cmodule.c
 *
 *  Created on: Mar 14, 2017
 *      Author: Oscar
 */

#include "cyu3system.h"
#include "cyu3os.h"
#include "cyu3dma.h"
#include "cyu3error.h"
#include "cyu3usb.h"
#include "cyu3i2c.h"
#include "cyu3gpio.h"
#include "cyu3utils.h"
#include "i2cmodule.h"

/* FX3 dedicated I2C pins (per hardware): SCL=GPIO58, SDA=GPIO59. */
#define I2C_SCL_GPIO  58
#define I2C_SDA_GPIO  59

extern uint32_t glCounter[20];

CyU3PReturnStatus_t
I2cInit ()
{
    CyU3PI2cConfig_t i2cConfig;
    CyU3PReturnStatus_t status = CY_U3P_SUCCESS;

    /* Initialize and configure the I2C master module. */
    status = CyU3PI2cInit ();
    if (status != CY_U3P_SUCCESS) return status;

    /* Start the I2C master block. The bit rate is set at 400KHz.
     * The data transfer is done via DMA. */
    CyU3PMemSet ((uint8_t *)&i2cConfig, 0, sizeof(i2cConfig));
    i2cConfig.bitRate    = I2C_BITRATE;
    i2cConfig.busTimeout = I2C_BUS_TIMEOUT;   /* finite; see i2cmodule.h (#154) */
    i2cConfig.dmaTimeout = 0xFFFF;
    i2cConfig.isDma      = CyFalse;
    status = CyU3PI2cSetConfig (&i2cConfig, NULL);

    return status;
}



/* I2C read / write for application. */
CyU3PReturnStatus_t
I2cTransfer (
        uint8_t   byteAddress,
        uint8_t   devAddr,
        uint8_t   byteCount,
        uint8_t   *buffer,
        CyBool_t  isRead)
{

    CyU3PI2cPreamble_t preamble;
    CyU3PReturnStatus_t status = CY_U3P_SUCCESS;
    if (byteCount == 0)
    {
        return status;
    }
	if (isRead)
	{
		/* Update the preamble information. */
		preamble.length    = 3;
		preamble.buffer[0] = devAddr;
		preamble.buffer[1] = byteAddress;
		preamble.buffer[2] = (devAddr | 0x01);
		preamble.ctrlMask  = 0x0002; // stop position 2
		status = CyU3PI2cReceiveBytes (&preamble, buffer, byteCount, 0);
	}
	else /* Write */
	{
		/* Update the preamble information. */
		preamble.length    = 2;
		preamble.buffer[0] = devAddr;
		preamble.buffer[1] = byteAddress;
		preamble.ctrlMask  = 0x0000;
		status = CyU3PI2cTransmitBytes (&preamble, buffer, byteCount, 0);
	}
    if (status != CY_U3P_SUCCESS)
        glCounter[1]++;
    return status;
}

CyU3PReturnStatus_t I2cTransferW1(  // Write one byte only
		uint8_t   byteAddress,
		uint8_t   devAddr,
        uint8_t   data)
{
	uint8_t   byteCount = 1;
	uint8_t   ldata = data;
	return I2cTransfer (byteAddress, devAddr, byteCount, &ldata, CyFalse);
}

/* Configure an overridden I2C pin as a push-pull output at the given level. */
static void i2c_gpio_out (uint8_t gpio, CyBool_t level)
{
	CyU3PGpioSimpleConfig_t c;
	c.outValue    = level;
	c.inputEn     = CyFalse;
	c.driveLowEn  = CyTrue;
	c.driveHighEn = CyTrue;
	c.intrMode    = CY_U3P_GPIO_NO_INTR;
	CyU3PGpioSetSimpleConfig (gpio, &c);
}

/* Configure an overridden I2C pin as an input with a weak pull-up. */
static void i2c_gpio_in (uint8_t gpio)
{
	CyU3PGpioSimpleConfig_t c;
	c.outValue    = CyTrue;
	c.inputEn     = CyTrue;
	c.driveLowEn  = CyFalse;
	c.driveHighEn = CyFalse;
	c.intrMode    = CY_U3P_GPIO_NO_INTR;
	CyU3PGpioSetSimpleConfig (gpio, &c);
	CyU3PGpioSetIoMode (gpio, CY_U3P_GPIO_IO_MODE_WPU);
}

/* I2C bus recovery (#163).  Releases the I2C block, drives SCL/SDA as GPIO,
 * issues up to 9 SCL pulses to clock a slave out of a hung transaction (a
 * slave that is holding SDA low), then a STOP, and re-initialises the block.
 * Only meaningful if the bus is actually held; a slave that has gone mute
 * without holding SDA will not be revived by this.  Returns the re-init
 * status; *clocks (if non-NULL) = SCL pulses issued before SDA released. */
CyU3PReturnStatus_t I2cBusRecover (uint8_t *info)
{
	CyBool_t sda_before = CyFalse, sda_after = CyFalse;
	uint8_t  n;

	/* Release the I2C block so the pins can be driven as simple GPIO. */
	CyU3PI2cDeInit ();
	CyU3PDeviceGpioOverride (I2C_SCL_GPIO, CyTrue);
	CyU3PDeviceGpioOverride (I2C_SDA_GPIO, CyTrue);

	i2c_gpio_in  (I2C_SDA_GPIO);        /* sample SDA (pulled up)   */
	i2c_gpio_out (I2C_SCL_GPIO, CyTrue);/* SCL idle high            */
	CyU3PBusyWait (20);

	CyU3PGpioSimpleGetValue (I2C_SDA_GPIO, &sda_before);

	/* FORCED: issue all 9 SCL pulses unconditionally (no early bail on
	 * SDA-high) — exercises the clock line even when the bus is not held,
	 * to test whether toggling SCL jogs a mute slave out of its state. */
	for (n = 0; n < 9; n++)
	{
		CyU3PGpioSetValue (I2C_SCL_GPIO, CyFalse); CyU3PBusyWait (20);
		CyU3PGpioSetValue (I2C_SCL_GPIO, CyTrue);  CyU3PBusyWait (20);
	}

	CyU3PGpioSimpleGetValue (I2C_SDA_GPIO, &sda_after);

	/* STOP condition: SDA low->high while SCL is high. */
	i2c_gpio_out (I2C_SDA_GPIO, CyFalse);       CyU3PBusyWait (20);
	CyU3PGpioSetValue (I2C_SCL_GPIO, CyTrue);   CyU3PBusyWait (20);
	CyU3PGpioSetValue (I2C_SDA_GPIO, CyTrue);   CyU3PBusyWait (20);

	/* info: bit0 = SDA before the clocks, bit1 = SDA after. */
	if (info) *info = (uint8_t)((sda_before ? 1 : 0) | (sda_after ? 2 : 0));

	/* Hand the pins back to the I2C block and re-initialise it. */
	CyU3PDeviceGpioRestore (I2C_SCL_GPIO);
	CyU3PDeviceGpioRestore (I2C_SDA_GPIO);
	return I2cInit ();
}
