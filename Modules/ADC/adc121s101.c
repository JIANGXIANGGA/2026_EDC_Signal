#include "adc121s101.h"

#define ADC121S101_DUMMY_WORD 0x0000U
#define ADC121S101_DATA_MASK 0x0FFFU

static SPI_HandleTypeDef *g_adc_spi;
static TIM_HandleTypeDef *g_adc_timer;
static DMA_HandleTypeDef *g_adc_dma;
static DMA_HandleTypeDef *g_adc_trigger_dma;

static uint16_t g_adc_input_buffer[ADC_INPUT_HALF_SIZE][ADC_INPUT_BLOCK_SIZE];
static uint16_t g_tx_dummy = ADC121S101_DUMMY_WORD;
static volatile uint8_t g_adc_input_running;
static volatile uint8_t g_adc_restart_requested;
static volatile uint8_t g_adc_latest_half;
static volatile uint32_t g_adc_latest_sequence;
static volatile uint32_t g_adc_copied_sequence;
static volatile uint32_t g_adc_half_complete_count;
static volatile uint32_t g_adc_complete_count;
static volatile uint32_t g_adc_error_count;
static volatile uint32_t g_adc_overrun_count;

static void ADC121S101_RxHalfCompleteCallback(DMA_HandleTypeDef *dma);
static void ADC121S101_RxCompleteCallback(DMA_HandleTypeDef *dma);
static void ADC121S101_DmaErrorCallback(DMA_HandleTypeDef *dma);
static void ADC121S101_RecordReadyHalf(uint8_t half_index);
static void ADC121S101_ClearState(void);
static void ADC121S101_ClearCounters(void);
static HAL_StatusTypeDef ADC121S101_StopInternal(void);

HAL_StatusTypeDef ADC_Input_Init(SPI_HandleTypeDef *adc_spi,
                                 TIM_HandleTypeDef *adc_timer)
{
    DMA_HandleTypeDef *trigger_dma;

    if ((adc_spi == NULL) || (adc_timer == NULL) ||
        (adc_spi->hdmarx == NULL) ||
        (adc_timer->hdma[TIM_DMA_ID_UPDATE] == NULL)) {
        return HAL_ERROR;
    }

    if (g_adc_input_running != 0U) {
        return HAL_BUSY;
    }

    trigger_dma = adc_timer->hdma[TIM_DMA_ID_UPDATE];
    if ((adc_spi->Init.Mode != SPI_MODE_MASTER) ||
        (adc_spi->Init.Direction != SPI_DIRECTION_2LINES) ||
        (adc_spi->Init.DataSize != SPI_DATASIZE_16BIT) ||
        (trigger_dma->Init.Direction != DMA_MEMORY_TO_PERIPH)) {
        return HAL_ERROR;
    }

    g_adc_spi = adc_spi;
    g_adc_timer = adc_timer;
    g_adc_dma = adc_spi->hdmarx;
    g_adc_trigger_dma = trigger_dma;
    ADC121S101_ClearState();
    ADC121S101_ClearCounters();

    return HAL_OK;
}

HAL_StatusTypeDef ADC_Start(void)
{
    HAL_StatusTypeDef status;

    if ((g_adc_spi == NULL) || (g_adc_timer == NULL) ||
        (g_adc_dma == NULL) || (g_adc_trigger_dma == NULL)) {
        return HAL_ERROR;
    }

    if (g_adc_input_running != 0U) {
        return HAL_BUSY;
    }

    ADC121S101_ClearState();
    g_tx_dummy = ADC121S101_DUMMY_WORD;

    g_adc_dma->XferHalfCpltCallback = ADC121S101_RxHalfCompleteCallback;
    g_adc_dma->XferCpltCallback = ADC121S101_RxCompleteCallback;
    g_adc_dma->XferErrorCallback = ADC121S101_DmaErrorCallback;
    g_adc_trigger_dma->XferHalfCpltCallback = NULL;
    g_adc_trigger_dma->XferCpltCallback = NULL;
    g_adc_trigger_dma->XferErrorCallback = ADC121S101_DmaErrorCallback;

    __HAL_TIM_SET_COUNTER(g_adc_timer, 0U);
    __HAL_TIM_CLEAR_FLAG(g_adc_timer, TIM_FLAG_UPDATE);
    __HAL_TIM_DISABLE_DMA(g_adc_timer, TIM_DMA_UPDATE);
    __HAL_SPI_DISABLE(g_adc_spi);
    CLEAR_BIT(g_adc_spi->Instance->CR2, SPI_CR2_RXDMAEN);
    __HAL_SPI_CLEAR_OVRFLAG(g_adc_spi);

    status = HAL_DMA_Start_IT(g_adc_dma,
                              (uint32_t)&g_adc_spi->Instance->DR,
                              (uint32_t)&g_adc_input_buffer[0][0],
                              ADC_INPUT_SAMPLE_SIZE);
    if (status != HAL_OK) {
        g_adc_error_count++;
        return status;
    }

    status = HAL_DMA_Start_IT(g_adc_trigger_dma,
                              (uint32_t)&g_tx_dummy,
                              (uint32_t)&g_adc_spi->Instance->DR,
                              ADC_INPUT_SAMPLE_SIZE);
    if (status != HAL_OK) {
        (void)HAL_DMA_Abort(g_adc_dma);
        g_adc_error_count++;
        return status;
    }

    SET_BIT(g_adc_spi->Instance->CR2, SPI_CR2_RXDMAEN);
    __HAL_SPI_ENABLE(g_adc_spi);
    __HAL_TIM_ENABLE_DMA(g_adc_timer, TIM_DMA_UPDATE);

    status = HAL_TIM_Base_Start(g_adc_timer);
    if (status != HAL_OK) {
        (void)ADC121S101_StopInternal();
        g_adc_error_count++;
        return status;
    }

    g_adc_input_running = 1U;
    return HAL_OK;
}

