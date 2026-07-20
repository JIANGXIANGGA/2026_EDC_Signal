#ifndef CST816T_H
#define CST816T_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "stm32g4xx_hal.h"

typedef struct {
    I2C_HandleTypeDef *i2c;          /**< 与 CST816T 通信的 I2C 句柄。 */
    GPIO_TypeDef *reset_port;        /**< 触摸控制器复位信号端口。 */
    uint16_t reset_pin;              /**< 触摸控制器复位信号引脚。 */
    uint16_t horizontal_resolution;  /**< 映射后的水平方向像素数。 */
    uint16_t vertical_resolution;    /**< 映射后的垂直方向像素数。 */
    bool swap_xy;                    /**< 是否交换触摸坐标的 X/Y 轴。 */
    bool mirror_x;                   /**< 是否沿 X 轴镜像触摸坐标。 */
    bool mirror_y;                   /**< 是否沿 Y 轴镜像触摸坐标。 */
    uint32_t refresh_period_ms;      /**< 无中断时主动读取坐标的周期。 */
} cst816t_config_t;

typedef struct {
    cst816t_config_t config;         /**< 当前触摸实例使用的配置副本。 */
    uint8_t rx_data[6];              /**< I2C DMA 接收的手势与坐标原始数据。 */
    volatile bool transfer_busy;     /**< I2C DMA 读取是否正在进行。 */
    volatile bool transfer_complete; /**< 是否收到待读取解析的 DMA 完成事件。 */
    volatile bool read_requested;    /**< 是否需要在主循环发起一次坐标读取。 */
    uint32_t last_read_tick;         /**< 上次周期读取调度的系统毫秒计数。 */
    int32_t x;                       /**< 最近一次有效触摸的屏幕 X 坐标。 */
    int32_t y;                       /**< 最近一次有效触摸的屏幕 Y 坐标。 */
    bool pressed;                    /**< 当前是否检测到手指按下。 */
    bool initialized;                /**< 触摸实例是否已完成初始化。 */
} cst816t_t;

/** @brief 初始化 CST816T 实例并执行硬件复位。 */
HAL_StatusTypeDef CST816T_Init(cst816t_t *touch,
                               const cst816t_config_t *config);
/** @brief 在主循环中处理读取完成事件和周期调度。 */
void CST816T_Process(cst816t_t *touch);
/** @brief 通知 Driver 已发生触摸外部中断。 */
void CST816T_NotifyInterrupt(cst816t_t *touch);
/** @brief 获取最近触摸坐标和当前按下状态。 */
bool CST816T_GetPoint(const cst816t_t *touch,
                      int32_t *x,
                      int32_t *y);

/** @brief 转发 I2C DMA 接收完成事件。 */
void CST816T_MemRxCpltCallback(cst816t_t *touch,
                               I2C_HandleTypeDef *i2c);
/** @brief 转发 I2C 传输错误事件。 */
void CST816T_ErrorCallback(cst816t_t *touch,
                           I2C_HandleTypeDef *i2c);

#ifdef __cplusplus
}
#endif

#endif /* CST816T_H */
