#ifndef ADC121S101_H
#define ADC121S101_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx_hal.h"

#define ADC121S101_BLOCK_SIZE 1024U

/** @brief ADC 连续采样运行统计。 */
typedef struct {
    uint32_t completed_block_count;
    uint32_t dropped_block_count;
    uint32_t copy_retry_count;
    uint32_t error_count;
    uint32_t recovery_count;
    uint8_t running;
    uint8_t recovery_pending;
} ADC121S101_Status;

/**
 * @brief 绑定 SPI2、TIM7 更新 DMA，并初始化双半区 DMA 缓冲区。
 * @note PB12 必须由 CubeMX 配置为 SPI2_NSS，SPI2_RX 与 TIM7_UP DMA 必须为循环模式。
 */
HAL_StatusTypeDef ADC121S101_Init(SPI_HandleTypeDef *hspi,
                                  TIM_HandleTypeDef *sample_timer);

/** @brief 启动 TIM7 硬件节拍驱动的连续 SPI DMA 采样。 */
HAL_StatusTypeDef ADC121S101_Start(void);

/** @brief 停止采样并终止 SPI DMA，供主循环重配置采样率时调用。 */
HAL_StatusTypeDef ADC121S101_Stop(void);

/** @brief 在主循环中处理 SPI/DMA 错误后的自动恢复。 */
HAL_StatusTypeDef ADC121S101_Process(void);

/** @brief SPI RX DMA 到达前半区时发布缓冲区 0。 */
void ADC121S101_TxRxHalfCpltCallback(SPI_HandleTypeDef *hspi);

/** @brief SPI RX DMA 到达全缓冲区时发布缓冲区 1。 */
void ADC121S101_TxRxCpltCallback(SPI_HandleTypeDef *hspi);

/** @brief 记录 SPI DMA 错误并停止硬件采样节拍。 */
void ADC121S101_ErrorCallback(SPI_HandleTypeDef *hspi);

/**
 * @brief 将最新完整的 1024 点采样块复制到调用方缓冲区。
 * @param destination 目标缓冲区。
 * @param capacity 目标缓冲区可容纳的 uint16_t 元素数量。
 * @retval 1 获得一个新数据块；0 当前无新块或复制期间 DMA 已回卷。
 * @note 复制时完成 12 位提取：sample = rx & 0x0FFF。
 */
uint8_t ADC121S101_CopyLatestBlock(uint16_t *destination,
                                   uint32_t capacity);

/** @brief 丢弃尚未消费的旧采样块，采样率切换后用于重新对齐帧。 */
void ADC121S101_DiscardPendingBlock(void);

/** @brief 读取连续采样运行统计。 */
void ADC121S101_GetStatus(ADC121S101_Status *status);

#ifdef __cplusplus
}
#endif

#endif /* ADC121S101_H */
