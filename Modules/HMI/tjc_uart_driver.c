#include "tjc_uart_driver.h"

#include <stddef.h>
#include <string.h>

#define TJC_UART_DRIVER_IRQ_PRIORITY 7U

typedef struct {
    UART_HandleTypeDef *uart;
    DMA_HandleTypeDef rx_dma;
    DMA_HandleTypeDef tx_dma;
    uint8_t rx_dma_buffer[TJC_UART_DRIVER_RX_DMA_BUFFER_SIZE];
    volatile uint32_t rx_produced;
    uint32_t rx_consumed;
    volatile uint16_t rx_event_position;
    volatile uint8_t rx_restart_pending;
    volatile uint8_t tx_complete_pending;
    tjc_uart_driver_status_t status;
} tjc_uart_driver_context_t;

static tjc_uart_driver_context_t g_tjc_uart_driver;

static HAL_StatusTypeDef tjc_uart_driver_start_receive(void)
{
    HAL_StatusTypeDef status;

    g_tjc_uart_driver.rx_event_position = 0U;
    g_tjc_uart_driver.rx_produced = 0U;
    g_tjc_uart_driver.rx_consumed = 0U;
    g_tjc_uart_driver.status.rx_active = 0U;

    status = HAL_UARTEx_ReceiveToIdle_DMA(
        g_tjc_uart_driver.uart,
        g_tjc_uart_driver.rx_dma_buffer,
        TJC_UART_DRIVER_RX_DMA_BUFFER_SIZE);
    g_tjc_uart_driver.status.last_hal_status = status;
    if (status == HAL_OK) {
        /* IDLE 和 DMA 全满事件足以发布数据，关闭半满中断降低中断频率。 */
        __HAL_DMA_DISABLE_IT(&g_tjc_uart_driver.rx_dma, DMA_IT_HT);
        g_tjc_uart_driver.status.rx_active = 1U;
    }

    return status;
}

static HAL_StatusTypeDef tjc_uart_driver_init_dma(void)
{
    HAL_StatusTypeDef status;

    __HAL_RCC_DMAMUX1_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();

    g_tjc_uart_driver.rx_dma.Instance = DMA1_Channel6;
    g_tjc_uart_driver.rx_dma.Init.Request = DMA_REQUEST_USART1_RX;
    g_tjc_uart_driver.rx_dma.Init.Direction = DMA_PERIPH_TO_MEMORY;
    g_tjc_uart_driver.rx_dma.Init.PeriphInc = DMA_PINC_DISABLE;
    g_tjc_uart_driver.rx_dma.Init.MemInc = DMA_MINC_ENABLE;
    g_tjc_uart_driver.rx_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    g_tjc_uart_driver.rx_dma.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    g_tjc_uart_driver.rx_dma.Init.Mode = DMA_CIRCULAR;
    g_tjc_uart_driver.rx_dma.Init.Priority = DMA_PRIORITY_LOW;
    status = HAL_DMA_Init(&g_tjc_uart_driver.rx_dma);
    if (status != HAL_OK) {
        return status;
    }

    g_tjc_uart_driver.tx_dma.Instance = DMA1_Channel7;
    g_tjc_uart_driver.tx_dma.Init.Request = DMA_REQUEST_USART1_TX;
    g_tjc_uart_driver.tx_dma.Init.Direction = DMA_MEMORY_TO_PERIPH;
    g_tjc_uart_driver.tx_dma.Init.PeriphInc = DMA_PINC_DISABLE;
    g_tjc_uart_driver.tx_dma.Init.MemInc = DMA_MINC_ENABLE;
    g_tjc_uart_driver.tx_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    g_tjc_uart_driver.tx_dma.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    g_tjc_uart_driver.tx_dma.Init.Mode = DMA_NORMAL;
    g_tjc_uart_driver.tx_dma.Init.Priority = DMA_PRIORITY_LOW;
    status = HAL_DMA_Init(&g_tjc_uart_driver.tx_dma);
    if (status != HAL_OK) {
        (void)HAL_DMA_DeInit(&g_tjc_uart_driver.rx_dma);
        return status;
    }

    __HAL_LINKDMA(g_tjc_uart_driver.uart,
                  hdmarx,
                  g_tjc_uart_driver.rx_dma);
    __HAL_LINKDMA(g_tjc_uart_driver.uart,
                  hdmatx,
                  g_tjc_uart_driver.tx_dma);
    return HAL_OK;
}

