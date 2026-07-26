#ifndef DAC_OUTPUT_H
#define DAC_OUTPUT_H

#include <stdint.h>

#include "stm32g4xx_hal.h"

#define DAC_OUTPUT_HALF_SIZE 2U
#define DAC_OUTPUT_BLOCK_SIZE 1024U
#define DAC_OUTPUT_SAMPLE_SIZE \
    (DAC_OUTPUT_HALF_SIZE * DAC_OUTPUT_BLOCK_SIZE)

typedef enum {
    DAC_OUTPUT_HALF_FREE = 0,
    DAC_OUTPUT_HALF_FILLING,
    DAC_OUTPUT_HALF_READY,
    DAC_OUTPUT_HALF_PLAYING
} dac_output_half_status_t;

/**
 * @brief 绑定由 CubeMX 初始化的 DAC 和触发定时器句柄。
 */
HAL_StatusTypeDef DAC_Output_Init(DAC_HandleTypeDef *hdac,
                                  TIM_HandleTypeDef *dac_timer);
HAL_StatusTypeDef DAC_Output_ConfigSampleRate(uint32_t sample_rate_hz);

/**
 * @brief 使用 DAC1 Channel 1 循环输出 12 bit 右对齐采样数组。
 */
HAL_StatusTypeDef DAC_Output_Start(void);

/**
 * @brief 先停止触发定时器，再停止 DAC DMA。
 */
HAL_StatusTypeDef DAC_Output_Stop(void);

uint8_t DAC_Output_AcquireBuffer(uint16_t **buffer, uint8_t *index);
uint8_t DAC_Output_CommitBuffer(uint8_t index);
uint32_t DAC_Output_GetHalfCompleteCount(void);
uint32_t DAC_Output_GetCompleteCount(void);
uint32_t DAC_Output_GetErrorCount(void);
uint32_t DAC_Output_GetUnderrunCount(void);

#endif
