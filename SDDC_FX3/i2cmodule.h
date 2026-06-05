#ifndef _INCLUDED_I2CMODULE_H_
#define _INCLUDED_I2CMODULE_H_

#include "cyu3types.h"
#include "cyu3usbconst.h"

/* I2C Data rate.  Dropped from Fast mode (400 kHz) to Standard mode (100 kHz)
 * as a #163 experiment: the on-board clock generator is very likely an MS5351M
 * clone, which is often marginal at 400 kHz.  Testing whether the slower bus
 * (a) prevents the Si5351 address-NAK wedge under malformed I2C and/or
 * (b) lets us reach a part that has gone mute at Fast mode. */
#define I2C_BITRATE        (100000)

/* I2C bus timeout, in FX3 core clocks (403.2 MHz), i.e. how long SCL may be
 * held low before CyU3PI2cReceiveBytes/TransmitBytes give up.  Formerly
 * 0xFFFFFFFF (~10.6 s), which let a stranded/clock-stretched bus block the
 * EP0 vendor handler long enough to trip the handler-liveness watchdog and
 * wedge control transfers (issue #154).  ~10 ms is far longer than any
 * legitimate 400 kHz transfer yet short enough to keep EP0 responsive.
 *   4,032,000 core clocks / 403.2 MHz = 10.0 ms */
#define I2C_BUS_TIMEOUT    (4032000u)

CyU3PReturnStatus_t I2cInit();

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
