#ifndef TJC_UART_DRIVER_H
#define TJC_UART_DRIVER_H

#include <stdint.h>

#include "stm32g4xx_hal.h"

#define TJC_UART_DRIVER_RX_DMA_BUFFER_SIZE 256U

typedef struct {
    uint8_t initialized;
    uint8_t rx_active;
    HAL_StatusTypeDef last_hal_status;
    uint32_t last_uart_error;
    uint32_t rx_dma_event_count;
    uint32_t rx_bytes_received;
    uint32_t rx_bytes_delivered;
    uint32_t rx_bytes_dropped;
    uint32_t rx_restart_count;
    uint32_t uart_error_count;
    uint32_t tx_complete_count;
    uint32_t tx_error_count;
} tjc_uart_driver_status_t;

HAL_StatusTypeDef TJC_UART_Driver_Init(UART_HandleTypeDef *uart);
HAL_StatusTypeDef TJC_UART_Driver_Process(void);
HAL_StatusTypeDef TJC_UART_Driver_Transmit(const uint8_t *data,
                                           uint16_t length);
uint16_t TJC_UART_Driver_Read(uint8_t *data, uint16_t capacity);
uint8_t TJC_UART_Driver_TakeTxComplete(void);
const tjc_uart_driver_status_t *TJC_UART_Driver_GetStatus(void);

#endif
