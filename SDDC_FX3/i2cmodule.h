#ifndef _INCLUDED_I2CMODULE_H_
#define _INCLUDED_I2CMODULE_H_

#include "cyu3types.h"
#include "cyu3usbconst.h"

/* I2C Data rate */
#define I2C_BITRATE        (400000)

/* I2C bus timeout, in FX3 core clocks (403.2 MHz), i.e. how long SCL may be
 * held low before CyU3PI2cReceiveBytes/TransmitBytes give up.  Formerly
 * 0xFFFFFFFF (~10.6 s), which let a stranded/clock-stretched bus block the
 * EP0 vendor handler long enough to trip the handler-liveness watchdog and
 * wedge control transfers (issue #154).  ~10 ms is far longer than any
 * legitimate 400 kHz transfer yet short enough to keep EP0 responsive.
 *   4,032,000 core clocks / 403.2 MHz = 10.0 ms */
#define I2C_BUS_TIMEOUT    (4032000u)

CyU3PReturnStatus_t I2cInit();

/* I2C bus recovery (#163 diagnostic): release the I2C block, clock SCL up to
 * 9 times to free a slave holding SDA, issue a STOP, then re-init the block.
 * *clocks (if non-NULL) returns the number of SCL pulses before SDA released. */
CyU3PReturnStatus_t I2cBusRecover(uint8_t *clocks);

CyU3PReturnStatus_t I2cTransferW1 (  // Write one byte only
		uint8_t   byteAddress,
		uint8_t   devAddr,
        uint8_t   data);

CyU3PReturnStatus_t I2cTransfer (
        uint8_t   byteAddress,
        uint8_t   devAddr,
        uint8_t   byteCount,
        uint8_t   *buffer,
        CyBool_t  isRead);


#endif
