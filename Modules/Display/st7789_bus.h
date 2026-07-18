#ifndef ST7789_BUS_H
#define ST7789_BUS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stm32g4xx_hal.h"

typedef void (*st7789_bus_transfer_callback_t)(void *context, bool success);

typedef struct {
    SPI_HandleTypeDef *spi;

    GPIO_TypeDef *cs_port;
    uint16_t cs_pin;

    GPIO_TypeDef *dc_port;
    uint16_t dc_pin;

    GPIO_TypeDef *reset_port;
    uint16_t reset_pin;

    GPIO_TypeDef *backlight_port;
    uint16_t backlight_pin;

    st7789_bus_transfer_callback_t transfer_callback;
    void *callback_context;
} st7789_bus_config_t;

typedef struct {
    st7789_bus_config_t config;
    volatile bool transfer_busy;
    bool initialized;
} st7789_bus_t;

HAL_StatusTypeDef ST7789_Bus_Init(st7789_bus_t *bus,
                                  const st7789_bus_config_t *config);
void ST7789_Bus_ResetPanel(st7789_bus_t *bus);
void ST7789_Bus_SetBacklight(st7789_bus_t *bus, bool enabled);

HAL_StatusTypeDef ST7789_Bus_WriteCommand(st7789_bus_t *bus,
                                          const uint8_t *command,
                                          size_t command_size,
                                          const uint8_t *parameters,
                                          size_t parameter_size);
HAL_StatusTypeDef ST7789_Bus_WritePixelsDma(st7789_bus_t *bus,
                                            const uint8_t *command,
                                            size_t command_size,
                                            uint8_t *pixels,
                                            size_t pixel_size);

void ST7789_Bus_TxCpltCallback(st7789_bus_t *bus,
                               SPI_HandleTypeDef *spi);
void ST7789_Bus_ErrorCallback(st7789_bus_t *bus,
                              SPI_HandleTypeDef *spi);

#ifdef __cplusplus
}
#endif

#endif /* ST7789_BUS_H */
