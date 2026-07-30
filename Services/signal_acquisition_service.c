#include "signal_acquisition_service.h"

#include <stddef.h>

#include "adc_internal.h"
#include "signal_measurement_service.h"
#include "waveform_analyzer_service.h"

#define SIGNAL_ACQUISITION_ANALYSIS_INTERVAL_MS 20U
#define SIGNAL_ACQUISITION_FFT_SAMPLE_RATE_HZ 2048000U

#if SIGNAL_ACQUISITION_ENABLE_DAC_LOOPBACK
#include "adc_dac_loopback_service.h"
#include "dac_output.h"
#endif

static signal_acquisition_status_t g_signal_acquisition_status;
static uint16_t g_signal_acquisition_block[ADC_INTERNAL_BLOCK_SIZE];
static uint32_t g_signal_acquisition_next_analysis_ms;

_Static_assert(ADC_INTERNAL_BLOCK_SIZE >= WAVEFORM_ANALYZER_FFT_SIZE,
               "ADC 采样块长度必须覆盖一次 FFT");

static uint32_t signal_acquisition_get_timer_clock_hz(
    const TIM_HandleTypeDef *timer)
{
    uint32_t timer_clock_hz;

    if ((timer == NULL) || (timer->Instance != TIM7)) {
        return 0U;
    }

    timer_clock_hz = HAL_RCC_GetPCLK1Freq();
    if ((RCC->CFGR & RCC_CFGR_PPRE1) != 0U) {
        timer_clock_hz *= 2U;
    }

    return timer_clock_hz;
}

static uint32_t signal_acquisition_get_timer_update_rate_hz(
    const TIM_HandleTypeDef *timer)
{
    const uint32_t timer_clock_hz =
        signal_acquisition_get_timer_clock_hz(timer);
    uint64_t divisor;

    if ((timer_clock_hz == 0U) || (timer == NULL)) {
        return 0U;
    }

    divisor = ((uint64_t)timer->Init.Prescaler + 1ULL) *
              ((uint64_t)timer->Init.Period + 1ULL);
    if (divisor == 0U) {
        return 0U;
    }

    return (uint32_t)(((uint64_t)timer_clock_hz + (divisor / 2ULL)) /
                      divisor);
}

static uint8_t signal_acquisition_time_reached(uint32_t deadline_ms)
{
    return ((int32_t)(HAL_GetTick() - deadline_ms) >= 0) ? 1U : 0U;
}

