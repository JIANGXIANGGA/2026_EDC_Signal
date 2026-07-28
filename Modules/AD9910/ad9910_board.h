#ifndef AD9910_BOARD_H
#define AD9910_BOARD_H

#include <stdint.h>

#include "ad9910.h"

const ad9910_pin_config_t *AD9910_Board_GetSerialPinConfig(void);
void AD9910_Board_SelectProfile(uint8_t profile_index);
void AD9910_Board_SetSweepDirection(uint8_t direction_up);
void AD9910_Board_SetSweepHold(uint8_t hold);

#endif
