#pragma once

// rx888r2
void rx888r2_GpioSet(uint32_t mdata);
void rx888r2_GpioInitialize();
void rx888r2_SetAttenuator(uint8_t value);
void rx888r2_SetGain(uint8_t value);
void rx888r2_AdcStandby(CyBool_t standby);
/* Snapshot the current state of all user-visible steady-state GPIOs,
 * packed using the same bit positions as the GPIOFX3 control word
 * (enum GPIOPin in protocol.h). PGA is inverted at the pin so it is
 * un-inverted here to keep symmetric control-word semantics: a host
 * consumer can do `if (gpio_state & SHDWN)`. Exposed via GETSTATS. */
uint32_t rx888r2_ReadGpioState(void);
