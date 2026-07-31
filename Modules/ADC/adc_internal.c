#include "adc_internal.h"

#include <stddef.h>
#include <string.h>

#define ADC_INTERNAL_12BIT_TOTAL_CYCLES 15U
#define ADC_INTERNAL_ADC_CLOCK_HZ 60000000U

_Static_assert(ADC_INTERNAL_ADC_CLOCK_HZ ==
                   (ADC_INTERNAL_SAMPLE_RATE_HZ *
                    ADC_INTERNAL_12BIT_TOTAL_CYCLES),
               "ADC 连续采样率必须与 ADC 内核时钟和转换周期一致");

#if SIGNAL_ADC_USE_CUBEMX_GENERATED
#include "adc.h"
#define ADC_INTERNAL_ADC_HANDLE hadc2
#else
static ADC_HandleTypeDef g_adc2;
static DMA_HandleTypeDef g_adc2_dma;
#define ADC_INTERNAL_ADC_HANDLE g_adc2
#endif

#if !SIGNAL_ADC_USE_CUBEMX_GENERATED
static HAL_StatusTypeDef g_adc_msp_status;
#endif

static uint16_t
    g_adc_input_buffer[ADC_INTERNAL_HALF_COUNT][ADC_INTERNAL_BLOCK_SIZE]
        __attribute__((aligned(4)));
static volatile uint8_t g_adc_running;
static volatile uint8_t g_adc_restart_requested;
static volatile uint8_t g_adc_latest_half;
static volatile uint32_t g_adc_latest_sequence;
static volatile uint32_t g_adc_copied_sequence;
static volatile uint32_t g_adc_half_complete_count;
static volatile uint32_t g_adc_complete_count;
static volatile uint32_t g_adc_error_count;
static volatile uint32_t g_adc_overrun_count;

static uint8_t adc_internal_timing_valid(void)
{
    uint32_t adc_clock_hz;

    adc_clock_hz = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_ADC12);
    return (adc_clock_hz == ADC_INTERNAL_ADC_CLOCK_HZ) ? 1U : 0U;
}

static void adc_internal_clear_state(void)
{
    g_adc_restart_requested = 0U;
    g_adc_latest_half = ADC_INTERNAL_HALF_COUNT;
    g_adc_latest_sequence = 0U;
    g_adc_copied_sequence = 0U;
}

static void adc_internal_clear_counters(void)
{
    g_adc_half_complete_count = 0U;
    g_adc_complete_count = 0U;
    g_adc_error_count = 0U;
    g_adc_overrun_count = 0U;
}

static void adc_internal_record_ready_half(uint8_t half_index)
{
    g_adc_latest_half = half_index;
    g_adc_latest_sequence++;
}

static HAL_StatusTypeDef adc_internal_configure_peripheral(void)
{
#if SIGNAL_ADC_USE_CUBEMX_GENERATED
    if (ADC_INTERNAL_ADC_HANDLE.State == HAL_ADC_STATE_RESET) {
        MX_ADC2_Init();
    }

    return ((ADC_INTERNAL_ADC_HANDLE.Instance == ADC2) &&
            (ADC_INTERNAL_ADC_HANDLE.DMA_Handle != NULL)) ?
               HAL_OK :
               HAL_ERROR;
#else
    ADC_ChannelConfTypeDef channel_config = {0};
    ADC_MultiModeTypeDef multimode_config = {0};

    ADC_INTERNAL_ADC_HANDLE.Instance = ADC2;
    ADC_INTERNAL_ADC_HANDLE.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV1;
    ADC_INTERNAL_ADC_HANDLE.Init.Resolution = ADC_RESOLUTION_12B;
    ADC_INTERNAL_ADC_HANDLE.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    ADC_INTERNAL_ADC_HANDLE.Init.GainCompensation = 0U;
    ADC_INTERNAL_ADC_HANDLE.Init.ScanConvMode = ADC_SCAN_DISABLE;
    ADC_INTERNAL_ADC_HANDLE.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    ADC_INTERNAL_ADC_HANDLE.Init.LowPowerAutoWait = DISABLE;
    ADC_INTERNAL_ADC_HANDLE.Init.ContinuousConvMode = ENABLE;
    ADC_INTERNAL_ADC_HANDLE.Init.NbrOfConversion = 1U;
    ADC_INTERNAL_ADC_HANDLE.Init.DiscontinuousConvMode = DISABLE;
    ADC_INTERNAL_ADC_HANDLE.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    ADC_INTERNAL_ADC_HANDLE.Init.ExternalTrigConvEdge =
        ADC_EXTERNALTRIGCONVEDGE_NONE;
    ADC_INTERNAL_ADC_HANDLE.Init.DMAContinuousRequests = ENABLE;
    ADC_INTERNAL_ADC_HANDLE.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
    ADC_INTERNAL_ADC_HANDLE.Init.OversamplingMode = DISABLE;

    g_adc_msp_status = HAL_OK;
    if ((HAL_ADC_Init(&ADC_INTERNAL_ADC_HANDLE) != HAL_OK) ||
        (g_adc_msp_status != HAL_OK)) {
        return HAL_ERROR;
    }

    (void)multimode_config;

    channel_config.Channel = ADC_CHANNEL_4;
    channel_config.Rank = ADC_REGULAR_RANK_1;
    channel_config.SamplingTime = ADC_SAMPLETIME_2CYCLES_5;
    channel_config.SingleDiff = ADC_SINGLE_ENDED;
    channel_config.OffsetNumber = ADC_OFFSET_NONE;
    channel_config.Offset = 0U;

    return HAL_ADC_ConfigChannel(&ADC_INTERNAL_ADC_HANDLE,
                                 &channel_config);
#endif
}

