#include "dac_output.h"

#define DAC_OUTPUT_CHANNEL DAC_CHANNEL_1

static DAC_HandleTypeDef *g_dac_output_hdac;
static TIM_HandleTypeDef *g_dac_output_dac_timer;

static uint16_t g_dac_output_buffer[DAC_OUTPUT_HALF_SIZE][DAC_OUTPUT_BLOCK_SIZE];

static volatile dac_output_half_status_t
    g_dac_output_half_status[DAC_OUTPUT_HALF_SIZE];
static volatile uint8_t g_dac_output_running;
static volatile uint32_t g_dac_output_ht_count;
static volatile uint32_t g_dac_output_tc_count;
static volatile uint32_t g_dac_output_error_count;
static volatile uint32_t g_dac_output_underrun_count;

static uint32_t DAC_Output_GetTimerClockHz(void)
{
    uint32_t timer_clock_hz;

    if ((g_dac_output_dac_timer == NULL) ||
        (g_dac_output_dac_timer->Instance != TIM6)) {
        return 0U;
    }

    timer_clock_hz = HAL_RCC_GetPCLK1Freq();
    if ((RCC->CFGR & RCC_CFGR_PPRE1) != 0U) {
        timer_clock_hz *= 2U;
    }

    return timer_clock_hz;
}

HAL_StatusTypeDef DAC_Output_Init(DAC_HandleTypeDef *hdac,
                                  TIM_HandleTypeDef *dac_timer)
{
    if ((hdac == NULL) || (hdac->DMA_Handle1 == NULL) ||
        (dac_timer == NULL)) {
        return HAL_ERROR;
    }

    if (g_dac_output_running != 0U) {
        return HAL_BUSY;
    }

    g_dac_output_hdac = hdac;
    g_dac_output_dac_timer = dac_timer;
    g_dac_output_half_status[0] = DAC_OUTPUT_HALF_FREE;
    g_dac_output_half_status[1] = DAC_OUTPUT_HALF_FREE;

    g_dac_output_ht_count = 0U;
    g_dac_output_tc_count = 0U;
    g_dac_output_error_count = 0U;
    g_dac_output_underrun_count = 0U;

    return HAL_OK;
}

HAL_StatusTypeDef DAC_Output_ConfigSampleRate(uint32_t sample_rate_hz)
{
    const uint32_t timer_clock_hz = DAC_Output_GetTimerClockHz();
    uint32_t timer_ticks;

    if ((timer_clock_hz == 0U) || (sample_rate_hz == 0U)) {
        return HAL_ERROR;
    }

    if (g_dac_output_running != 0U) {
        return HAL_BUSY;
    }

    timer_ticks = (timer_clock_hz + (sample_rate_hz / 2U)) / sample_rate_hz;
    if ((timer_ticks == 0U) || (timer_ticks > 0x10000U)) {
        return HAL_ERROR;
    }

    /* DAC 回放节拍由 TIM6 控制，启动前对齐到目标采样率。 */
    __HAL_TIM_DISABLE(g_dac_output_dac_timer);
    g_dac_output_dac_timer->Init.Prescaler = 0U;
    g_dac_output_dac_timer->Init.Period = timer_ticks - 1U;
    __HAL_TIM_SET_PRESCALER(g_dac_output_dac_timer,
                            g_dac_output_dac_timer->Init.Prescaler);
    __HAL_TIM_SET_AUTORELOAD(g_dac_output_dac_timer,
                             g_dac_output_dac_timer->Init.Period);
    __HAL_TIM_SET_COUNTER(g_dac_output_dac_timer, 0U);
    g_dac_output_dac_timer->Instance->EGR = TIM_EGR_UG;
    __HAL_TIM_CLEAR_FLAG(g_dac_output_dac_timer, TIM_FLAG_UPDATE);

    return HAL_OK;
}

uint8_t DAC_Output_AcquireBuffer(uint16_t **buffer, uint8_t *index)
{
    if ((g_dac_output_hdac == NULL) || (buffer == NULL) ||
        (index == NULL)) {
        return 0U;
    }

    for (uint8_t current = 0U; current < DAC_OUTPUT_HALF_SIZE; ++current) {
        if (g_dac_output_half_status[current] == DAC_OUTPUT_HALF_FREE) {
            g_dac_output_half_status[current] = DAC_OUTPUT_HALF_FILLING;
            *buffer = &g_dac_output_buffer[current][0];
            *index = current;
            return 1U;
        }
    }

    return 0U;
}

uint8_t DAC_Output_CommitBuffer(uint8_t index)
{
    if ((index >= DAC_OUTPUT_HALF_SIZE) ||
        (g_dac_output_half_status[index] != DAC_OUTPUT_HALF_FILLING)) {
        return 0U;
    }

    g_dac_output_half_status[index] = DAC_OUTPUT_HALF_READY;
    return 1U;
}

