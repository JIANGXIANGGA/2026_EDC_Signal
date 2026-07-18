/**
 * @file lv_port_indev.h
 * @brief LVGL CST816T 输入端口接口
 */

#ifndef LV_PORT_INDEV_H
#define LV_PORT_INDEV_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cst816t.h"
#include "lvgl.h"

lv_indev_t *lv_port_indev_init(const cst816t_config_t *config,
                               lv_display_t *display);
void lv_port_indev_process(void);
void lv_port_indev_notify_interrupt(void);
void lv_port_indev_mem_rx_cplt_callback(I2C_HandleTypeDef *i2c);
void lv_port_indev_error_callback(I2C_HandleTypeDef *i2c);

#ifdef __cplusplus
}
#endif

#endif /* LV_PORT_INDEV_H */
