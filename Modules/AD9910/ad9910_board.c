#include "ad9910_board.h"

#include "main.h"

static const ad9910_pin_config_t g_ad9910_serial_pins = {
    .csb_port = AD9910_CSB_GPIO_Port,
    .csb_pin = AD9910_CSB_Pin,
    .io_update_port = AD9910_IO_UPDATE_GPIO_Port,
    .io_update_pin = AD9910_IO_UPDATE_Pin,
};

const ad9910_pin_config_t *AD9910_Board_GetSerialPinConfig(void)
{
    return &g_ad9910_serial_pins;
}

void AD9910_Board_SelectProfile(uint8_t profile_index)
{
    HAL_GPIO_WritePin(AD9910_PROFILE0_GPIO_Port,
                      AD9910_PROFILE0_Pin,
                      ((profile_index & 0x01U) != 0U) ?
                          GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(AD9910_PROFILE1_GPIO_Port,
                      AD9910_PROFILE1_Pin,
                      ((profile_index & 0x02U) != 0U) ?
                          GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(AD9910_PROFILE2_GPIO_Port,
                      AD9910_PROFILE2_Pin,
                      ((profile_index & 0x04U) != 0U) ?
                          GPIO_PIN_SET : GPIO_PIN_RESET);
}

void AD9910_Board_SetSweepDirection(uint8_t direction_up)
{
    HAL_GPIO_WritePin(AD9910_DRCTL_GPIO_Port,
                      AD9910_DRCTL_Pin,
                      (direction_up != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void AD9910_Board_SetSweepHold(uint8_t hold)
{
    HAL_GPIO_WritePin(AD9910_DRHOLD_GPIO_Port,
                      AD9910_DRHOLD_Pin,
                      (hold != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
