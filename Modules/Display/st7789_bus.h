#ifndef ST7789_BUS_H
#define ST7789_BUS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stm32g4xx_hal.h"

/** @brief 像素 DMA 传输结束后向上层报告结果的回调函数类型。 */
typedef void (*st7789_bus_transfer_callback_t)(void *context, bool success);

typedef struct {
    SPI_HandleTypeDef *spi; /**< 与 ST7789 通信的 SPI 句柄。 */

    GPIO_TypeDef *cs_port;  /**< 片选信号所在的 GPIO 端口。 */
    uint16_t cs_pin;        /**< 片选信号的 GPIO 引脚。 */

    GPIO_TypeDef *dc_port;  /**< 数据/命令信号所在的 GPIO 端口。 */
    uint16_t dc_pin;        /**< 数据/命令信号的 GPIO 引脚。 */

    GPIO_TypeDef *reset_port; /**< 复位信号所在的 GPIO 端口。 */
    uint16_t reset_pin;       /**< 复位信号的 GPIO 引脚。 */

    GPIO_TypeDef *backlight_port; /**< 背光控制端口，直连电源时为 NULL。 */
    uint16_t backlight_pin;       /**< 背光控制 GPIO 引脚。 */

    st7789_bus_transfer_callback_t transfer_callback; /**< 像素 DMA 结束后的通知回调。 */
    void *callback_context;                            /**< 原样传给通知回调的用户上下文。 */
} st7789_bus_config_t;

typedef struct {
    st7789_bus_config_t config;    /**< 当前总线实例使用的硬件配置副本。 */
    volatile bool transfer_busy;   /**< 像素 DMA 传输是否仍在进行。 */
    bool initialized;              /**< 总线实例是否已完成初始化。 */
} st7789_bus_t;

/** @brief 初始化 ST7789 SPI 总线实例。 */
HAL_StatusTypeDef ST7789_Bus_Init(st7789_bus_t *bus,
                                  const st7789_bus_config_t *config);
/** @brief 按面板时序执行硬件复位。 */
void ST7789_Bus_ResetPanel(st7789_bus_t *bus);
/** @brief 控制可选的面板背光 GPIO。 */
void ST7789_Bus_SetBacklight(st7789_bus_t *bus, bool enabled);

/** @brief 发送 ST7789 命令及可选参数。 */
HAL_StatusTypeDef ST7789_Bus_WriteCommand(st7789_bus_t *bus,
                                          const uint8_t *command,
                                          size_t command_size,
                                          const uint8_t *parameters,
                                          size_t parameter_size);
/** @brief 使用 SPI DMA 异步发送像素数据。 */
HAL_StatusTypeDef ST7789_Bus_WritePixelsDma(st7789_bus_t *bus,
                                            const uint8_t *command,
                                            size_t command_size,
                                            uint8_t *pixels,
                                            size_t pixel_size);

/** @brief 转发 SPI 像素 DMA 发送完成事件。 */
void ST7789_Bus_TxCpltCallback(st7789_bus_t *bus,
                               SPI_HandleTypeDef *spi);
/** @brief 转发 SPI 像素 DMA 错误事件。 */
void ST7789_Bus_ErrorCallback(st7789_bus_t *bus,
                              SPI_HandleTypeDef *spi);

#ifdef __cplusplus
}
#endif

#endif /* ST7789_BUS_H */
