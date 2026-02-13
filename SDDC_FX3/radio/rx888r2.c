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