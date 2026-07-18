#include "signal_service.h"

#include <limits.h>
#include <stddef.h>

#include "adc121s101.h"
#include "dac_output.h"
#include "fft_service.h"
#include "signal_process.h"

#define SIGNAL_SERVICE_TIMER_MAX_DIVIDER     65536U
#define SIGNAL_SERVICE_DEFAULT_DDS_FREQ_HZ   100U

#if (ADC121S101_BLOCK_SIZE != FFT_SERVICE_SAMPLE_COUNT)
#error "ADC block size must match FFT sample count"
#endif

#if defined(__GNUC__)
#define SIGNAL_SERVICE_CCMRAM \
    __attribute__((section(".ccmram"), aligned(4)))
#else
#define SIGNAL_SERVICE_CCMRAM
#endif

typedef struct {
    TIM_HandleTypeDef *adc_sample_timer;
    dds_t dds;
    FftService_Result fft_result;
    int32_t adc_snapshot[SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT];
    int32_t dds_snapshot[SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT];
    int32_t spectrum_snapshot[SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT];
    uint32_t sample_rate_hz;
    uint32_t dds_sample_rate_hz;
    uint32_t adc_snapshot_generation;
    uint32_t dds_snapshot_generation;
    uint32_t spectrum_snapshot_generation;
    uint8_t adc_snapshot_valid;
    uint8_t dds_snapshot_valid;
    uint8_t spectrum_snapshot_valid;
    uint8_t initialized;
} SignalService_Context;

static SignalService_Context g_signal_service;

/* Driver 快速复制到稳定块后，FFT 才读取；该工作块不参与 DMA。 */
static uint16_t g_adc_work_block[ADC121S101_BLOCK_SIZE]
    SIGNAL_SERVICE_CCMRAM;

static uint8_t signal_service_rate_is_supported(uint32_t sample_rate_hz)
{
    return (sample_rate_hz == SIGNAL_SERVICE_SAMPLE_RATE_20K_HZ) ||
           (sample_rate_hz == SIGNAL_SERVICE_SAMPLE_RATE_50K_HZ) ||
           (sample_rate_hz == SIGNAL_SERVICE_SAMPLE_RATE_100K_HZ) ||
           (sample_rate_hz == SIGNAL_SERVICE_SAMPLE_RATE_200K_HZ);
}

static uint32_t signal_service_get_apb1_timer_clock_hz(void)
{
    RCC_ClkInitTypeDef clock_config = {0};
    uint32_t flash_latency = 0U;
    uint32_t timer_clock_hz = HAL_RCC_GetPCLK1Freq();

    HAL_RCC_GetClockConfig(&clock_config, &flash_latency);
    if (clock_config.APB1CLKDivider != RCC_HCLK_DIV1) {
        timer_clock_hz *= 2U;
    }
    return timer_clock_hz;
}

static uint32_t signal_service_get_timer_rate_hz(
    const TIM_HandleTypeDef *sample_timer)
{
    uint64_t divider;
    uint32_t timer_clock_hz;

    if (sample_timer == NULL) {
        return 0U;
    }

    timer_clock_hz = signal_service_get_apb1_timer_clock_hz();
    divider = ((uint64_t)sample_timer->Instance->PSC + 1U) *
              ((uint64_t)sample_timer->Instance->ARR + 1U);
    if (divider == 0U) {
        return 0U;
    }
    return (uint32_t)((uint64_t)timer_clock_hz / divider);
}

