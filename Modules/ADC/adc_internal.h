#ifndef ADC_INTERNAL_H
#define ADC_INTERNAL_H

#include <stdint.h>

#include "stm32g4xx_hal.h"

#ifndef SIGNAL_ADC_USE_CUBEMX_GENERATED
#define SIGNAL_ADC_USE_CUBEMX_GENERATED 0U
#endif

#define ADC_INTERNAL_HALF_COUNT 2U
#define ADC_INTERNAL_BLOCK_SIZE 8192U
#define ADC_INTERNAL_SAMPLE_RATE_HZ 4000000U
#define ADC_INTERNAL_SAMPLE_COUNT \
    (ADC_INTERNAL_HALF_COUNT * ADC_INTERNAL_BLOCK_SIZE)
#define ADC_INTERNAL_INPUT_GPIO_PORT GPIOA
#define ADC_INTERNAL_INPUT_PIN GPIO_PIN_7

/**
 * @brief 配置 ADC2_IN4（PA7 高速通道）与循环 DMA 连续采集。
 */
HAL_StatusTypeDef ADC_Internal_Init(void);

/**
 * @brief 启动 ADC DMA 和 ADC 硬件连续转换。
 */
HAL_StatusTypeDef ADC_Internal_Start(void);

/**
 * @brief 在主循环处理 ADC/DMA 错误后的非阻塞恢复请求。
 */
HAL_StatusTypeDef ADC_Internal_Process(void);

/**
 * @brief 复制最新完成的 8192 点 DMA 半区。
 * @retval 1 已复制新数据；0 当前没有新数据或复制期间发生覆盖。
 */
uint8_t ADC_Internal_CopyLatestBlock(uint16_t *destination,
                                     uint32_t capacity);

uint8_t ADC_Internal_IsRunning(void);
uint32_t ADC_Internal_GetHalfCompleteCount(void);
uint32_t ADC_Internal_GetCompleteCount(void);
uint32_t ADC_Internal_GetErrorCount(void);
uint32_t ADC_Internal_GetOverrunCount(void);
uint32_t ADC_Internal_GetSampleRateHz(void);

#endif
