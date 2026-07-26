#ifndef ADC121S101_H
#define ADC121S101_H

#include <stdint.h>

#include "stm32g4xx_hal.h"

#define ADC_INPUT_HALF_SIZE 2U
#define ADC_INPUT_BLOCK_SIZE 1024U
#define ADC_INPUT_SAMPLE_SIZE \
    (ADC_INPUT_HALF_SIZE * ADC_INPUT_BLOCK_SIZE)

/**
 * @brief 绑定 SPI2、TIM7 更新 DMA，并初始化双半区 DMA 缓冲区。
 * @note PB12 必须由 CubeMX 配置为 SPI2_NSS。
 */
HAL_StatusTypeDef ADC_Input_Init(SPI_HandleTypeDef *adc_spi,
                                 TIM_HandleTypeDef *adc_timer);

/**
 * @brief 启动 TIM7 硬件节拍驱动的连续 SPI DMA 采样。
 */
HAL_StatusTypeDef ADC_Start(void);

/**
 * @brief 在主循环中处理 SPI/DMA 错误后的恢复请求。
 */
HAL_StatusTypeDef ADC_Process(void);

/**
 * @brief 将最新完整的 1024 点采样块复制到调用方缓冲区。
 * @retval 1 获得新块；0 当前无新块、容量不足或复制期间目标块已变化。
 * @note 复制时完成 sample = rx & 0x0FFFU。
 */
uint8_t ADC121S101_CopyLatestBlock(uint16_t *destination, uint32_t capacity);

uint8_t ADC121S101_IsRunning(void);
uint32_t ADC121S101_GetHalfCompleteCount(void);
uint32_t ADC121S101_GetCompleteCount(void);
uint32_t ADC121S101_GetErrorCount(void);
uint32_t ADC121S101_GetOverrunCount(void);

#endif
