/**
 * @file lv_port_indev.c
 * @brief LVGL CST816T 输入端口实现
 */

#include "lv_port_indev.h"

static cst816t_t g_touch;          /* CST816T 触摸控制器实例。 */
static lv_indev_t *g_touch_indev;  /* LVGL 指针输入设备对象。 */

/** @brief 向 LVGL 返回最近一次触摸坐标和按下状态。 */
static void lv_port_indev_read(lv_indev_t *indev,
                               lv_indev_data_t *data)
{
    int32_t x;   /* 最近读取的触摸 X 坐标。 */
    int32_t y;   /* 最近读取的触摸 Y 坐标。 */
    bool pressed; /* 当前触摸按下状态。 */

    LV_UNUSED(indev);
    pressed = CST816T_GetPoint(&g_touch, &x, &y);
    data->point.x = x;
    data->point.y = y;
    if(pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
    }
    else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/** @brief 初始化 CST816T 并注册为 LVGL 指针输入设备。 */
lv_indev_t *lv_port_indev_init(const cst816t_config_t *config,
                               lv_display_t *display)
{
    if((config == NULL) || (display == NULL)) {
        return NULL;
    }

    g_touch_indev = NULL;
    if(CST816T_Init(&g_touch, config) != HAL_OK) {
        return NULL;
    }

    g_touch_indev = lv_indev_create();
    if(g_touch_indev == NULL) {
        return NULL;
    }

    lv_indev_set_type(g_touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(g_touch_indev, lv_port_indev_read);
    lv_indev_set_display(g_touch_indev, display);
    return g_touch_indev;
}

/** @brief 在主循环中执行触摸帧解析和读取调度。 */
void lv_port_indev_process(void)
{
    CST816T_Process(&g_touch);
}

/** @brief 将触摸 GPIO 中断转发给 CST816T Driver。 */
void lv_port_indev_notify_interrupt(void)
{
    CST816T_NotifyInterrupt(&g_touch);
}

/** @brief 将 I2C DMA 接收完成事件转发给 CST816T Driver。 */
void lv_port_indev_mem_rx_cplt_callback(I2C_HandleTypeDef *i2c)
{
    CST816T_MemRxCpltCallback(&g_touch, i2c);
}

/** @brief 将 I2C 错误事件转发给 CST816T Driver。 */
void lv_port_indev_error_callback(I2C_HandleTypeDef *i2c)
{
    CST816T_ErrorCallback(&g_touch, i2c);
}
