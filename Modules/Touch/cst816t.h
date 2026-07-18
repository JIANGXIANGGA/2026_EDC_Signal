#ifndef CST816T_H
#define CST816T_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "stm32g4xx_hal.h"

typedef struct {
    I2C_HandleTypeDef *i2c;
    GPIO_TypeDef *reset_port;
    uint16_t reset_pin;
    uint16_t horizontal_resolution;
    uint16_t vertical_resolution;
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
    uint32_t refresh_period_ms;
} cst816t_config_t;

typedef struct {
    cst816t_config_t config;
    uint8_t rx_data[6];
    volatile bool transfer_busy;
    volatile bool transfer_complete;
    volatile bool read_requested;
    uint32_t last_read_tick;
    int32_t x;
    int32_t y;
    bool pressed;
    bool initialized;
} cst816t_t;

HAL_StatusTypeDef CST816T_Init(cst816t_t *touch,
                               const cst816t_config_t *config);
void CST816T_Process(cst816t_t *touch);
void CST816T_NotifyInterrupt(cst816t_t *touch);
bool CST816T_GetPoint(const cst816t_t *touch,
                      int32_t *x,
                      int32_t *y);

void CST816T_MemRxCpltCallback(cst816t_t *touch,
                               I2C_HandleTypeDef *i2c);
void CST816T_ErrorCallback(cst816t_t *touch,
                           I2C_HandleTypeDef *i2c);

#ifdef __cplusplus
}
#endif

#endif /* CST816T_H */
