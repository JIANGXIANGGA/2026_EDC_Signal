/**
 * @file dds_app.c
 * @brief 信号服务与 LVGL 界面的 Application 层实现
 */

#include "dds_app.h"

#include <stddef.h>

#include "signal_service.h"

#define DDS_APP_UI_REFRESH_PERIOD_MS 30U
#define DDS_APP_FREQ_STEP_HZ         10
#define DDS_APP_AMPLITUDE_STEP       10

static const uint32_t g_sample_rate_options[] = { /* 控制页允许选择的 ADC 采样率列表。 */
    SIGNAL_SERVICE_SAMPLE_RATE_20K_HZ,
    SIGNAL_SERVICE_SAMPLE_RATE_50K_HZ,
    SIGNAL_SERVICE_SAMPLE_RATE_100K_HZ,
    SIGNAL_SERVICE_SAMPLE_RATE_200K_HZ,
};

static int32_t g_adc_points[SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT];      /* UI 使用的 ADC 时域数据点。 */
static int32_t g_dds_points[SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT];      /* UI 使用的 DDS 时域数据点。 */
static int32_t g_spectrum_points[SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT]; /* UI 使用的 FFT 频谱数据点。 */
static uint32_t g_adc_generation;       /* 已复制到 UI 缓冲区的 ADC 快照代次。 */
static uint32_t g_dds_generation;       /* 已复制到 UI 缓冲区的 DDS 快照代次。 */
static uint32_t g_spectrum_generation;  /* 已复制到 UI 缓冲区的频谱快照代次。 */
static uint32_t g_last_ui_refresh_tick; /* 上一次允许 UI 刷新的系统毫秒计数。 */
static uint8_t g_sample_rate_index;     /* 当前采样率在可选列表中的索引。 */
static bool g_ui_dirty;                 /* UI 状态是否发生变化并等待刷新。 */

static int8_t g_pending_sample_rate_step;  /* 待处理的采样率列表调整方向。 */
static int32_t g_pending_frequency_delta;  /* 待叠加到 DDS 频率的增量，单位 Hz。 */
static int16_t g_pending_amplitude_delta;  /* 待叠加到 DDS 幅度的百分比增量。 */
static uint8_t g_pending_waveform_next;    /* 是否收到切换到下一波形的请求。 */

/** @brief 将 DDS 波形枚举转换为界面显示文本。 */
static const char *dds_app_waveform_text(dds_waveform_t waveform)
{
    switch(waveform) {
        case DDS_WAVE_SINE:
            return "Sine";
        case DDS_WAVE_COMPOSITE:
            return "Composite";
        case DDS_WAVE_COUNT:
        default:
            return "Unknown";
    }
}

/** @brief 查找与实际采样率最接近的界面采样率选项。 */
static uint8_t dds_app_find_sample_rate_index(uint32_t sample_rate_hz)
{
    uint8_t index;                  /* 遍历采样率选项的数组索引。 */
    uint8_t nearest_index = 0U;     /* 与当前实际采样率最接近的选项索引。 */
    uint32_t nearest_difference = UINT32_MAX; /* 当前找到的最小采样率差值。 */

    for(index = 0U;
        index < (uint8_t)(sizeof(g_sample_rate_options) /
                          sizeof(g_sample_rate_options[0]));
        index++) {
        uint32_t option = g_sample_rate_options[index]; /* 当前检查的候选采样率。 */
        uint32_t difference = (option > sample_rate_hz) ?
                              (option - sample_rate_hz) :
                              (sample_rate_hz - option); /* 候选值与实际采样率的绝对差。 */

        if(difference < nearest_difference) {
            nearest_difference = difference;
            nearest_index = index;
        }
    }
    return nearest_index;
}

/** @brief 记录等待主循环处理的采样率调整方向。 */
static void dds_app_queue_sample_rate_step(int8_t step)
{
    if(step < 0) {
        g_pending_sample_rate_step = -1;
    }
    else if(step > 0) {
        g_pending_sample_rate_step = 1;
    }
}