static HAL_StatusTypeDef adc_internal_stop(void)
{
    HAL_StatusTypeDef adc_status;

    adc_status = HAL_ADC_Stop_DMA(&ADC_INTERNAL_ADC_HANDLE);
    g_adc_running = 0U;
    return adc_status;
}

HAL_StatusTypeDef ADC_Internal_Init(void)
{
    HAL_StatusTypeDef status;

    if (g_adc_running != 0U) {
        return HAL_BUSY;
    }

    adc_internal_clear_state();
    adc_internal_clear_counters();

    status = adc_internal_configure_peripheral();
    if (status != HAL_OK) {
        g_adc_error_count++;
        return status;
    }
    if (adc_internal_timing_valid() == 0U) {
        g_adc_error_count++;
        return HAL_ERROR;
    }

    status = HAL_ADCEx_Calibration_Start(&ADC_INTERNAL_ADC_HANDLE,
                                         ADC_SINGLE_ENDED);
    if (status != HAL_OK) {
        g_adc_error_count++;
    }
    return status;
}

HAL_StatusTypeDef ADC_Internal_Start(void)
{
    HAL_StatusTypeDef status;

    if ((ADC_INTERNAL_ADC_HANDLE.Instance != ADC2) ||
        (ADC_INTERNAL_ADC_HANDLE.DMA_Handle == NULL)) {
        return HAL_ERROR;
    }
    if (g_adc_running != 0U) {
        return HAL_BUSY;
    }

    adc_internal_clear_state();
    status = HAL_ADC_Start_DMA(
        &ADC_INTERNAL_ADC_HANDLE,
        (uint32_t *)&g_adc_input_buffer[0][0],
        ADC_INTERNAL_SAMPLE_COUNT);
    if (status != HAL_OK) {
        g_adc_error_count++;
        return status;
    }

    g_adc_running = 1U;
    return status;
}

HAL_StatusTypeDef ADC_Internal_Process(void)
{
    if (g_adc_restart_requested == 0U) {
        return HAL_OK;
    }

    g_adc_restart_requested = 0U;
    (void)adc_internal_stop();
    return ADC_Internal_Start();
}

uint8_t ADC_Internal_CopyLatestBlock(uint16_t *destination,
                                     uint32_t capacity)
{
    if ((destination == NULL) ||
        (capacity < ADC_INTERNAL_BLOCK_SIZE)) {
        return 0U;
    }

    for (uint8_t attempt = 0U; attempt < 2U; ++attempt) {
        uint8_t half_index;
        uint32_t sequence;
        uint32_t primask = __get_PRIMASK();

        __disable_irq();
        sequence = g_adc_latest_sequence;
        half_index = g_adc_latest_half;
        __set_PRIMASK(primask);

        if ((sequence == 0U) ||
            (sequence == g_adc_copied_sequence) ||
            (half_index >= ADC_INTERNAL_HALF_COUNT)) {
            return 0U;
        }

        /* 使用库优化块复制，确保 16 KB 数据能在约 2.07 ms 安全窗口内完成。 */
        (void)memcpy(destination,
                     g_adc_input_buffer[half_index],
                     sizeof(g_adc_input_buffer[half_index]));

        primask = __get_PRIMASK();
        __disable_irq();
        if ((sequence == g_adc_latest_sequence) &&
            (half_index == g_adc_latest_half)) {
            g_adc_copied_sequence = sequence;
            __set_PRIMASK(primask);
            return 1U;
        }
        __set_PRIMASK(primask);

        /*
         * 首次复制若恰好跨越 DMA 半区边界，立即复制刚完成的新半区。
         * 新半区拥有完整安全窗口，不把一次可恢复的相位碰撞计为 overrun。
         */
    }

    g_adc_overrun_count++;
    return 0U;
}

