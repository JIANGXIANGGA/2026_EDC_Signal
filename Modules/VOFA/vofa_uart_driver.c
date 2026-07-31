#include "vofa_uart_driver.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    vofa_uart_driver_status_t status;
} vofa_uart_driver_context_t;

static vofa_uart_driver_context_t g_vofa_uart_driver;
static UART_HandleTypeDef g_vofa_uart;
static DMA_HandleTypeDef g_vofa_tx_dma;

static HAL_StatusTypeDef vofa_uart_driver_fail(HAL_StatusTypeDef status)
{
    g_vofa_uart_driver.status.last_hal_status = status;
    g_vofa_uart_driver.status.tx_error_count++;
    return status;
}

HAL_StatusTypeDef VOFA_UART_Driver_Init(void)
{
    RCC_PeriphCLKInitTypeDef clock = {0};
    GPIO_InitTypeDef gpio = {0};
    HAL_StatusTypeDef status;

    g_vofa_uart_driver = (vofa_uart_driver_context_t){0};
    g_vofa_uart = (UART_HandleTypeDef){0};
    g_vofa_tx_dma = (DMA_HandleTypeDef){0};

    clock.PeriphClockSelection = RCC_PERIPHCLK_USART2;
    clock.Usart2ClockSelection = RCC_USART2CLKSOURCE_PCLK1;
    status = HAL_RCCEx_PeriphCLKConfig(&clock);
    if (status != HAL_OK) {
        return vofa_uart_driver_fail(status);
    }

    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_DMAMUX1_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();
    __HAL_RCC_USART2_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_5;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOD, &gpio);

    g_vofa_tx_dma.Instance = DMA2_Channel1;
    g_vofa_tx_dma.Init.Request = DMA_REQUEST_USART2_TX;
    g_vofa_tx_dma.Init.Direction = DMA_MEMORY_TO_PERIPH;
    g_vofa_tx_dma.Init.PeriphInc = DMA_PINC_DISABLE;
    g_vofa_tx_dma.Init.MemInc = DMA_MINC_ENABLE;
    g_vofa_tx_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    g_vofa_tx_dma.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    g_vofa_tx_dma.Init.Mode = DMA_NORMAL;
    g_vofa_tx_dma.Init.Priority = DMA_PRIORITY_LOW;
    status = HAL_DMA_Init(&g_vofa_tx_dma);
    if (status != HAL_OK) {
        return vofa_uart_driver_fail(status);
    }

    g_vofa_uart.Instance = USART2;
    g_vofa_uart.Init.BaudRate = VOFA_UART_BAUD_RATE;
    g_vofa_uart.Init.WordLength = UART_WORDLENGTH_8B;
    g_vofa_uart.Init.StopBits = UART_STOPBITS_1;
    g_vofa_uart.Init.Parity = UART_PARITY_NONE;
    g_vofa_uart.Init.Mode = UART_MODE_TX;
    g_vofa_uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    g_vofa_uart.Init.OverSampling = UART_OVERSAMPLING_16;
    g_vofa_uart.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    g_vofa_uart.Init.ClockPrescaler = UART_PRESCALER_DIV1;
    g_vofa_uart.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    __HAL_LINKDMA(&g_vofa_uart, hdmatx, g_vofa_tx_dma);

    status = HAL_UART_Init(&g_vofa_uart);
    if (status == HAL_OK) {
        status = HAL_UARTEx_SetTxFifoThreshold(
            &g_vofa_uart, UART_TXFIFO_THRESHOLD_1_8);
    }
    if (status == HAL_OK) {
        status = HAL_UARTEx_DisableFifoMode(&g_vofa_uart);
    }
    if (status != HAL_OK) {
        return vofa_uart_driver_fail(status);
    }

    HAL_NVIC_SetPriority(DMA2_Channel1_IRQn, 5U, 0U);
    HAL_NVIC_EnableIRQ(DMA2_Channel1_IRQn);
    HAL_NVIC_SetPriority(USART2_IRQn, 5U, 0U);
    HAL_NVIC_EnableIRQ(USART2_IRQn);

    g_vofa_uart_driver.status.initialized = 1U;
    g_vofa_uart_driver.status.last_hal_status = HAL_OK;
    return HAL_OK;
}

HAL_StatusTypeDef VOFA_UART_Driver_Process(void)
{
    uint32_t uart_error;

    if (g_vofa_uart_driver.status.initialized == 0U) {
        return HAL_ERROR;
    }

    uart_error = HAL_UART_GetError(&g_vofa_uart);
    if (uart_error != HAL_UART_ERROR_NONE) {
        g_vofa_uart_driver.status.last_uart_error = uart_error;
        if (g_vofa_uart_driver.status.tx_busy != 0U) {
            (void)HAL_UART_AbortTransmit(&g_vofa_uart);
            g_vofa_uart_driver.status.tx_busy = 0U;
        }
        return vofa_uart_driver_fail(HAL_ERROR);
    }

    if ((g_vofa_uart_driver.status.tx_busy != 0U) &&
        (HAL_UART_GetState(&g_vofa_uart) == HAL_UART_STATE_READY)) {
        g_vofa_uart_driver.status.tx_busy = 0U;
        g_vofa_uart_driver.status.tx_complete_count++;
    }

    g_vofa_uart_driver.status.last_hal_status = HAL_OK;
    return HAL_OK;
}

HAL_StatusTypeDef VOFA_UART_Driver_Transmit(const uint8_t *data,
                                             uint16_t length)
{
    HAL_StatusTypeDef status;

    if ((g_vofa_uart_driver.status.initialized == 0U) ||
        (data == NULL) || (length == 0U)) {
        return vofa_uart_driver_fail(HAL_ERROR);
    }

    status = VOFA_UART_Driver_Process();
    if (status != HAL_OK) {
        return status;
    }
    if ((g_vofa_uart_driver.status.tx_busy != 0U) ||
        (HAL_UART_GetState(&g_vofa_uart) != HAL_UART_STATE_READY)) {
        g_vofa_uart_driver.status.tx_busy_count++;
        g_vofa_uart_driver.status.last_hal_status = HAL_BUSY;
        return HAL_BUSY;
    }

    status = HAL_UART_Transmit_DMA(&g_vofa_uart, data, length);
    if (status != HAL_OK) {
        if (status == HAL_BUSY) {
            g_vofa_uart_driver.status.tx_busy_count++;
            g_vofa_uart_driver.status.last_hal_status = HAL_BUSY;
            return HAL_BUSY;
        }
        return vofa_uart_driver_fail(status);
    }

    g_vofa_uart_driver.status.tx_busy = 1U;
    g_vofa_uart_driver.status.tx_start_count++;
    g_vofa_uart_driver.status.last_hal_status = HAL_OK;
    return HAL_OK;
}

uint8_t VOFA_UART_Driver_IsBusy(void)
{
    (void)VOFA_UART_Driver_Process();
    return g_vofa_uart_driver.status.tx_busy;
}

const vofa_uart_driver_status_t *VOFA_UART_Driver_GetStatus(void)
{
    return &g_vofa_uart_driver.status;
}

void DMA2_Channel1_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&g_vofa_tx_dma);
}

void USART2_IRQHandler(void)
{
    HAL_UART_IRQHandler(&g_vofa_uart);
}
