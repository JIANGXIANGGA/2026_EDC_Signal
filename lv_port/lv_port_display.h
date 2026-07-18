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
    st7789_bus_config_t bus;
    uint16_t horizontal_resolution;
    uint16_t vertical_resolution;
    uint16_t x_gap;
    uint16_t y_gap;
    lv_display_rotation_t rotation;
    lv_lcd_flag_t flags;
    bool invert_colors;
} lv_port_display_config_t;

lv_display_t *lv_port_display_init(const lv_port_display_config_t *config);
void lv_port_display_tx_cplt_callback(SPI_HandleTypeDef *spi);
void lv_port_display_error_callback(SPI_HandleTypeDef *spi);

#ifdef __cplusplus
}
#endif

#endif /* LV_PORT_DISPLAY_H */
