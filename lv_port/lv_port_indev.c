/**
 * @file lv_port_indev.c
 * @brief LVGL CST816T 输入端口实现
 */

#include "lv_port_indev.h"

static cst816t_t g_touch;
static lv_indev_t *g_touch_indev;

static void lv_port_indev_read(lv_indev_t *indev,
                               lv_indev_data_t *data)
{
    int32_t x;
    int32_t y;
    bool pressed;

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

void lv_port_indev_process(void)
{
    CST816T_Process(&g_touch);
}

void lv_port_indev_notify_interrupt(void)
{
    CST816T_NotifyInterrupt(&g_touch);
}

void lv_port_indev_mem_rx_cplt_callback(I2C_HandleTypeDef *i2c)
{
    CST816T_MemRxCpltCallback(&g_touch, i2c);
}

void lv_port_indev_error_callback(I2C_HandleTypeDef *i2c)
{
    CST816T_ErrorCallback(&g_touch, i2c);
}
