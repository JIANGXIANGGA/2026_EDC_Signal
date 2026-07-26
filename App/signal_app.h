#ifndef SIGNAL_APP_H
#define SIGNAL_APP_H

#include <stdint.h>

#include "stm32g4xx_hal.h"

typedef struct {
    uint8_t initialized;
    uint8_t adc_running;
    uint32_t adc_block_count;
    uint16_t adc_last_min;
    uint16_t adc_last_max;
    uint16_t adc_last_average;
    uint32_t adc_half_complete_count;
    uint32_t adc_complete_count;
    uint32_t adc_error_count;
    uint32_t adc_overrun_count;
    uint32_t dac_half_complete_count;
    uint32_t dac_complete_count;
    uint32_t dac_error_count;
    uint32_t dac_underrun_count;
    uint8_t dac_loopback_running;
    uint32_t dac_loopback_dropped_block_count;
    uint32_t dac_loopback_error_count;
} signal_app_status_t;

extern signal_app_status_t g_signal_app_status;

HAL_StatusTypeDef Signal_App_Init(SPI_HandleTypeDef *ad9910_spi,
                                  SPI_HandleTypeDef *adc_spi,
                                  TIM_HandleTypeDef *adc_timer,
                                  DAC_HandleTypeDef *dac,
                                  TIM_HandleTypeDef *dac_timer);
void Signal_App_Process(void);

#endif