static HAL_StatusTypeDef signal_service_calculate_timer_dividers(
    uint32_t requested_hz,
    uint32_t *prescaler,
    uint32_t *period,
    uint32_t *actual_hz)
{
    uint32_t timer_clock_hz;
    uint64_t total_divider;
    uint64_t prescaler_divider;
    uint64_t period_divider;
    uint64_t denominator;

    if ((requested_hz == 0U) || (prescaler == NULL) ||
        (period == NULL) || (actual_hz == NULL)) {
        return HAL_ERROR;
    }

    timer_clock_hz = signal_service_get_apb1_timer_clock_hz();
    if (timer_clock_hz == 0U) {
        return HAL_ERROR;
    }

    total_divider = ((uint64_t)timer_clock_hz +
                     ((uint64_t)requested_hz / 2U)) /
                    (uint64_t)requested_hz;
    if (total_divider == 0U) {
        total_divider = 1U;
    }

    prescaler_divider =
        (total_divider + SIGNAL_SERVICE_TIMER_MAX_DIVIDER - 1U) /
        SIGNAL_SERVICE_TIMER_MAX_DIVIDER;
    if ((prescaler_divider == 0U) ||
        (prescaler_divider > SIGNAL_SERVICE_TIMER_MAX_DIVIDER)) {
        return HAL_ERROR;
    }

    period_divider = (total_divider + (prescaler_divider / 2U)) /
                     prescaler_divider;
    if ((period_divider == 0U) ||
        (period_divider > SIGNAL_SERVICE_TIMER_MAX_DIVIDER)) {
        return HAL_ERROR;
    }

    denominator = prescaler_divider * period_divider;
    *prescaler = (uint32_t)(prescaler_divider - 1U);
    *period = (uint32_t)(period_divider - 1U);
    *actual_hz = (uint32_t)((uint64_t)timer_clock_hz / denominator);
    return HAL_OK;
}

static void signal_service_write_timer_dividers(
    TIM_HandleTypeDef *sample_timer,
    uint32_t prescaler,
    uint32_t period)
{
    __HAL_TIM_SET_PRESCALER(sample_timer, prescaler);
    __HAL_TIM_SET_AUTORELOAD(sample_timer, period);
    __HAL_TIM_SET_COUNTER(sample_timer, 0U);
    (void)HAL_TIM_GenerateEvent(sample_timer, TIM_EVENTSOURCE_UPDATE);
    __HAL_TIM_CLEAR_FLAG(sample_timer, TIM_FLAG_UPDATE);
    sample_timer->Init.Prescaler = prescaler;
    sample_timer->Init.Period = period;
}

static HAL_StatusTypeDef signal_service_apply_timer_dividers(
    TIM_HandleTypeDef *sample_timer,
    uint32_t prescaler,
    uint32_t period)
{
    ADC121S101_Status adc_status;
    uint32_t old_prescaler;
    uint32_t old_period;
    uint8_t was_running;

    ADC121S101_GetStatus(&adc_status);
    was_running = adc_status.running;
    old_prescaler = sample_timer->Instance->PSC;
    old_period = sample_timer->Instance->ARR;

    if ((was_running != 0U) && (ADC121S101_Stop() != HAL_OK)) {
        return HAL_ERROR;
    }

    signal_service_write_timer_dividers(sample_timer, prescaler, period);
    ADC121S101_DiscardPendingBlock();

    if ((was_running != 0U) && (ADC121S101_Start() != HAL_OK)) {
        signal_service_write_timer_dividers(sample_timer,
                                            old_prescaler,
                                            old_period);
        ADC121S101_DiscardPendingBlock();
        (void)ADC121S101_Start();
        return HAL_ERROR;
    }
    return HAL_OK;
}

static uint32_t signal_service_next_generation(uint32_t generation)
{
    generation++;
    return (generation != 0U) ? generation : 1U;
}

static uint32_t signal_service_float_to_u32(float value, float scale)
{
    float scaled;

    if (value <= 0.0f) {
        return 0U;
    }
    scaled = value * scale;
    if (scaled >= (float)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)(scaled + 0.5f);
}

static uint16_t signal_service_float_to_u16(float value)
{
    if (value <= 0.0f) {
        return 0U;
    }
    if (value >= (float)UINT16_MAX) {
        return UINT16_MAX;
    }
    return (uint16_t)(value + 0.5f);
}

