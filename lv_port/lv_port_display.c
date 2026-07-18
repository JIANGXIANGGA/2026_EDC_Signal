/**
 * @file lv_port_display.c
 * @brief LVGL ST7789 显示端口实现
 */

#include "lv_port_display.h"

static uint16_t g_draw_buffer_1[LV_PORT_DISPLAY_MAX_HOR_RES *
                                LV_PORT_DISPLAY_BUF_LINES];
static uint16_t g_draw_buffer_2[LV_PORT_DISPLAY_MAX_HOR_RES *
                                LV_PORT_DISPLAY_BUF_LINES];

static st7789_bus_t g_lcd_bus;
static lv_display_t *g_display;
static bool g_bus_error;

static void lv_port_display_transfer_complete(void *context, bool success)
{
    lv_display_t *display = (lv_display_t *)context;

    LV_UNUSED(success);
    if(display != NULL) {
        lv_display_flush_ready(display);
    }
}

static void lv_port_display_send_command(lv_display_t *display,
                                         const uint8_t *command,
                                         size_t command_size,
                                         const uint8_t *parameters,
                                         size_t parameter_size)
{
    LV_UNUSED(display);
    if(ST7789_Bus_WriteCommand(&g_lcd_bus,
                               command,
                               command_size,
                               parameters,
                               parameter_size) != HAL_OK) {
        g_bus_error = true;
    }
}

static void lv_port_display_send_pixels(lv_display_t *display,
                                        const uint8_t *command,
                                        size_t command_size,
                                        uint8_t *pixels,
                                        size_t pixel_size)
{
    if(g_bus_error) {
        g_bus_error = false;
        lv_display_flush_ready(display);
        return;
    }

    if(ST7789_Bus_WritePixelsDma(&g_lcd_bus,
                                 command,
                                 command_size,
                                 pixels,
                                 pixel_size) != HAL_OK) {
        lv_display_flush_ready(display);
    }
}

lv_display_t *lv_port_display_init(const lv_port_display_config_t *config)
{
    st7789_bus_config_t bus_config;

    if((config == NULL) || (config->horizontal_resolution == 0U) ||
       (config->vertical_resolution == 0U) ||
       (config->horizontal_resolution > LV_PORT_DISPLAY_MAX_HOR_RES) ||
       (config->vertical_resolution > LV_PORT_DISPLAY_MAX_HOR_RES)) {
        return NULL;
    }

    g_display = NULL;
    g_bus_error = false;
    bus_config = config->bus;
    bus_config.transfer_callback = lv_port_display_transfer_complete;
    bus_config.callback_context = NULL;
    if(ST7789_Bus_Init(&g_lcd_bus, &bus_config) != HAL_OK) {
        return NULL;
    }

    ST7789_Bus_ResetPanel(&g_lcd_bus);
    g_display = lv_st7789_create(config->horizontal_resolution,
                                 config->vertical_resolution,
                                 config->flags,
                                 lv_port_display_send_command,
                                 lv_port_display_send_pixels);
    if((g_display == NULL) || g_bus_error) {
        if(g_display != NULL) {
            lv_display_delete(g_display);
            g_display = NULL;
        }
        return NULL;
    }

    g_lcd_bus.config.callback_context = g_display;
    lv_st7789_set_gap(g_display, config->x_gap, config->y_gap);
    lv_st7789_set_invert(g_display, config->invert_colors);

    /* 8-bit SPI 直接发送高字节在前的 RGB565，避免每帧进行 CPU 字节交换。 */
    lv_display_set_color_format(g_display, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(g_display,
                           g_draw_buffer_1,
                           g_draw_buffer_2,
                           sizeof(g_draw_buffer_1),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_rotation(g_display, config->rotation);
    ST7789_Bus_SetBacklight(&g_lcd_bus, true);
    return g_display;
}

void lv_port_display_tx_cplt_callback(SPI_HandleTypeDef *spi)
{
    ST7789_Bus_TxCpltCallback(&g_lcd_bus, spi);
}

void lv_port_display_error_callback(SPI_HandleTypeDef *spi)
{
    ST7789_Bus_ErrorCallback(&g_lcd_bus, spi);
}