static void signal_acquisition_update_status(void)
{
    const waveform_analyzer_result_t *fft_result =
        Waveform_Analyzer_GetResult();
    const signal_measurement_result_t *measurement_result =
        Signal_Measurement_Service_GetResult();

    g_signal_acquisition_status.adc_running = ADC_Internal_IsRunning();
    g_signal_acquisition_status.adc_half_complete_count =
        ADC_Internal_GetHalfCompleteCount();
    g_signal_acquisition_status.adc_complete_count =
        ADC_Internal_GetCompleteCount();
    g_signal_acquisition_status.adc_error_count =
        ADC_Internal_GetErrorCount();
    g_signal_acquisition_status.adc_overrun_count =
        ADC_Internal_GetOverrunCount();

    if (measurement_result != NULL) {
        g_signal_acquisition_status.measurement = *measurement_result;
    }

#if SIGNAL_ACQUISITION_ENABLE_DAC_LOOPBACK
    g_signal_acquisition_status.dac_half_complete_count =
        DAC_Output_GetHalfCompleteCount();
    g_signal_acquisition_status.dac_complete_count =
        DAC_Output_GetCompleteCount();
    g_signal_acquisition_status.dac_error_count =
        DAC_Output_GetErrorCount();
    g_signal_acquisition_status.dac_underrun_count =
        DAC_Output_GetUnderrunCount();
    g_signal_acquisition_status.dac_loopback_running =
        ADC_DAC_Loopback_IsRunning();
    g_signal_acquisition_status.dac_loopback_dropped_block_count =
        ADC_DAC_Loopback_GetDroppedBlockCount();
    g_signal_acquisition_status.dac_loopback_error_count =
        ADC_DAC_Loopback_GetErrorCount();
#endif

    if (fft_result == NULL) {
        return;
    }

    g_signal_acquisition_status.fft_ready = fft_result->result_ready;
    g_signal_acquisition_status.fft_analysis_count =
        fft_result->analysis_count;
    g_signal_acquisition_status.fft_sample_rate_hz =
        fft_result->sample_rate_hz;
    g_signal_acquisition_status.fft_bin_resolution_hz =
        fft_result->bin_resolution_hz;
    g_signal_acquisition_status.detected_frequency_hz =
        fft_result->fundamental_frequency_hz;
    g_signal_acquisition_status.waveform_peak_to_peak_code =
        fft_result->peak_to_peak_code;
    g_signal_acquisition_status.waveform_average_code =
        fft_result->average_code;
    g_signal_acquisition_status.waveform_rms_code = fft_result->rms_code;
    g_signal_acquisition_status.adc_last_min = fft_result->min_code;
    g_signal_acquisition_status.adc_last_max = fft_result->max_code;
    g_signal_acquisition_status.adc_last_average =
        fft_result->average_code;
    if ((measurement_result != NULL) &&
        (measurement_result->result_ready != 0U)) {
        g_signal_acquisition_status.detected_frequency_hz =
            measurement_result->fundamental_frequency_hz;
    }
}

static HAL_StatusTypeDef signal_acquisition_set_error(
    signal_acquisition_error_t error,
    HAL_StatusTypeDef hal_status)
{
    g_signal_acquisition_status.error = error;
    g_signal_acquisition_status.last_hal_status = hal_status;
    signal_acquisition_update_status();
    return hal_status;
}

HAL_StatusTypeDef Signal_Acquisition_Service_Init(
    const signal_acquisition_config_t *config)
{
    HAL_StatusTypeDef status;
    uint32_t adc_sample_rate_hz;

    g_signal_acquisition_status = (signal_acquisition_status_t){0};
    if ((config == NULL) || (config->adc_timer == NULL)) {
        return signal_acquisition_set_error(
            SIGNAL_ACQUISITION_ERROR_INVALID_CONFIG,
            HAL_ERROR);
    }

#if SIGNAL_ACQUISITION_ENABLE_DAC_LOOPBACK
    if ((config->dac == NULL) || (config->dac_timer == NULL)) {
        return signal_acquisition_set_error(
            SIGNAL_ACQUISITION_ERROR_INVALID_CONFIG,
            HAL_ERROR);
    }
#else
    (void)config->dac;
    (void)config->dac_timer;
#endif

    adc_sample_rate_hz =
        signal_acquisition_get_timer_update_rate_hz(config->adc_timer);
    if (adc_sample_rate_hz == 0U) {
        return signal_acquisition_set_error(
            SIGNAL_ACQUISITION_ERROR_INVALID_CONFIG,
            HAL_ERROR);
    }

    status = ADC_Internal_Init(config->adc_timer);
    if (status != HAL_OK) {
        return signal_acquisition_set_error(
            SIGNAL_ACQUISITION_ERROR_ADC_INIT,
            status);
    }

    status = Waveform_Analyzer_Init(SIGNAL_ACQUISITION_FFT_SAMPLE_RATE_HZ);
    if (status != HAL_OK) {
        return signal_acquisition_set_error(
            SIGNAL_ACQUISITION_ERROR_ANALYZER_INIT,
            status);
    }

    status = Signal_Measurement_Service_Init(
        config->measurement_calibration);
    if (status != HAL_OK) {
        return signal_acquisition_set_error(
            SIGNAL_ACQUISITION_ERROR_MEASUREMENT_INIT,
            status);
    }

#if SIGNAL_ACQUISITION_ENABLE_DAC_LOOPBACK
    status = ADC_DAC_Loopback_Init(config->dac,
                                   config->dac_timer,
                                   adc_sample_rate_hz);
    if (status != HAL_OK) {
        return signal_acquisition_set_error(
            SIGNAL_ACQUISITION_ERROR_LOOPBACK_INIT,
            status);
    }
#endif

    status = ADC_Internal_Start();
    if (status != HAL_OK) {
        return signal_acquisition_set_error(
            SIGNAL_ACQUISITION_ERROR_ADC_START,
            status);
    }

    g_signal_acquisition_status.initialized = 1U;
    g_signal_acquisition_status.error = SIGNAL_ACQUISITION_ERROR_NONE;
    g_signal_acquisition_status.last_hal_status = HAL_OK;
    g_signal_acquisition_next_analysis_ms = HAL_GetTick();
    signal_acquisition_update_status();
    return HAL_OK;
}

