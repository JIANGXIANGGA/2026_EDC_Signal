#include "st7789_bus.h"

#define ST7789_COMMAND_TIMEOUT_MS 10U

/** @brief 结束像素 DMA 事务、释放片选并通知上层。 */
static void st7789_bus_finish_transfer(st7789_bus_t *bus, bool success)
{
    HAL_GPIO_WritePin(bus->config.cs_port,
                      bus->config.cs_pin,
                      GPIO_PIN_SET);
    bus->transfer_busy = false;

    if(bus->config.transfer_callback != NULL) {
        bus->config.transfer_callback(bus->config.callback_context, success);
    }
}

/** @brief 校验并保存 ST7789 SPI 与 GPIO 总线配置。 */
HAL_StatusTypeDef ST7789_Bus_Init(st7789_bus_t *bus,
                                  const st7789_bus_config_t *config)
{
    if((bus == NULL) || (config == NULL) || (config->spi == NULL) ||
       (config->cs_port == NULL) || (config->dc_port == NULL) ||
       (config->reset_port == NULL) || (config->spi->hdmatx == NULL)) {
        return HAL_ERROR;
    }

    bus->config = *config;
    bus->transfer_busy = false;
    bus->initialized = false;

    HAL_GPIO_WritePin(config->cs_port, config->cs_pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(config->dc_port, config->dc_pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(config->reset_port, config->reset_pin, GPIO_PIN_SET);
    if(config->backlight_port != NULL) {
        HAL_GPIO_WritePin(config->backlight_port,
                          config->backlight_pin,
                          GPIO_PIN_RESET);
    }

    bus->initialized = true;
    return HAL_OK;
}

/** @brief 按 ST7789 时序拉低复位信号并等待面板启动。 */
void ST7789_Bus_ResetPanel(st7789_bus_t *bus)
{
    if((bus == NULL) || !bus->initialized) {
        return;
    }

    HAL_GPIO_WritePin(bus->config.reset_port,
                      bus->config.reset_pin,
                      GPIO_PIN_RESET);
    HAL_Delay(20U);
    HAL_GPIO_WritePin(bus->config.reset_port,
                      bus->config.reset_pin,
                      GPIO_PIN_SET);
    HAL_Delay(150U);
}

/** @brief 设置可选的显示背光 GPIO 输出状态。 */
void ST7789_Bus_SetBacklight(st7789_bus_t *bus, bool enabled)
{
    if((bus == NULL) || !bus->initialized ||
       (bus->config.backlight_port == NULL)) {
        return;
    }

    HAL_GPIO_WritePin(bus->config.backlight_port,
                      bus->config.backlight_pin,
                      enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/** @brief 发送 ST7789 命令及其可选参数。 */
HAL_StatusTypeDef ST7789_Bus_WriteCommand(st7789_bus_t *bus,
                                          const uint8_t *command,
                                          size_t command_size,
                                          const uint8_t *parameters,
                                          size_t parameter_size)
{
    HAL_StatusTypeDef status; /* SPI 命令或参数传输的 HAL 返回状态。 */

    if((bus == NULL) || !bus->initialized || bus->transfer_busy ||
       (command == NULL) || (command_size == 0U) ||
       (command_size > UINT16_MAX) || (parameter_size > UINT16_MAX)) {
        return HAL_ERROR;
    }

    HAL_GPIO_WritePin(bus->config.cs_port,
                      bus->config.cs_pin,
                      GPIO_PIN_RESET);
    HAL_GPIO_WritePin(bus->config.dc_port,
                      bus->config.dc_pin,
                      GPIO_PIN_RESET);
    status = HAL_SPI_Transmit(bus->config.spi,
                              (uint8_t *)command,
                              (uint16_t)command_size,
                              ST7789_COMMAND_TIMEOUT_MS);

    if((status == HAL_OK) && (parameters != NULL) &&
       (parameter_size > 0U)) {
        HAL_GPIO_WritePin(bus->config.dc_port,
                          bus->config.dc_pin,
                          GPIO_PIN_SET);
        status = HAL_SPI_Transmit(bus->config.spi,
                                  (uint8_t *)parameters,
                                  (uint16_t)parameter_size,
                                  ST7789_COMMAND_TIMEOUT_MS);
    }

    HAL_GPIO_WritePin(bus->config.cs_port,
                      bus->config.cs_pin,
                      GPIO_PIN_SET);
    return status;
}

/** @brief 发送显存写入命令并使用 SPI DMA 异步发送像素数据。 */
HAL_StatusTypeDef ST7789_Bus_WritePixelsDma(st7789_bus_t *bus,
                                            const uint8_t *command,
                                            size_t command_size,
                                            uint8_t *pixels,
                                            size_t pixel_size)
{
    HAL_StatusTypeDef status; /* SPI 命令及像素 DMA 启动的 HAL 返回状态。 */

    if((bus == NULL) || !bus->initialized || bus->transfer_busy ||
       (command == NULL) || (command_size == 0U) || (pixels == NULL) ||
       (pixel_size == 0U) || (command_size > UINT16_MAX) ||
       (pixel_size > UINT16_MAX)) {
        return HAL_ERROR;
    }

    HAL_GPIO_WritePin(bus->config.cs_port,
                      bus->config.cs_pin,
                      GPIO_PIN_RESET);
    HAL_GPIO_WritePin(bus->config.dc_port,
                      bus->config.dc_pin,
                      GPIO_PIN_RESET);
    status = HAL_SPI_Transmit(bus->config.spi,
                              (uint8_t *)command,
                              (uint16_t)command_size,
                              ST7789_COMMAND_TIMEOUT_MS);
    if(status != HAL_OK) {
        HAL_GPIO_WritePin(bus->config.cs_port,
                          bus->config.cs_pin,
                          GPIO_PIN_SET);
        return status;
    }

    HAL_GPIO_WritePin(bus->config.dc_port,
                      bus->config.dc_pin,
                      GPIO_PIN_SET);
    bus->transfer_busy = true;
    status = HAL_SPI_Transmit_DMA(bus->config.spi,
                                  pixels,
                                  (uint16_t)pixel_size);
    if(status != HAL_OK) {
        bus->transfer_busy = false;
        HAL_GPIO_WritePin(bus->config.cs_port,
                          bus->config.cs_pin,
                          GPIO_PIN_SET);
    }

    return status;
}

/** @brief 处理匹配 SPI 的像素 DMA 发送完成事件。 */
void ST7789_Bus_TxCpltCallback(st7789_bus_t *bus,
                               SPI_HandleTypeDef *spi)
{
    if((bus != NULL) && bus->initialized &&
       (spi == bus->config.spi) && bus->transfer_busy) {
        st7789_bus_finish_transfer(bus, true);
    }
}

/** @brief 处理匹配 SPI 的像素 DMA 发送错误事件。 */
void ST7789_Bus_ErrorCallback(st7789_bus_t *bus,
                              SPI_HandleTypeDef *spi)
{
    if((bus != NULL) && bus->initialized &&
       (spi == bus->config.spi) && bus->transfer_busy) {
        st7789_bus_finish_transfer(bus, false);
    }
}