static uint8_t signal_service_process_fft(void)
{
    const float *magnitudes;
    uint16_t bin_count;

    if (FftService_Process(g_adc_work_block,
                           ADC121S101_BLOCK_SIZE,
                           g_signal_service.sample_rate_hz) !=
        FFT_SERVICE_STATUS_OK) {
        return 0U;
    }
    if ((FftService_GetResult(&g_signal_service.fft_result) !=
         FFT_SERVICE_STATUS_OK) ||
        (FftService_GetSpectrum(&magnitudes, &bin_count) !=
         FFT_SERVICE_STATUS_OK) ||
        (SignalProcess_BuildSpectrumSnapshot(
             magnitudes,
             bin_count,
             g_signal_service.spectrum_snapshot,
             SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT) == 0U)) {
        return 0U;
    }

    g_signal_service.spectrum_snapshot_generation =
        signal_service_next_generation(
            g_signal_service.spectrum_snapshot_generation);
    g_signal_service.spectrum_snapshot_valid = 1U;
    return 1U;
}

HAL_StatusTypeDef SignalService_Init(TIM_HandleTypeDef *adc_sample_timer,
                                     TIM_HandleTypeDef *dds_sample_timer)
{
    dds_config_t dds_config;
    uint32_t sample_rate_hz;
    uint32_t dds_sample_rate_hz;
    uint32_t point_index;

    if ((adc_sample_timer == NULL) || (dds_sample_timer == NULL)) {
        return HAL_ERROR;
    }

    sample_rate_hz = signal_service_get_timer_rate_hz(adc_sample_timer);
    dds_sample_rate_hz = signal_service_get_timer_rate_hz(dds_sample_timer);
    if ((sample_rate_hz == 0U) || (dds_sample_rate_hz == 0U) ||
        (FftService_Init() != FFT_SERVICE_STATUS_OK)) {
        return HAL_ERROR;
    }

    g_signal_service.adc_sample_timer = adc_sample_timer;
    g_signal_service.sample_rate_hz = sample_rate_hz;
    g_signal_service.dds_sample_rate_hz = dds_sample_rate_hz;
    g_signal_service.adc_snapshot_generation = 0U;
    g_signal_service.dds_snapshot_generation = 0U;
    g_signal_service.spectrum_snapshot_generation = 0U;
    g_signal_service.adc_snapshot_valid = 0U;
    g_signal_service.dds_snapshot_valid = 0U;
    g_signal_service.spectrum_snapshot_valid = 0U;
    g_signal_service.initialized = 0U;
    g_signal_service.fft_result = (FftService_Result){0};

    for (point_index = 0U;
         point_index < SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT;
         point_index++) {
        g_signal_service.adc_snapshot[point_index] = 0;
        g_signal_service.dds_snapshot[point_index] = 0;
        g_signal_service.spectrum_snapshot[point_index] = 0;
    }

    dds_config.sample_rate = dds_sample_rate_hz;
    dds_config.waveform = DDS_WAVE_SINE;
    dds_config.amplitude_percent = DDS_AMPLITUDE_DEFAULT_PERCENT;
    dds_init(&g_signal_service.dds, &dds_config);
    dds_set_freq(&g_signal_service.dds,
                 SIGNAL_SERVICE_DEFAULT_DDS_FREQ_HZ);

    g_signal_service.initialized = 1U;
    return HAL_OK;
}

