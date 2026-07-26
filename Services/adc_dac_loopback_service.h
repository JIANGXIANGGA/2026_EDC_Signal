#ifndef ADC_DAC_LOOPBACK_SERVICE_H
#define ADC_DAC_LOOPBACK_SERVICE_H

#include <stdint.h>

#include "stm32g4xx_hal.h"

HAL_StatusTypeDef ADC_DAC_Loopback_Init(DAC_HandleTypeDef *hdac,
                                        TIM_HandleTypeDef *trigger_timer,
                                        uint32_t sample_rate_hz);
uint8_t ADC_DAC_Loopback_PushBlock(const uint16_t *samples, uint32_t length);
uint8_t ADC_DAC_Loopback_IsRunning(void);
uint32_t ADC_DAC_Loopback_GetDroppedBlockCount(void);
uint32_t ADC_DAC_Loopback_GetErrorCount(void);

#endif