/** @brief 在主循环中集中应用界面提交的参数修改请求。 */
static void dds_app_apply_requests(void)
{
    SignalService_State state; /* 应用控制请求前读取的信号服务状态。 */
    int8_t sample_rate_step = g_pending_sample_rate_step; /* 本轮消费的采样率调整方向。 */
    int32_t frequency_delta = g_pending_frequency_delta; /* 本轮消费的 DDS 频率增量。 */
    int16_t amplitude_delta = g_pending_amplitude_delta; /* 本轮消费的 DDS 幅度增量。 */
    uint8_t waveform_next = g_pending_waveform_next;     /* 本轮是否切换 DDS 波形。 */

    g_pending_sample_rate_step = 0;
    g_pending_frequency_delta = 0;
    g_pending_amplitude_delta = 0;
    g_pending_waveform_next = 0U;

    if(SignalService_GetState(&state) != HAL_OK) {
        return;
    }

    if(sample_rate_step != 0) {
        int32_t next_index = (int32_t)g_sample_rate_index +
                             (int32_t)sample_rate_step; /* 调整后待应用的采样率选项索引。 */
        int32_t max_index =
            (int32_t)(sizeof(g_sample_rate_options) /
                      sizeof(g_sample_rate_options[0])) - 1; /* 采样率选项数组的最大有效索引。 */

        if(next_index < 0) {
            next_index = 0;
        }
        if(next_index > max_index) {
            next_index = max_index;
        }
        if((uint8_t)next_index != g_sample_rate_index) {
            uint32_t actual_rate; /* 定时器实际能够生成的 ADC 采样率。 */

            if(SignalService_SetSampleRate(
                   g_sample_rate_options[next_index],
                   &actual_rate) == HAL_OK) {
                g_sample_rate_index = (uint8_t)next_index;
                state.sample_rate_hz = actual_rate;
                g_ui_dirty = true;
            }
        }
    }

    if(frequency_delta != 0) {
        int64_t requested = (int64_t)state.dds_frequency_hz +
                            frequency_delta; /* 应用增量后、限幅前的 DDS 目标频率。 */

        if(requested < 0) {
            requested = 0;
        }
        if(SignalService_SetDdsFrequency((uint32_t)requested,
                                         NULL) == HAL_OK) {
            g_ui_dirty = true;
        }
    }

    if(amplitude_delta != 0) {
        int32_t requested = (int32_t)state.dds_amplitude_percent +
                            amplitude_delta; /* 应用增量后、限幅前的 DDS 目标幅度。 */

        if(requested < 0) {
            requested = 0;
        }
        if(requested > UINT8_MAX) {
            requested = UINT8_MAX;
        }
        if(SignalService_SetDdsAmplitude((uint8_t)requested,
                                         NULL) == HAL_OK) {
            g_ui_dirty = true;
        }
    }

    if(waveform_next != 0U) {
        dds_waveform_t waveform =
            (dds_waveform_t)(((uint32_t)state.dds_waveform + 1U) %
                             (uint32_t)DDS_WAVE_COUNT); /* 循环切换后的下一种 DDS 波形。 */

        if(SignalService_SetDdsWaveform(waveform) == HAL_OK) {
            g_ui_dirty = true;
        }
    }
}

/** @brief 将 Service 层新生成的数据快照复制到稳定的 UI 缓冲区。 */
static void dds_app_copy_new_snapshots(void)
{
    SignalService_State state; /* 用于判断各类显示快照是否更新的服务状态。 */

    if(SignalService_GetState(&state) != HAL_OK) {
        return;
    }

    if(state.adc_snapshot_valid &&
       (state.adc_snapshot_generation != g_adc_generation) &&
       SignalService_CopyAdcSnapshot(g_adc_points,
                                     SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT,
                                     &g_adc_generation)) {
        g_ui_dirty = true;
    }

    if(state.dds_snapshot_valid &&
       (state.dds_snapshot_generation != g_dds_generation) &&
       SignalService_CopyDdsSnapshot(g_dds_points,
                                     SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT,
                                     &g_dds_generation)) {
        g_ui_dirty = true;
    }

    if(state.spectrum_snapshot_valid &&
       (state.spectrum_snapshot_generation != g_spectrum_generation) &&
       SignalService_CopySpectrumSnapshot(
           g_spectrum_points,
           SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT,
           &g_spectrum_generation)) {
        g_ui_dirty = true;
    }
}