uint32_t SignalService_Process(void)
{
    uint16_t *dac_buffer = NULL;
    uint8_t dac_buffer_index = 0U;
    uint32_t sample_index;
    uint32_t events = SIGNAL_SERVICE_PROCESS_NONE;

    if (g_signal_service.initialized == 0U) {
        return events;
    }

    (void)ADC121S101_Process();
    (void)DAC_Output_Process();

    if (ADC121S101_CopyLatestBlock(g_adc_work_block,
                                   ADC121S101_BLOCK_SIZE) != 0U) {
        if (SignalProcess_BuildTimeSnapshot(
                g_adc_work_block,
                ADC121S101_BLOCK_SIZE,
                g_signal_service.adc_snapshot,
                SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT) != 0U) {
            g_signal_service.adc_snapshot_generation =
                signal_service_next_generation(
                    g_signal_service.adc_snapshot_generation);
            g_signal_service.adc_snapshot_valid = 1U;
            events |= SIGNAL_SERVICE_PROCESS_ADC_UPDATED;
        }

        if (signal_service_process_fft() != 0U) {
            events |= SIGNAL_SERVICE_PROCESS_FFT_UPDATED;
        }
    }

    if (DAC_Output_AcquireBuffer(&dac_buffer, &dac_buffer_index) != 0U) {
        for (sample_index = 0U;
             sample_index < DAC_OUTPUT_BLOCK_SIZE;
             sample_index++) {
            dac_buffer[sample_index] =
                dds_get_sample(&g_signal_service.dds);
        }

        if ((SignalProcess_BuildTimeSnapshot(
                 dac_buffer,
                 DAC_OUTPUT_BLOCK_SIZE,
                 g_signal_service.dds_snapshot,
                 SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT) != 0U) &&
            (DAC_Output_CommitBuffer(dac_buffer_index) != 0U)) {
            g_signal_service.dds_snapshot_generation =
                signal_service_next_generation(
                    g_signal_service.dds_snapshot_generation);
            g_signal_service.dds_snapshot_valid = 1U;
            events |= SIGNAL_SERVICE_PROCESS_DDS_UPDATED;
        }
    }

    return events;
}

HAL_StatusTypeDef SignalService_SetSampleRate(uint32_t requested_hz,
                                             uint32_t *actual_hz)
{
    uint32_t prescaler;
    uint32_t period;
    uint32_t calculated_rate_hz;

    if ((g_signal_service.initialized == 0U) ||
        (signal_service_rate_is_supported(requested_hz) == 0U)) {
        return HAL_ERROR;
    }

    if (signal_service_calculate_timer_dividers(requested_hz,
                                                &prescaler,
                                                &period,
                                                &calculated_rate_hz) !=
        HAL_OK) {
        return HAL_ERROR;
    }

    if (signal_service_apply_timer_dividers(
            g_signal_service.adc_sample_timer,
            prescaler,
            period) != HAL_OK) {
        return HAL_ERROR;
    }

    g_signal_service.sample_rate_hz = calculated_rate_hz;
    if (actual_hz != NULL) {
        *actual_hz = calculated_rate_hz;
    }
    return HAL_OK;
}

HAL_StatusTypeDef SignalService_SetDdsFrequency(uint32_t requested_hz,
                                               uint32_t *applied_hz)
{
    uint32_t max_frequency_hz;
    uint32_t frequency_hz;

    if (g_signal_service.initialized == 0U) {
        return HAL_ERROR;
    }

    max_frequency_hz = g_signal_service.dds_sample_rate_hz / 10U;
    if (max_frequency_hz < SIGNAL_SERVICE_DDS_FREQ_MIN_HZ) {
        return HAL_ERROR;
    }

    frequency_hz = requested_hz;
    if (frequency_hz < SIGNAL_SERVICE_DDS_FREQ_MIN_HZ) {
        frequency_hz = SIGNAL_SERVICE_DDS_FREQ_MIN_HZ;
    }
    if (frequency_hz > max_frequency_hz) {
        frequency_hz = max_frequency_hz;
    }

    dds_set_freq(&g_signal_service.dds, frequency_hz);
    if (applied_hz != NULL) {
        *applied_hz = frequency_hz;
    }
    return HAL_OK;
}

HAL_StatusTypeDef SignalService_SetDdsAmplitude(uint8_t requested_percent,
                                               uint8_t *applied_percent)
{
    uint8_t amplitude_percent = requested_percent;

    if (g_signal_service.initialized == 0U) {
        return HAL_ERROR;
    }

    if (amplitude_percent < DDS_AMPLITUDE_MIN_PERCENT) {
        amplitude_percent = DDS_AMPLITUDE_MIN_PERCENT;
    }
    if (amplitude_percent > DDS_AMPLITUDE_MAX_PERCENT) {
        amplitude_percent = DDS_AMPLITUDE_MAX_PERCENT;
    }

    dds_set_amplitude(&g_signal_service.dds, amplitude_percent);
    if (applied_percent != NULL) {
        *applied_percent = amplitude_percent;
    }
    return HAL_OK;
}

