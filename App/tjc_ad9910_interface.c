#include "tjc_ad9910_interface.h"

HAL_StatusTypeDef TJC_AD9910_Interface_Dispatch(
    const ad9910_siggen_command_t *command)
{
    return AD9910_SignalGenerator_HandleCommand(command);
}
