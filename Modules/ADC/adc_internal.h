#ifndef ADC_INTERNAL_H
#define ADC_INTERNAL_H

#include <stdint.h>

#include "stm32g4xx_hal.h"

#ifndef SIGNAL_ADC_USE_CUBEMX_GENERATED
#define SIGNAL_ADC_USE_CUBEMX_GENERATED 0U
#endif

#define ADC_INTERNAL_HALF_COUNT 2U
#define ADC_INTERNAL_BLOCK_SIZE 4096U
#define ADC_INTERNAL_SAMPLE_COUNT \
    (ADC_INTERNAL_HALF_COUNT * ADC_INTERNAL_BLOCK_SIZE)
#define ADC_INTERNAL_INPUT_GPIO_PORT GPIOB
#define ADC_INTERNAL_INPUT_PIN GPIO_PIN_0

/**
 * @brief 配置 ADC1_IN15（PB0）、循环 DMA 和 TIM7 外部触发采集。
 * @param trigger_timer 产生 ADC 触发事件的 TIM7 句柄。
 */
HAL_StatusTypeDef ADC_Internal_Init(TIM_HandleTypeDef *trigger_timer);

/**
 * @brief 启动 ADC DMA，并在 DMA 就绪后启动 TIM7 触发。
 */
HAL_StatusTypeDef ADC_Internal_Start(void);

/**
 * @brief 在主循环处理 ADC/DMA 错误后的非阻塞恢复请求。
 */
HAL_StatusTypeDef ADC_Internal_Process(void);

/**
 * @brief 复制最新完成的 4096 点 DMA 半区。
 * @retval 1 已复制新数据；0 当前没有新数据或复制期间发生覆盖。
 */
uint8_t ADC_Internal_CopyLatestBlock(uint16_t *destination,
                                     uint32_t capacity);

uint8_t ADC_Internal_IsRunning(void);
uint32_t ADC_Internal_GetHalfCompleteCount(void);
uint32_t ADC_Internal_GetCompleteCount(void);
uint32_t ADC_Internal_GetErrorCount(void);
uint32_t ADC_Internal_GetOverrunCount(void);

#endif