HAL_StatusTypeDef SignalService_SetDdsWaveform(dds_waveform_t waveform)
{
    if ((g_signal_service.initialized == 0U) ||
        ((uint32_t)waveform >= (uint32_t)DDS_WAVE_COUNT)) {
        return HAL_ERROR;
    }

    dds_set_waveform(&g_signal_service.dds, waveform);
    return HAL_OK;
}

HAL_StatusTypeDef SignalService_GetState(SignalService_State *state)
{
    ADC121S101_Status adc_status;
    DAC_Output_Status dac_status;

    if ((g_signal_service.initialized == 0U) || (state == NULL)) {
        return HAL_ERROR;
    }

    ADC121S101_GetStatus(&adc_status);
    DAC_Output_GetStatus(&dac_status);
    state->sample_rate_hz = g_signal_service.sample_rate_hz;
    state->dds_frequency_hz = dds_get_freq(&g_signal_service.dds);
    state->adc_snapshot_generation =
        g_signal_service.adc_snapshot_generation;
    state->dds_snapshot_generation =
        g_signal_service.dds_snapshot_generation;
    state->spectrum_snapshot_generation =
        g_signal_service.spectrum_snapshot_generation;
    state->fft_peak_frequency_millihz = signal_service_float_to_u32(
        g_signal_service.fft_result.dominant_frequency_hz, 1000.0f);
    state->fft_resolution_millihz = signal_service_float_to_u32(
        g_signal_service.fft_result.frequency_resolution_hz, 1000.0f);
    state->fft_peak_amplitude_codes = signal_service_float_to_u16(
        g_signal_service.fft_result.dominant_amplitude_codes);
    state->adc_rms_codes = signal_service_float_to_u16(
        g_signal_service.fft_result.rms_codes);
    state->adc_dropped_block_count = adc_status.dropped_block_count;
    state->adc_error_count = adc_status.error_count;
    state->dac_underrun_count = dac_status.underrun_count;
    state->dds_waveform = dds_get_waveform(&g_signal_service.dds);
    state->dds_amplitude_percent =
        dds_get_amplitude(&g_signal_service.dds);
    state->adc_snapshot_valid = g_signal_service.adc_snapshot_valid;
    state->dds_snapshot_valid = g_signal_service.dds_snapshot_valid;
    state->spectrum_snapshot_valid =
        g_signal_service.spectrum_snapshot_valid;
    return HAL_OK;
}

static uint8_t signal_service_copy_snapshot(const int32_t *source,
                                            uint8_t valid,
                                            uint32_t source_generation,
                                            int32_t *destination,
                                            uint16_t capacity,
                                            uint32_t *generation)
{
    uint32_t point_index;

    if ((valid == 0U) || (destination == NULL) ||
        (capacity < SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT)) {
        return 0U;
    }

    for (point_index = 0U;
         point_index < SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT;
         point_index++) {
        destination[point_index] = source[point_index];
    }
    if (generation != NULL) {
        *generation = source_generation;
    }
    return 1U;
}

uint8_t SignalService_CopyAdcSnapshot(int32_t *points,
                                     uint16_t capacity,
                                     uint32_t *generation)
{
    return signal_service_copy_snapshot(
        g_signal_service.adc_snapshot,
        g_signal_service.adc_snapshot_valid,
        g_signal_service.adc_snapshot_generation,
        points,
        capacity,
        generation);
}

uint8_t SignalService_CopyDdsSnapshot(int32_t *points,
                                     uint16_t capacity,
                                     uint32_t *generation)
{
    return signal_service_copy_snapshot(
        g_signal_service.dds_snapshot,
        g_signal_service.dds_snapshot_valid,
        g_signal_service.dds_snapshot_generation,
        points,
        capacity,
        generation);
}

uint8_t SignalService_CopySpectrumSnapshot(int32_t *points,
                                          uint16_t capacity,
                                          uint32_t *generation)
{
    return signal_service_copy_snapshot(
        g_signal_service.spectrum_snapshot,
        g_signal_service.spectrum_snapshot_valid,
        g_signal_service.spectrum_snapshot_generation,
        points,
        capacity,
        generation);
}
