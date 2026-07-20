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

/** @brief 初始化 CST816T 对应的 LVGL 指针输入端口。 */
lv_indev_t *lv_port_indev_init(const cst816t_config_t *config,
                               lv_display_t *display);
/** @brief 在主循环中处理触摸输入。 */
void lv_port_indev_process(void);
/** @brief 转发触摸 GPIO 外部中断。 */
void lv_port_indev_notify_interrupt(void);
/** @brief 转发 I2C DMA 接收完成事件。 */
void lv_port_indev_mem_rx_cplt_callback(I2C_HandleTypeDef *i2c);
/** @brief 转发 I2C 错误事件。 */
void lv_port_indev_error_callback(I2C_HandleTypeDef *i2c);

#ifdef __cplusplus
}
#endif

#endif /* LV_PORT_INDEV_H */