static void tjc_uart_driver_publish_rx_position(
    uint16_t size,
    HAL_UART_RxEventTypeTypeDef event_type)
{
    const uint16_t previous = g_tjc_uart_driver.rx_event_position;
    uint16_t current;
    uint16_t received;

    if ((size == 0U) ||
        (size > TJC_UART_DRIVER_RX_DMA_BUFFER_SIZE)) {
        g_tjc_uart_driver.rx_restart_pending = 1U;
        return;
    }

    if (size == TJC_UART_DRIVER_RX_DMA_BUFFER_SIZE) {
        current = 0U;
        received = ((event_type == HAL_UART_RXEVENT_IDLE) &&
                    (previous == 0U)) ?
                       0U :
                       (uint16_t)(TJC_UART_DRIVER_RX_DMA_BUFFER_SIZE -
                                  previous);
    } else {
        current = size;
        received = (current >= previous) ?
                       (uint16_t)(current - previous) :
                       (uint16_t)(TJC_UART_DRIVER_RX_DMA_BUFFER_SIZE -
                                  previous + current);
    }

    g_tjc_uart_driver.rx_event_position = current;
    g_tjc_uart_driver.rx_produced += received;
    g_tjc_uart_driver.status.rx_dma_event_count++;
    g_tjc_uart_driver.status.rx_bytes_received += received;
}

HAL_StatusTypeDef TJC_UART_Driver_Init(UART_HandleTypeDef *uart)
{
    HAL_StatusTypeDef status;

    if ((uart == NULL) || (uart->Instance != USART1) ||
        (g_tjc_uart_driver.status.initialized != 0U) ||
        (uart->hdmarx != NULL) || (uart->hdmatx != NULL)) {
        return HAL_ERROR;
    }

    memset(&g_tjc_uart_driver, 0, sizeof(g_tjc_uart_driver));
    g_tjc_uart_driver.uart = uart;

    status = tjc_uart_driver_init_dma();
    if (status != HAL_OK) {
        g_tjc_uart_driver.status.last_hal_status = status;
        return status;
    }

    HAL_NVIC_SetPriority(DMA1_Channel6_IRQn,
                         TJC_UART_DRIVER_IRQ_PRIORITY,
                         0U);
    HAL_NVIC_EnableIRQ(DMA1_Channel6_IRQn);
    HAL_NVIC_SetPriority(DMA1_Channel7_IRQn,
                         TJC_UART_DRIVER_IRQ_PRIORITY,
                         0U);
    HAL_NVIC_EnableIRQ(DMA1_Channel7_IRQn);
    HAL_NVIC_SetPriority(USART1_IRQn, TJC_UART_DRIVER_IRQ_PRIORITY, 0U);
    HAL_NVIC_EnableIRQ(USART1_IRQn);

    g_tjc_uart_driver.status.initialized = 1U;
    status = tjc_uart_driver_start_receive();
    if (status != HAL_OK) {
        g_tjc_uart_driver.status.initialized = 0U;
        return status;
    }

    return HAL_OK;
}

HAL_StatusTypeDef TJC_UART_Driver_Process(void)
{
    HAL_StatusTypeDef status;
    uint32_t produced_count;
    uint32_t unread_count;

    if (g_tjc_uart_driver.status.initialized == 0U) {
        return HAL_ERROR;
    }
    if (g_tjc_uart_driver.rx_restart_pending == 0U) {
        return HAL_OK;
    }

    produced_count = g_tjc_uart_driver.rx_produced;
    unread_count = produced_count - g_tjc_uart_driver.rx_consumed;
    g_tjc_uart_driver.status.rx_bytes_dropped += unread_count;
    g_tjc_uart_driver.rx_consumed = produced_count;
    g_tjc_uart_driver.status.rx_active = 0U;

    status = HAL_UART_AbortReceive(g_tjc_uart_driver.uart);
    if (status == HAL_OK) {
        /* 先消费旧恢复请求，启动期间出现的新错误仍会重新置位。 */
        g_tjc_uart_driver.rx_restart_pending = 0U;
        status = tjc_uart_driver_start_receive();
        if (status != HAL_OK) {
            g_tjc_uart_driver.rx_restart_pending = 1U;
        }
    }

    g_tjc_uart_driver.status.last_hal_status = status;
    if (status == HAL_OK) {
        g_tjc_uart_driver.status.rx_restart_count++;
    }

    return status;
}