HAL_StatusTypeDef ADC_Process(void)
{
    if (g_adc_restart_requested == 0U) {
        return HAL_OK;
    }

    g_adc_restart_requested = 0U;
    (void)ADC121S101_StopInternal();
    return ADC_Start();
}

uint8_t ADC121S101_CopyLatestBlock(uint16_t *destination, uint32_t capacity)
{
    uint8_t half_index;
    uint32_t sequence;
    uint32_t primask;

    if ((destination == NULL) || (capacity < ADC_INPUT_BLOCK_SIZE)) {
        return 0U;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    sequence = g_adc_latest_sequence;
    half_index = g_adc_latest_half;
    __set_PRIMASK(primask);

    if ((sequence == 0U) || (sequence == g_adc_copied_sequence) ||
        (half_index >= ADC_INPUT_HALF_SIZE)) {
        return 0U;
    }

    for (uint32_t index = 0U; index < ADC_INPUT_BLOCK_SIZE; ++index) {
        destination[index] =
            g_adc_input_buffer[half_index][index] & ADC121S101_DATA_MASK;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    if ((sequence == g_adc_latest_sequence) &&
        (half_index == g_adc_latest_half)) {
        g_adc_copied_sequence = sequence;
        __set_PRIMASK(primask);
        return 1U;
    }
    __set_PRIMASK(primask);

    g_adc_overrun_count++;
    return 0U;
}

uint8_t ADC121S101_IsRunning(void)
{
    return g_adc_input_running;
}

uint32_t ADC121S101_GetHalfCompleteCount(void)
{
    return g_adc_half_complete_count;
}

uint32_t ADC121S101_GetCompleteCount(void)
{
    return g_adc_complete_count;
}

uint32_t ADC121S101_GetErrorCount(void)
{
    return g_adc_error_count;
}

uint32_t ADC121S101_GetOverrunCount(void)
{
    return g_adc_overrun_count;
}

static void ADC121S101_ClearState(void)
{
    g_adc_restart_requested = 0U;
    g_adc_latest_half = ADC_INPUT_HALF_SIZE;
    g_adc_latest_sequence = 0U;
    g_adc_copied_sequence = 0U;
}

static void ADC121S101_ClearCounters(void)
{
    g_adc_half_complete_count = 0U;
    g_adc_complete_count = 0U;
    g_adc_error_count = 0U;
    g_adc_overrun_count = 0U;
}

static HAL_StatusTypeDef ADC121S101_StopInternal(void)
{
    HAL_StatusTypeDef rx_status = HAL_OK;
    HAL_StatusTypeDef trigger_status = HAL_OK;

    if ((g_adc_spi == NULL) || (g_adc_timer == NULL)) {
        return HAL_ERROR;
    }

    (void)HAL_TIM_Base_Stop(g_adc_timer);
    __HAL_TIM_DISABLE_DMA(g_adc_timer, TIM_DMA_UPDATE);

    if (g_adc_trigger_dma != NULL) {
        trigger_status = HAL_DMA_Abort(g_adc_trigger_dma);
    }

    CLEAR_BIT(g_adc_spi->Instance->CR2, SPI_CR2_RXDMAEN);
    __HAL_SPI_DISABLE(g_adc_spi);
    __HAL_SPI_CLEAR_OVRFLAG(g_adc_spi);

    if (g_adc_dma != NULL) {
        rx_status = HAL_DMA_Abort(g_adc_dma);
    }

    g_adc_input_running = 0U;

    if (trigger_status != HAL_OK) {
        return trigger_status;
    }
    return rx_status;
}

static void ADC121S101_RecordReadyHalf(uint8_t half_index)
{
    g_adc_latest_half = half_index;
    g_adc_latest_sequence++;
}

static void ADC121S101_RxHalfCompleteCallback(DMA_HandleTypeDef *dma)
{
    if ((dma != g_adc_dma) || (g_adc_input_running == 0U)) {
        return;
    }

    g_adc_half_complete_count++;
    ADC121S101_RecordReadyHalf(0U);
}

static void ADC121S101_RxCompleteCallback(DMA_HandleTypeDef *dma)
{
    if ((dma != g_adc_dma) || (g_adc_input_running == 0U)) {
        return;
    }

    g_adc_complete_count++;
    ADC121S101_RecordReadyHalf(1U);
}

static void ADC121S101_DmaErrorCallback(DMA_HandleTypeDef *dma)
{
    if ((dma == g_adc_dma) || (dma == g_adc_trigger_dma)) {
        g_adc_error_count++;
        g_adc_restart_requested = 1U;
    }
}
