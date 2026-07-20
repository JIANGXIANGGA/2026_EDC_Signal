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
    TIM_HandleTypeDef *adc_sample_timer; /**< 产生 ADC 采样节拍的定时器句柄。 */
    dds_t dds;                          /**< DDS 波形发生器实例。 */
    FftService_Result fft_result;       /**< 最近一次 ADC 块的 FFT 测量结果。 */
    int32_t adc_snapshot[SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT];      /**< ADC 时域显示快照。 */
    int32_t dds_snapshot[SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT];      /**< DDS 时域显示快照。 */
    int32_t spectrum_snapshot[SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT]; /**< FFT 频谱显示快照。 */
    uint32_t sample_rate_hz;                /**< ADC 当前实际采样率，单位 Hz。 */
    uint32_t dds_sample_rate_hz;            /**< DDS 由 DAC 定时器决定的更新率。 */
    uint32_t adc_snapshot_generation;       /**< ADC 快照更新代次。 */
    uint32_t dds_snapshot_generation;       /**< DDS 快照更新代次。 */
    uint32_t spectrum_snapshot_generation;  /**< 频谱快照更新代次。 */
    uint8_t adc_snapshot_valid;             /**< ADC 快照是否已经生成。 */
    uint8_t dds_snapshot_valid;             /**< DDS 快照是否已经生成。 */
    uint8_t spectrum_snapshot_valid;        /**< 频谱快照是否已经生成。 */
    uint8_t initialized;                    /**< 信号服务是否已完成初始化。 */
} SignalService_Context;

static SignalService_Context g_signal_service; /* 信号服务的唯一运行上下文。 */

/* Driver 快速复制到稳定块后，FFT 才读取；该工作块不参与 DMA。 */
static uint16_t g_adc_work_block[ADC121S101_BLOCK_SIZE]
    SIGNAL_SERVICE_CCMRAM; /* 从 DMA 稳定复制后供快照与 FFT 共用的 ADC 数据块。 */

/** @brief 判断请求的 ADC 采样率是否属于预设支持列表。 */
static uint8_t signal_service_rate_is_supported(uint32_t sample_rate_hz)
{
    return (sample_rate_hz == SIGNAL_SERVICE_SAMPLE_RATE_20K_HZ) ||
           (sample_rate_hz == SIGNAL_SERVICE_SAMPLE_RATE_50K_HZ) ||
           (sample_rate_hz == SIGNAL_SERVICE_SAMPLE_RATE_100K_HZ) ||
           (sample_rate_hz == SIGNAL_SERVICE_SAMPLE_RATE_200K_HZ);
}

/** @brief 根据 APB1 分频配置计算 APB1 定时器输入时钟。 */
static uint32_t signal_service_get_apb1_timer_clock_hz(void)
{
    RCC_ClkInitTypeDef clock_config = {0}; /* 当前系统时钟树配置。 */
    uint32_t flash_latency = 0U;           /* HAL 时钟查询要求返回的 Flash 等待周期。 */
    uint32_t timer_clock_hz = HAL_RCC_GetPCLK1Freq(); /* APB1 定时器输入时钟频率。 */

    HAL_RCC_GetClockConfig(&clock_config, &flash_latency);
    if (clock_config.APB1CLKDivider != RCC_HCLK_DIV1) {
        timer_clock_hz *= 2U;
    }
    return timer_clock_hz;
}