HAL_StatusTypeDef TJC_UART_Driver_Transmit(const uint8_t *data,
                                           uint16_t length)
{
    HAL_StatusTypeDef status;

    if ((g_tjc_uart_driver.status.initialized == 0U) ||
        (data == NULL) || (length == 0U)) {
        return HAL_ERROR;
    }

    status = HAL_UART_Transmit_DMA(g_tjc_uart_driver.uart,
                                   data,
                                   length);
    g_tjc_uart_driver.status.last_hal_status = status;
    if ((status != HAL_OK) && (status != HAL_BUSY)) {
        g_tjc_uart_driver.status.tx_error_count++;
    }
    return status;
}

uint16_t TJC_UART_Driver_Read(uint8_t *data, uint16_t capacity)
{
    const uint32_t produced = g_tjc_uart_driver.rx_produced;
    uint32_t available;
    uint16_t read_length;
    uint16_t read_index;
    uint16_t first_length;

    if ((g_tjc_uart_driver.status.initialized == 0U) ||
        (data == NULL) || (capacity == 0U)) {
        return 0U;
    }

    available = produced - g_tjc_uart_driver.rx_consumed;
    if (available > TJC_UART_DRIVER_RX_DMA_BUFFER_SIZE) {
        const uint32_t dropped =
            available - TJC_UART_DRIVER_RX_DMA_BUFFER_SIZE;
        g_tjc_uart_driver.rx_consumed += dropped;
        g_tjc_uart_driver.status.rx_bytes_dropped += dropped;
        available = TJC_UART_DRIVER_RX_DMA_BUFFER_SIZE;
    }

    read_length = (available > capacity) ? capacity : (uint16_t)available;
    if (read_length == 0U) {
        return 0U;
    }

    read_index = (uint16_t)(g_tjc_uart_driver.rx_consumed %
                            TJC_UART_DRIVER_RX_DMA_BUFFER_SIZE);
    first_length = (uint16_t)(TJC_UART_DRIVER_RX_DMA_BUFFER_SIZE -
                              read_index);
    if (first_length > read_length) {
        first_length = read_length;
    }

    memcpy(data,
           &g_tjc_uart_driver.rx_dma_buffer[read_index],
           first_length);
    if (read_length > first_length) {
        memcpy(&data[first_length],
               g_tjc_uart_driver.rx_dma_buffer,
               (uint16_t)(read_length - first_length));
    }

    g_tjc_uart_driver.rx_consumed += read_length;
    g_tjc_uart_driver.status.rx_bytes_delivered += read_length;
    return read_length;
}

uint8_t TJC_UART_Driver_TakeTxComplete(void)
{
    uint8_t completed;

    completed = g_tjc_uart_driver.tx_complete_pending;
    g_tjc_uart_driver.tx_complete_pending = 0U;
    return completed;
}

const tjc_uart_driver_status_t *TJC_UART_Driver_GetStatus(void)
{
    return &g_tjc_uart_driver.status;
}

void DMA1_Channel6_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&g_tjc_uart_driver.rx_dma);
}

void DMA1_Channel7_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&g_tjc_uart_driver.tx_dma);
}

void USART1_IRQHandler(void)
{
    if (g_tjc_uart_driver.uart != NULL) {
        HAL_UART_IRQHandler(g_tjc_uart_driver.uart);
    }
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *uart, uint16_t size)
{
    if ((g_tjc_uart_driver.status.initialized != 0U) &&
        (uart == g_tjc_uart_driver.uart)) {
        tjc_uart_driver_publish_rx_position(
            size,
            HAL_UARTEx_GetRxEventType(uart));
    }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *uart)
{
    if ((g_tjc_uart_driver.status.initialized != 0U) &&
        (uart == g_tjc_uart_driver.uart)) {
        g_tjc_uart_driver.tx_complete_pending = 1U;
        g_tjc_uart_driver.status.tx_complete_count++;
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart)
{
    if ((g_tjc_uart_driver.status.initialized != 0U) &&
        (uart == g_tjc_uart_driver.uart)) {
        g_tjc_uart_driver.status.last_uart_error =
            HAL_UART_GetError(uart);
        g_tjc_uart_driver.status.uart_error_count++;
        g_tjc_uart_driver.status.rx_active = 0U;
        g_tjc_uart_driver.rx_restart_pending = 1U;
        g_tjc_uart_driver.tx_complete_pending = 1U;
    }
}
