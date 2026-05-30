#include "Application.h"
#include "radio.h"

#define GPIO_ATT_LE 17
#define GPIO_BIAS_VHF 18
#define GPIO_BIAS_HF 19
#define GPIO_RANDO 20
#define GPIO_LED_BLUE 21
#define GPIO_PGA 24
#define GPIO_ATT_DATA  26
#define GPIO_ATT_CLK   27
#define GPIO_SHDWN 28
#define GPIO_DITH 29
#define GPIO_VHF_EN 35
#define GPIO_VGA_LE 38

void rx888r2_GpioSet(uint32_t mdata)
{
    CyU3PGpioSetValue (GPIO_SHDWN, (mdata & SHDWN) == SHDWN ); 		 // SHDN
    CyU3PGpioSetValue (GPIO_DITH, (mdata & DITH ) == DITH  ); 		 // DITH
    CyU3PGpioSetValue (GPIO_RANDO, (mdata & RANDO) == RANDO ); 		 // RAND
    CyU3PGpioSetValue (GPIO_BIAS_HF, (mdata & BIAS_HF) == BIAS_HF);
    CyU3PGpioSetValue (GPIO_BIAS_VHF, (mdata & BIAS_VHF) == BIAS_VHF);
	CyU3PGpioSetValue (GPIO_LED_BLUE, (mdata & LED_BLUE) == LED_BLUE);
	CyU3PGpioSetValue (GPIO_PGA, (mdata & PGA_EN ) != PGA_EN  ); 		 // PGA_EN
    CyU3PGpioSetValue (GPIO_VHF_EN, (mdata & VHF_EN) == VHF_EN ); // VHF_EN
}

/* Drive the ADC SHDN line: standby=CyTrue parks the ADC in low-power
 * standby (SHDN high, ~330 mA + thermal saving); CyFalse wakes it. */
void rx888r2_AdcStandby(CyBool_t standby)
{
    CyU3PGpioSetValue (GPIO_SHDWN, standby);
}

/* Pack the current state of every user-visible steady-state GPIO into one
 * 32-bit word, using the GPIOFX3 control-word bit positions (enum GPIOPin
 * in protocol.h). PGA is inverted in rx888r2_GpioSet (control PGA_EN=1
 * drives pin LOW for high-gain range); we un-invert here so a host that
 * does `if (gpio_state & PGA_EN)` matches what it sent.
 * The transient shift-register strobe lines (ATT_LE/CLK/DATA, VGA_LE) are
 * intentionally omitted — their values mid-operation are meaningless. */
uint32_t rx888r2_ReadGpioState(void)
{
    CyBool_t v;
    uint32_t s = 0;
    if (CyU3PGpioGetValue(GPIO_SHDWN,    &v) == CY_U3P_SUCCESS && v) s |= SHDWN;
    if (CyU3PGpioGetValue(GPIO_DITH,     &v) == CY_U3P_SUCCESS && v) s |= DITH;
    if (CyU3PGpioGetValue(GPIO_RANDO,    &v) == CY_U3P_SUCCESS && v) s |= RANDO;
    if (CyU3PGpioGetValue(GPIO_BIAS_HF,  &v) == CY_U3P_SUCCESS && v) s |= BIAS_HF;
    if (CyU3PGpioGetValue(GPIO_BIAS_VHF, &v) == CY_U3P_SUCCESS && v) s |= BIAS_VHF;
    if (CyU3PGpioGetValue(GPIO_LED_BLUE, &v) == CY_U3P_SUCCESS && v) s |= LED_BLUE;
    if (CyU3PGpioGetValue(GPIO_VHF_EN,   &v) == CY_U3P_SUCCESS && v) s |= VHF_EN;
    /* PGA pin is inverted: pin LOW means logical PGA_EN asserted. */
    if (CyU3PGpioGetValue(GPIO_PGA,      &v) == CY_U3P_SUCCESS && !v) s |= PGA_EN;
    return s;
}

void rx888r2_GpioInitialize()
{
    ConfGPIOsimpleout (GPIO_SHDWN);
    ConfGPIOsimpleout (GPIO_DITH);
    ConfGPIOsimpleout (GPIO_RANDO);
    ConfGPIOsimpleout (GPIO_BIAS_HF);
    ConfGPIOsimpleout (GPIO_BIAS_VHF);
	ConfGPIOsimpleout (GPIO_LED_BLUE);
    ConfGPIOsimpleout (GPIO_VHF_EN);

	ConfGPIOsimpleout (GPIO_PGA);
    ConfGPIOsimpleout (GPIO_ATT_LE);
    ConfGPIOsimpleout (GPIO_ATT_DATA);
    ConfGPIOsimpleout (GPIO_ATT_CLK);
    ConfGPIOsimpleout (GPIO_VGA_LE);

    rx888r2_GpioSet(LED_BLUE);

    CyU3PGpioSetValue (GPIO_ATT_LE, 0);  // ATT_LE latched
    CyU3PGpioSetValue (GPIO_ATT_CLK, 0);  // test version
    CyU3PGpioSetValue (GPIO_ATT_DATA, 0);  // ATT_DATA
    CyU3PGpioSetValue (GPIO_VGA_LE, 1);

	CyU3PGpioSetValue (GPIO_PGA, 1); // PGA =1 , 1.5v range

	rx888r2_AdcStandby(CyTrue);  // not streaming at boot — park ADC in standby
}

/*
 * Bit-bang SPI shift-out: clock `bits` MSB-first from `value` on
 * GPIO_ATT_DATA / GPIO_ATT_CLK, then pulse `latch_pin` high.
 */
static void GpioShiftOut(uint8_t latch_pin, uint8_t value, uint8_t bits)
{
	uint8_t mask = 1 << (bits - 1);
	uint8_t i, b;

	CyU3PGpioSetValue (latch_pin, 0);
	CyU3PGpioSetValue (GPIO_ATT_CLK, 0);
	for (i = bits; i > 0; i--)
	{
		b = (value & mask) != 0;
		CyU3PGpioSetValue (GPIO_ATT_DATA, b);
		CyU3PGpioSetValue (GPIO_ATT_CLK, 1);
		value = value << 1;
		CyU3PGpioSetValue (GPIO_ATT_CLK, 0);
	}
	CyU3PGpioSetValue (latch_pin, 1);
}

/* PE4304 — 64 step, 0.5 dB */
void rx888r2_SetAttenuator(uint8_t value)
{
	GpioShiftOut(GPIO_ATT_LE, value, 6);
	CyU3PGpioSetValue (GPIO_ATT_LE, 0);
}

/* AD8370 — 128 step, 0.5 dB */
void rx888r2_SetGain(uint8_t value)
{
	GpioShiftOut(GPIO_VGA_LE, value, 8);
	CyU3PGpioSetValue (GPIO_ATT_DATA, 0);
}