HAL_StatusTypeDef DAC_Output_Start(void)
{
    HAL_StatusTypeDef status;

    if ((g_dac_output_hdac == NULL) ||
        (g_dac_output_dac_timer == NULL) ||
        (g_dac_output_half_status[0] != DAC_OUTPUT_HALF_READY) ||
        (g_dac_output_half_status[1] != DAC_OUTPUT_HALF_READY)) {
        return HAL_ERROR;
    }

    if (g_dac_output_running != 0U) {
        return HAL_BUSY;
    }

    /* 保证每次启动都从确定的 TIM6 计数状态开始。 */
    __HAL_TIM_SET_COUNTER(g_dac_output_dac_timer, 0U);
    __HAL_TIM_CLEAR_FLAG(g_dac_output_dac_timer, TIM_FLAG_UPDATE);
    __HAL_DAC_CLEAR_FLAG(g_dac_output_hdac, DAC_FLAG_DMAUDR1);

    status = HAL_DAC_Start_DMA(g_dac_output_hdac,
                               DAC_OUTPUT_CHANNEL,
                               (const uint32_t *)&g_dac_output_buffer[0][0],
                               DAC_OUTPUT_SAMPLE_SIZE,
                               DAC_ALIGN_12B_R);
    if (status != HAL_OK) {
        return status;
    }

    status = HAL_TIM_Base_Start(g_dac_output_dac_timer);
    if (status != HAL_OK) {
        (void)HAL_DAC_Stop_DMA(g_dac_output_hdac, DAC_OUTPUT_CHANNEL);
        return status;
    }

    g_dac_output_half_status[0] = DAC_OUTPUT_HALF_PLAYING;
    g_dac_output_running = 1U;
    return HAL_OK;
}

HAL_StatusTypeDef DAC_Output_Stop(void)
{
    HAL_StatusTypeDef timer_status;
    HAL_StatusTypeDef dac_status;

    if ((g_dac_output_hdac == NULL) ||
        (g_dac_output_dac_timer == NULL)) {
        return HAL_ERROR;
    }

    if (g_dac_output_running == 0U) {
        return HAL_OK;
    }

    timer_status = HAL_TIM_Base_Stop(g_dac_output_dac_timer);
    dac_status = HAL_DAC_Stop_DMA(g_dac_output_hdac, DAC_OUTPUT_CHANNEL);
    g_dac_output_running = 0U;

    if (timer_status != HAL_OK) {
        return timer_status;
    }

    return dac_status;
}

void HAL_DAC_ConvHalfCpltCallbackCh1(DAC_HandleTypeDef *hdac)
{
    if ((hdac != g_dac_output_hdac) || (g_dac_output_running == 0U)) {
        return;
    }

    g_dac_output_ht_count++;

    if (g_dac_output_half_status[0U] != DAC_OUTPUT_HALF_PLAYING) {
        g_dac_output_underrun_count++;
    }
    g_dac_output_half_status[0U] = DAC_OUTPUT_HALF_FREE;

    if (g_dac_output_half_status[1U] != DAC_OUTPUT_HALF_READY) {
        g_dac_output_underrun_count++;
    }
    g_dac_output_half_status[1U] = DAC_OUTPUT_HALF_PLAYING;
}

void HAL_DAC_ConvCpltCallbackCh1(DAC_HandleTypeDef *hdac)
{
    if ((hdac != g_dac_output_hdac) || (g_dac_output_running == 0U)) {
        return;
    }

    g_dac_output_tc_count++;

    if (g_dac_output_half_status[1U] != DAC_OUTPUT_HALF_PLAYING) {
        g_dac_output_underrun_count++;
    }
    g_dac_output_half_status[1U] = DAC_OUTPUT_HALF_FREE;

    if (g_dac_output_half_status[0U] != DAC_OUTPUT_HALF_READY) {
        g_dac_output_underrun_count++;
    }
    g_dac_output_half_status[0U] = DAC_OUTPUT_HALF_PLAYING;
}

void HAL_DAC_ErrorCallbackCh1(DAC_HandleTypeDef *hdac)
{
    if (hdac == g_dac_output_hdac) {
        g_dac_output_error_count++;
    }
}

void HAL_DAC_DMAUnderrunCallbackCh1(DAC_HandleTypeDef *hdac)
{
    if (hdac == g_dac_output_hdac) {
        g_dac_output_underrun_count++;
    }
}

uint32_t DAC_Output_GetHalfCompleteCount(void)
{
    return g_dac_output_ht_count;
}

uint32_t DAC_Output_GetCompleteCount(void)
{
    return g_dac_output_tc_count;
}

uint32_t DAC_Output_GetErrorCount(void)
{
    return g_dac_output_error_count;
}

uint32_t DAC_Output_GetUnderrunCount(void)
{
    return g_dac_output_underrun_count;
}
