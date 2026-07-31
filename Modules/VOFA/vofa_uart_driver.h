#ifndef VOFA_UART_DRIVER_H
#define VOFA_UART_DRIVER_H

#include <stdint.h>

#include "stm32g4xx_hal.h"

#define VOFA_UART_BAUD_RATE 460800U

typedef struct {
    uint8_t initialized;
    uint8_t tx_busy;
    HAL_StatusTypeDef last_hal_status;
    uint32_t last_uart_error;
    uint32_t tx_start_count;
    uint32_t tx_complete_count;
    uint32_t tx_busy_count;
    uint32_t tx_error_count;
} vofa_uart_driver_status_t;

HAL_StatusTypeDef VOFA_UART_Driver_Init(void);
HAL_StatusTypeDef VOFA_UART_Driver_Process(void);
HAL_StatusTypeDef VOFA_UART_Driver_Transmit(const uint8_t *data,
                                             uint16_t length);
uint8_t VOFA_UART_Driver_IsBusy(void);
const vofa_uart_driver_status_t *VOFA_UART_Driver_GetStatus(void);

#endif