/** @brief 初始化信号服务及 Application 层状态。 */
HAL_StatusTypeDef dds_app_init(TIM_HandleTypeDef *adc_sample_timer,
                               TIM_HandleTypeDef *dds_sample_timer)
{
    SignalService_State state; /* 初始化后读取的信号服务状态。 */
    uint32_t index;            /* 清零三路 UI 数据点数组的索引。 */

    for(index = 0U; index < SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT; index++) {
        g_adc_points[index] = 0;
        g_dds_points[index] = 0;
        g_spectrum_points[index] = 0;
    }

    g_adc_generation = 0U;
    g_dds_generation = 0U;
    g_spectrum_generation = 0U;
    g_last_ui_refresh_tick = HAL_GetTick();
    g_pending_sample_rate_step = 0;
    g_pending_frequency_delta = 0;
    g_pending_amplitude_delta = 0;
    g_pending_waveform_next = 0U;
    g_ui_dirty = true;

    if(SignalService_Init(adc_sample_timer, dds_sample_timer) != HAL_OK) {
        return HAL_ERROR;
    }
    if(SignalService_GetState(&state) != HAL_OK) {
        return HAL_ERROR;
    }

    g_sample_rate_index =
        dds_app_find_sample_rate_index(state.sample_rate_hz);
    return HAL_OK;
}

/** @brief 请求将 ADC 采样率切换到较低一档。 */
void dds_app_request_sample_rate_down(void)
{
    dds_app_queue_sample_rate_step(-1);
}

/** @brief 请求将 ADC 采样率切换到较高一档。 */
void dds_app_request_sample_rate_up(void)
{
    dds_app_queue_sample_rate_step(1);
}

/** @brief 请求降低 DDS 输出频率。 */
void dds_app_request_freq_down(void)
{
    g_pending_frequency_delta -= DDS_APP_FREQ_STEP_HZ;
}

/** @brief 请求提高 DDS 输出频率。 */
void dds_app_request_freq_up(void)
{
    g_pending_frequency_delta += DDS_APP_FREQ_STEP_HZ;
}

/** @brief 请求降低 DDS 输出幅度。 */
void dds_app_request_amplitude_down(void)
{
    g_pending_amplitude_delta -= DDS_APP_AMPLITUDE_STEP;
}

/** @brief 请求提高 DDS 输出幅度。 */
void dds_app_request_amplitude_up(void)
{
    g_pending_amplitude_delta += DDS_APP_AMPLITUDE_STEP;
}

/** @brief 请求切换到下一种 DDS 波形。 */
void dds_app_request_next_waveform(void)
{
    g_pending_waveform_next = 1U;
}

/** @brief 处理控制请求和数据快照，并判断是否需要刷新 UI。 */
bool dds_app_process(void)
{
    uint32_t now; /* 当前系统毫秒计数，用于限制 UI 刷新频率。 */

    dds_app_apply_requests();
    (void)SignalService_Process();
    dds_app_copy_new_snapshots();

    now = HAL_GetTick();
    if(g_ui_dirty &&
       ((now - g_last_ui_refresh_tick) >= DDS_APP_UI_REFRESH_PERIOD_MS)) {
        g_last_ui_refresh_tick = now;
        g_ui_dirty = false;
        return true;
    }
    return false;
}

/** @brief 将 Service 层状态整理为 UI 可直接使用的状态快照。 */
void dds_app_fill_ui_state(ui_signal_state_t *state)
{
    SignalService_State service_state; /* 从 Service 层读取并转换给 UI 的状态快照。 */

    if((state == NULL) ||
       (SignalService_GetState(&service_state) != HAL_OK)) {
        return;
    }

    state->sample_rate_hz = service_state.sample_rate_hz;
    state->dds_freq_hz = service_state.dds_frequency_hz;
    state->dds_amplitude_percent =
        service_state.dds_amplitude_percent;
    state->dds_waveform_text =
        dds_app_waveform_text(service_state.dds_waveform);
    state->fft_peak_frequency_millihz =
        service_state.fft_peak_frequency_millihz;
    state->fft_resolution_millihz =
        service_state.fft_resolution_millihz;
    state->fft_peak_amplitude_codes =
        service_state.fft_peak_amplitude_codes;
    state->adc_rms_codes = service_state.adc_rms_codes;
    state->adc_dropped_block_count =
        service_state.adc_dropped_block_count;
    state->adc_error_count = service_state.adc_error_count;
    state->dac_underrun_count = service_state.dac_underrun_count;
    state->adc_points = g_adc_points;
    state->dds_points = g_dds_points;
    state->spectrum_points = g_spectrum_points;
    state->wave_point_count = SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT;
    state->spectrum_point_count = SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT;
}