/** @brief 根据定时器 PSC 和 ARR 寄存器计算当前更新频率。 */
static uint32_t signal_service_get_timer_rate_hz(
    const TIM_HandleTypeDef *sample_timer)
{
    uint64_t divider;          /* 定时器预分频器与自动重装值的总分频系数。 */
    uint32_t timer_clock_hz;   /* APB1 定时器输入时钟频率。 */

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

/** @brief 为目标采样率计算 16 位定时器可用的 PSC、ARR 组合。 */
static HAL_StatusTypeDef signal_service_calculate_timer_dividers(
    uint32_t requested_hz,
    uint32_t *prescaler,
    uint32_t *period,
    uint32_t *actual_hz)
{
    uint32_t timer_clock_hz;      /* APB1 定时器输入时钟频率。 */
    uint64_t total_divider;       /* 最接近目标采样率的总分频系数。 */
    uint64_t prescaler_divider;   /* PSC 寄存器对应的实际分频系数。 */
    uint64_t period_divider;      /* ARR 寄存器对应的实际计数周期。 */
    uint64_t denominator;         /* PSC 与 ARR 组合后的实际总分频系数。 */

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

/** @brief 将新的 PSC、ARR 写入采样定时器并立即装载。 */
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

/** @brief 停止 ADC 采样流，原子应用新分频值并在失败时回滚。 */
static HAL_StatusTypeDef signal_service_apply_timer_dividers(
    TIM_HandleTypeDef *sample_timer,
    uint32_t prescaler,
    uint32_t period)
{
    ADC121S101_Status adc_status; /* 修改定时器前读取的 ADC 运行状态。 */
    uint32_t old_prescaler;       /* 配置失败时用于回滚的原 PSC 值。 */
    uint32_t old_period;          /* 配置失败时用于回滚的原 ARR 值。 */
    uint8_t was_running;          /* 修改采样率前 ADC 是否正在运行。 */

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

/** @brief 生成非零且可回卷的快照代次编号。 */
static uint32_t signal_service_next_generation(uint32_t generation)
{
    generation++;
    return (generation != 0U) ? generation : 1U;
}

/** @brief 将非负浮点量按比例四舍五入并饱和转换为 uint32_t。 */
static uint32_t signal_service_float_to_u32(float value, float scale)
{
    float scaled; /* 按指定倍率换算后的浮点值。 */

    if (value <= 0.0f) {
        return 0U;
    }
    scaled = value * scale;
    if (scaled >= (float)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)(scaled + 0.5f);
}

/** @brief 将非负浮点量四舍五入并饱和转换为 uint16_t。 */
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

/** @brief 处理稳定 ADC 块并更新 FFT 结果与频谱显示快照。 */
static uint8_t signal_service_process_fft(void)
{
    const float *magnitudes; /* FFT 服务最近生成的单边幅度谱只读指针。 */
    uint16_t bin_count;      /* 单边幅度谱包含的频点数量。 */

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

/** @brief 初始化 DDS、FFT 和三路显示快照的服务上下文。 */
HAL_StatusTypeDef SignalService_Init(TIM_HandleTypeDef *adc_sample_timer,
                                     TIM_HandleTypeDef *dds_sample_timer)
{
    dds_config_t dds_config;      /* DDS 实例的初始配置。 */
    uint32_t sample_rate_hz;      /* 从 ADC 定时器计算出的实际采样率。 */
    uint32_t dds_sample_rate_hz;  /* 从 DAC 定时器计算出的 DDS 更新率。 */
    uint32_t point_index;         /* 初始化三路显示快照的数组索引。 */

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

/** @brief 在主循环中消费 ADC 块、执行 FFT 并填充 DAC 输出块。 */
uint32_t SignalService_Process(void)
{
    uint16_t *dac_buffer = NULL; /* 当前从 DAC Driver 领取的可写半区。 */
    uint8_t dac_buffer_index = 0U; /* 当前可写 DAC 半区的编号。 */
    uint32_t sample_index;         /* 生成 DDS 输出块时的采样点索引。 */
    uint32_t events = SIGNAL_SERVICE_PROCESS_NONE; /* 本轮主循环产生的数据更新事件位图。 */

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

/** @brief 校验并应用新的 ADC 采样率。 */
HAL_StatusTypeDef SignalService_SetSampleRate(uint32_t requested_hz,
                                             uint32_t *actual_hz)
{
    uint32_t prescaler;          /* 目标采样率对应的 TIM PSC 寄存器值。 */
    uint32_t period;             /* 目标采样率对应的 TIM ARR 寄存器值。 */
    uint32_t calculated_rate_hz; /* PSC/ARR 实际能够产生的采样率。 */

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

/** @brief 将 DDS 输出频率限制在安全范围后应用。 */
HAL_StatusTypeDef SignalService_SetDdsFrequency(uint32_t requested_hz,
                                               uint32_t *applied_hz)
{
    uint32_t max_frequency_hz; /* 当前 DDS 更新率允许的最高输出频率。 */
    uint32_t frequency_hz;     /* 限幅后实际应用的 DDS 输出频率。 */

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

/** @brief 将 DDS 幅度限制在配置范围后应用。 */
HAL_StatusTypeDef SignalService_SetDdsAmplitude(uint8_t requested_percent,
                                               uint8_t *applied_percent)
{
    uint8_t amplitude_percent = requested_percent; /* 限幅后实际应用的 DDS 幅度百分比。 */

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

/** @brief 校验并切换 DDS 输出波形。 */
HAL_StatusTypeDef SignalService_SetDdsWaveform(dds_waveform_t waveform)
{
    if ((g_signal_service.initialized == 0U) ||
        ((uint32_t)waveform >= (uint32_t)DDS_WAVE_COUNT)) {
        return HAL_ERROR;
    }

    dds_set_waveform(&g_signal_service.dds, waveform);
    return HAL_OK;
}

/** @brief 汇总服务参数、FFT 结果及 Driver 运行统计。 */
HAL_StatusTypeDef SignalService_GetState(SignalService_State *state)
{
    ADC121S101_Status adc_status; /* ADC Driver 当前运行统计。 */
    DAC_Output_Status dac_status; /* DAC Driver 当前运行统计。 */

    if ((g_signal_service.initialized == 0U) || (state == NULL)) {
        return HAL_ERROR;
    }

    ADC121S101_GetStatus(&adc_status);
    DAC_Output_GetStatus(&dac_status);
    state->sample_rate_hz = g_signal_service.sample_rate_hz;
    state->dds_frequency_hz = dds_get_freq(&g_signal_service.dds);
    state->adc_snapshot_generation = g_signal_service.adc_snapshot_generation;
    state->dds_snapshot_generation = g_signal_service.dds_snapshot_generation;
    state->spectrum_snapshot_generation = g_signal_service.spectrum_snapshot_generation;
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

/** @brief 校验目标容量并复制一份固定长度的显示快照。 */
static uint8_t signal_service_copy_snapshot(const int32_t *source,
                                            uint8_t valid,
                                            uint32_t source_generation,
                                            int32_t *destination,
                                            uint16_t capacity,
                                            uint32_t *generation)
{
    uint32_t point_index; /* 复制显示快照时的数据点索引。 */

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

/** @brief 将最近的 ADC 时域快照复制给调用方。 */
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

/** @brief 将最近的 DDS 时域快照复制给调用方。 */
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

/** @brief 将最近的 FFT 频谱快照复制给调用方。 */
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