HAL_StatusTypeDef Signal_Acquisition_Service_Process(void)
{
    HAL_StatusTypeDef status;

    if (g_signal_acquisition_status.initialized == 0U) {
        return signal_acquisition_set_error(
            SIGNAL_ACQUISITION_ERROR_INVALID_CONFIG,
            HAL_ERROR);
    }

    status = ADC_Internal_Process();
    if (status != HAL_OK) {
        return signal_acquisition_set_error(
            SIGNAL_ACQUISITION_ERROR_ADC_RUNTIME,
            status);
    }

    if ((signal_acquisition_time_reached(
             g_signal_acquisition_next_analysis_ms) != 0U) &&
        (ADC_Internal_CopyLatestBlock(g_signal_acquisition_block,
                                      ADC_INTERNAL_BLOCK_SIZE) != 0U)) {
        g_signal_acquisition_next_analysis_ms =
            HAL_GetTick() + SIGNAL_ACQUISITION_ANALYSIS_INTERVAL_MS;
#if SIGNAL_ACQUISITION_ENABLE_DAC_LOOPBACK
        /* 先交付实时回环数据，再执行耗时的 FFT。 */
        (void)ADC_DAC_Loopback_PushBlock(g_signal_acquisition_block,
                                         ADC_INTERNAL_BLOCK_SIZE);
#endif

        status = Waveform_Analyzer_ProcessBlock(g_signal_acquisition_block,
                                                ADC_INTERNAL_BLOCK_SIZE);
        if (status != HAL_OK) {
            return signal_acquisition_set_error(
                SIGNAL_ACQUISITION_ERROR_ANALYZER_RUNTIME,
                status);
        }

        status = Signal_Measurement_Service_Process(
            Waveform_Analyzer_GetResult());
        if (status != HAL_OK) {
            return signal_acquisition_set_error(
                SIGNAL_ACQUISITION_ERROR_MEASUREMENT_RUNTIME,
                status);
        }

        g_signal_acquisition_status.adc_block_count++;
    }

    g_signal_acquisition_status.error = SIGNAL_ACQUISITION_ERROR_NONE;
    g_signal_acquisition_status.last_hal_status = HAL_OK;
    signal_acquisition_update_status();
    return HAL_OK;
}

uint8_t Signal_Acquisition_Service_GetLatestBlock(
    const uint16_t **samples,
    uint32_t *length,
    uint32_t *sequence)
{
    if ((samples == NULL) || (length == NULL) || (sequence == NULL) ||
        (g_signal_acquisition_status.initialized == 0U) ||
        (g_signal_acquisition_status.adc_block_count == 0U)) {
        return 0U;
    }

    *samples = g_signal_acquisition_block;
    *length = ADC_INTERNAL_BLOCK_SIZE;
    *sequence = g_signal_acquisition_status.adc_block_count;
    return 1U;
}

const signal_acquisition_status_t *Signal_Acquisition_Service_GetStatus(void)
{
    return &g_signal_acquisition_status;
}
