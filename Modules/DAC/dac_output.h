#ifndef DAC_OUTPUT_H
#define DAC_OUTPUT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx_hal.h"

#define DAC_OUTPUT_BLOCK_SIZE 1024U

/** @brief DAC 双缓冲运行统计。 */
typedef struct {
    uint32_t completed_block_count; /**< DMA 已输出的半缓冲区总数。 */
    uint32_t underrun_count;        /**< DMA 切换时下一半区未就绪的次数。 */
    uint32_t error_count;           /**< DAC 或 DMA 传输错误累计次数。 */
    uint32_t recovery_count;        /**< 主循环成功恢复 DAC 输出的次数。 */
    uint8_t running;                /**< DAC 循环 DMA 当前是否正在运行。 */
    uint8_t recovery_pending;       /**< 是否存在等待主循环处理的恢复请求。 */
} DAC_Output_Status;

/**
 * @brief 初始化 DAC 双缓冲区的软件状态。
 * @note DAC、TIM6 和 DMA 的硬件配置由 CubeMX 生成代码负责。
 */
HAL_StatusTypeDef DAC_Output_Init(DAC_HandleTypeDef *hdac);

/** @brief 以 2048 点循环 DMA 方式启动 DAC1 Channel1。 */
HAL_StatusTypeDef DAC_Output_Start(void);

/** @brief 停止 DAC DMA。 */
HAL_StatusTypeDef DAC_Output_Stop(void);

/** @brief 在主循环中处理 DMA 欠载或错误后的自动恢复。 */
HAL_StatusTypeDef DAC_Output_Process(void);

/**
 * @brief 领取一个当前未被 DMA 读取的 DAC 半区。
 * @param buffer 返回可写缓冲区首地址。
 * @param buffer_index 返回缓冲区编号，提交时必须原样传回。
 * @retval 1 成功领取；0 当前没有安全可写的半区。
 */
uint8_t DAC_Output_AcquireBuffer(uint16_t **buffer, uint8_t *buffer_index);

/** @brief 标记一个 DAC 半区已经写完，可以交给 DMA 输出。 */
uint8_t DAC_Output_CommitBuffer(uint8_t buffer_index);

/** @brief 处理 DAC DMA 前半区传输完成事件。 */
void DAC_Output_HalfCpltCallback(DAC_HandleTypeDef *hdac);

/** @brief 处理 DAC DMA 整个循环传输完成事件。 */
void DAC_Output_CpltCallback(DAC_HandleTypeDef *hdac);

/** @brief 记录 DAC DMA 错误并通知主循环恢复。 */
void DAC_Output_ErrorCallback(DAC_HandleTypeDef *hdac);

/** @brief 读取 DAC 双缓冲运行统计。 */
void DAC_Output_GetStatus(DAC_Output_Status *status);

#ifdef __cplusplus
}
#endif

#endif /* DAC_OUTPUT_H */
