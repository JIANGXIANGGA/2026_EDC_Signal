/**
 * @file lv_port_display.h
 * @brief LVGL ST7789 显示端口接口
 */

#ifndef LV_PORT_DISPLAY_H
#define LV_PORT_DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "st7789_bus.h"

#define LV_PORT_DISPLAY_MAX_HOR_RES 320U
#define LV_PORT_DISPLAY_BUF_LINES   20U

typedef struct {
    st7789_bus_config_t bus;             /**< ST7789 底层 SPI 总线配置。 */
    uint16_t horizontal_resolution;      /**< 显示面板水平像素数。 */
    uint16_t vertical_resolution;        /**< 显示面板垂直像素数。 */
    uint16_t x_gap;                      /**< ST7789 显存窗口的水平偏移。 */
    uint16_t y_gap;                      /**< ST7789 显存窗口的垂直偏移。 */
    lv_display_rotation_t rotation;      /**< LVGL 显示旋转方向。 */
    lv_lcd_flag_t flags;                 /**< LVGL ST7789 驱动功能标志。 */
    bool invert_colors;                  /**< 是否启用面板颜色反相。 */
} lv_port_display_config_t;

/** @brief 初始化 ST7789 对应的 LVGL 显示端口。 */
lv_display_t *lv_port_display_init(const lv_port_display_config_t *config);
/** @brief 转发 SPI DMA 发送完成事件。 */
void lv_port_display_tx_cplt_callback(SPI_HandleTypeDef *spi);
/** @brief 转发 SPI DMA 错误事件。 */
void lv_port_display_error_callback(SPI_HandleTypeDef *spi);

#ifdef __cplusplus
}
#endif

#endif /* LV_PORT_DISPLAY_H */