uint8_t ADC_Internal_IsRunning(void)
{
    return g_adc_running;
}

uint32_t ADC_Internal_GetHalfCompleteCount(void)
{
    return g_adc_half_complete_count;
}

uint32_t ADC_Internal_GetCompleteCount(void)
{
    return g_adc_complete_count;
}

uint32_t ADC_Internal_GetErrorCount(void)
{
    return g_adc_error_count;
}

uint32_t ADC_Internal_GetOverrunCount(void)
{
    return g_adc_overrun_count;
}

uint32_t ADC_Internal_GetSampleRateHz(void)
{
    return ADC_INTERNAL_SAMPLE_RATE_HZ;
}

#if !SIGNAL_ADC_USE_CUBEMX_GENERATED
void HAL_ADC_MspInit(ADC_HandleTypeDef *adc_handle)
{
    GPIO_InitTypeDef gpio_init = {0};
    RCC_PeriphCLKInitTypeDef peripheral_clock = {0};

    if ((adc_handle == NULL) || (adc_handle->Instance != ADC2)) {
        return;
    }

    peripheral_clock.PeriphClockSelection = RCC_PERIPHCLK_ADC12;
    peripheral_clock.Adc12ClockSelection = RCC_ADC12CLKSOURCE_PLL;
    if (HAL_RCCEx_PeriphCLKConfig(&peripheral_clock) != HAL_OK) {
        g_adc_msp_status = HAL_ERROR;
        return;
    }

    __HAL_RCC_ADC12_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_DMAMUX1_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();

    gpio_init.Pin = ADC_INTERNAL_INPUT_PIN;
    gpio_init.Mode = GPIO_MODE_ANALOG;
    gpio_init.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(ADC_INTERNAL_INPUT_GPIO_PORT, &gpio_init);

    g_adc2_dma.Instance = DMA2_Channel2;
    g_adc2_dma.Init.Request = DMA_REQUEST_ADC2;
    g_adc2_dma.Init.Direction = DMA_PERIPH_TO_MEMORY;
    g_adc2_dma.Init.PeriphInc = DMA_PINC_DISABLE;
    g_adc2_dma.Init.MemInc = DMA_MINC_ENABLE;
    g_adc2_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    g_adc2_dma.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    g_adc2_dma.Init.Mode = DMA_CIRCULAR;
    g_adc2_dma.Init.Priority = DMA_PRIORITY_VERY_HIGH;
    if (HAL_DMA_Init(&g_adc2_dma) != HAL_OK) {
        g_adc_msp_status = HAL_ERROR;
        return;
    }

    __HAL_LINKDMA(adc_handle, DMA_Handle, g_adc2_dma);
    HAL_NVIC_SetPriority(DMA2_Channel2_IRQn, 0U, 0U);
    HAL_NVIC_EnableIRQ(DMA2_Channel2_IRQn);
}

void HAL_ADC_MspDeInit(ADC_HandleTypeDef *adc_handle)
{
    if ((adc_handle == NULL) || (adc_handle->Instance != ADC2)) {
        return;
    }

    __HAL_RCC_ADC12_CLK_DISABLE();
    HAL_GPIO_DeInit(ADC_INTERNAL_INPUT_GPIO_PORT,
                    ADC_INTERNAL_INPUT_PIN);
    HAL_DMA_DeInit(adc_handle->DMA_Handle);
    HAL_NVIC_DisableIRQ(DMA2_Channel2_IRQn);
}
#endif

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *adc_handle)
{
    if ((adc_handle != &ADC_INTERNAL_ADC_HANDLE) ||
        (g_adc_running == 0U)) {
        return;
    }

    g_adc_half_complete_count++;
    adc_internal_record_ready_half(0U);
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *adc_handle)
{
    if ((adc_handle != &ADC_INTERNAL_ADC_HANDLE) ||
        (g_adc_running == 0U)) {
        return;
    }

    g_adc_complete_count++;
    adc_internal_record_ready_half(1U);
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *adc_handle)
{
    if (adc_handle != &ADC_INTERNAL_ADC_HANDLE) {
        return;
    }

    g_adc_error_count++;
    g_adc_restart_requested = 1U;
}

#if !SIGNAL_ADC_USE_CUBEMX_GENERATED
void DMA2_Channel2_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&g_adc2_dma);
}
#endif
