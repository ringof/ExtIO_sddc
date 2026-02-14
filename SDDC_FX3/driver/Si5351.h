#pragma once

CyU3PReturnStatus_t Si5351Init();

CyBool_t si5351_pll_locked(void);
CyBool_t si5351_clk0_enabled(void);

CyU3PReturnStatus_t si5351aSetFrequencyA(UINT32 freq);
CyU3PReturnStatus_t si5351aSetFrequencyB(UINT32 freq2);
