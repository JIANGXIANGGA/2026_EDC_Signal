#include "signal_hmi_app.h"

#include <stddef.h>

#include "signal_acquisition_service.h"
#include "signal_measurement_service.h"
#include "tjc_screen_config.h"
#include "tjc_uart_driver.h"
#include "usart_hmi_service.h"
#include "waveform_analyzer_service.h"

#if TJC_SCREEN_ENABLE_GENERATOR_CONTROL
#include "ad9910_signal_generator_app.h"
#include "tjc_ad9910_interface.h"
#endif

#define ONE_HMI_VAR_COUNT 7U
#define ONE_HMI_MAX_AMPLITUDE_PERCENT 100U
#define ONE_HMI_QUERY_TIMEOUT_MS 200U
#define ONE_HMI_STARTUP_SYNC_DELAY_MS 800U
#define ONE_HMI_PERIODIC_SYNC_MS 500U
#define ONE_HMI_LINK_TIMEOUT_MS 2000U
#define ONE_HMI_RX_TRANSFER_BUFFER_SIZE 64U
#define ONE_HMI_WAVEFORM_REFRESH_MS 250U
#define ONE_HMI_WAVEFORM_PAGE_TIMEOUT_MS 200U
#define ONE_HMI_WAVEFORM_TRANSFER_TIMEOUT_MS 1000U
#define ONE_HMI_WAVEFORM_MIN_SAMPLES_PER_CYCLE 2U
#define ONE_HMI_TIME_DISPLAY_MIN 16U
#define ONE_HMI_TIME_DISPLAY_MAX 239U
#define ONE_HMI_SPECTRUM_DISPLAY_MIN 8U
#define ONE_HMI_SPECTRUM_DISPLAY_MAX 239U
#define ONE_HMI_SPECTRUM_LEFT_MARGIN_POINTS 6U
#define ONE_HMI_SPECTRUM_RIGHT_MARGIN_POINTS 6U
#define ONE_HMI_SPECTRUM_LINE_HALF_WIDTH_POINTS 2U
#define ONE_HMI_SPECTRUM_RIGHT_HEADROOM_RATIO 1.10f
#define ONE_HMI_MEASUREMENT_FIELD_COUNT 12U
#define ONE_HMI_MEASUREMENT_REFRESH_MS 200U
#define ONE_HMI_PLOT_SWITCH_DELAY_MS 20U

_Static_assert(TJC_SCREEN_PLOT_POINT_COUNT <=
                   USART_HMI_TRANSPARENT_MAX_SIZE,
               "TJC waveform exceeds transparent transfer limit");
_Static_assert(TJC_SCREEN_PLOT_POINT_COUNT > 1U,
               "TJC waveform requires at least two points");
_Static_assert((ONE_HMI_SPECTRUM_LEFT_MARGIN_POINTS +
                ONE_HMI_SPECTRUM_RIGHT_MARGIN_POINTS + 1U) <
                   TJC_SCREEN_PLOT_POINT_COUNT,
               "TJC spectrum horizontal margins are too large");

typedef enum {
    ONE_HMI_VAR_WAVEFORM = 0,
    ONE_HMI_VAR_AMPLITUDE,
    ONE_HMI_VAR_PHASE,
    ONE_HMI_VAR_FREQUENCY,
    ONE_HMI_VAR_MODE,
    ONE_HMI_VAR_PROFILE,
    ONE_HMI_VAR_RUN_FLAG
} one_hmi_var_index_t;

typedef struct {
    const char *object_name;
    int32_t value;
} one_hmi_variable_t;

typedef struct {
    one_hmi_variable_t variables[ONE_HMI_VAR_COUNT];
    uint8_t pending_sync;
    uint8_t active_query;
    uint8_t restart_sync_pending;
    uint8_t applied_snapshot_valid;
    uint8_t query_index;
    uint32_t query_deadline_ms;
    uint32_t next_sync_ms;
    uint32_t observed_rx_restart_count;
    uint32_t observed_rx_dropped_count;
    uint32_t waveform_deadline_ms;
    uint32_t waveform_next_refresh_ms;
    uint32_t waveform_pending_sequence;
    uint8_t waveform_snapshot_requested;
    uint32_t plot_last_sequence[2];
    uint32_t measurement_next_refresh_ms;
    uint32_t measurement_pending_sequence;
    uint8_t measurement_publish_active;
    uint8_t measurement_field_index;
    signal_measurement_result_t measurement_snapshot;
    uint8_t waveform_points[TJC_SCREEN_PLOT_POINT_COUNT];
    signal_hmi_status_t status;
} one_hmi_context_t;

static one_hmi_context_t g_one_hmi;

static const char *const g_one_hmi_variable_names[ONE_HMI_VAR_COUNT] = {
    TJC_SCREEN_GEN_WAVEFORM,
    TJC_SCREEN_GEN_AMPLITUDE_PERCENT,
    TJC_SCREEN_GEN_PHASE_DEGREES,
    TJC_SCREEN_GEN_FREQUENCY_HZ,
    TJC_SCREEN_GEN_MODE,
    TJC_SCREEN_GEN_RAM_PRESET,
    TJC_SCREEN_GEN_RUN_FLAG,
};

/* 页面 0 隐藏数值变量协议：幅值均以 0.1 mV 为单位。 */
static const char *const
    g_one_hmi_measurement_names[ONE_HMI_MEASUREMENT_FIELD_COUNT] = {
        TJC_SCREEN_MEAS_SIGNAL_VALID,
        TJC_SCREEN_MEAS_PEAK_TO_PEAK,
        TJC_SCREEN_MEAS_TRUE_RMS,
        TJC_SCREEN_MEAS_FUNDAMENTAL_HZ,
        TJC_SCREEN_MEAS_COMPONENT_COUNT,
        TJC_SCREEN_MEAS_COMPONENT1_HZ,
        TJC_SCREEN_MEAS_COMPONENT1_AMPLITUDE,
        TJC_SCREEN_MEAS_COMPONENT2_HZ,
        TJC_SCREEN_MEAS_COMPONENT2_AMPLITUDE,
        TJC_SCREEN_MEAS_COMPONENT3_HZ,
        TJC_SCREEN_MEAS_COMPONENT3_AMPLITUDE,
        TJC_SCREEN_MEAS_WAVEFORM_CYCLES,
};

static uint8_t one_hmi_time_reached(uint32_t deadline_ms)
{
    return ((int32_t)(HAL_GetTick() - deadline_ms) >= 0) ? 1U : 0U;
}

static void one_hmi_schedule_next_sync(uint32_t delay_ms)
{
    g_one_hmi.next_sync_ms = HAL_GetTick() + delay_ms;
}

static void one_hmi_waveform_set_state(one_hmi_waveform_state_t state)
{
    g_one_hmi.status.waveform_state = state;
}

static void one_hmi_waveform_schedule_next(void)
{
    g_one_hmi.waveform_next_refresh_ms =
        HAL_GetTick() + ONE_HMI_WAVEFORM_REFRESH_MS;
}

static void one_hmi_measurement_schedule_next(void)
{
    g_one_hmi.measurement_next_refresh_ms =
        HAL_GetTick() + ONE_HMI_MEASUREMENT_REFRESH_MS;
}

static void one_hmi_waveform_reset(void)
{
    one_hmi_waveform_set_state(ONE_HMI_WAVEFORM_STATE_IDLE);
    g_one_hmi.waveform_deadline_ms = 0U;
    g_one_hmi.waveform_pending_sequence = 0U;
    g_one_hmi.waveform_snapshot_requested = 0U;
    g_one_hmi.plot_last_sequence[ONE_HMI_PLOT_TIME_DOMAIN] = 0U;
    g_one_hmi.plot_last_sequence[ONE_HMI_PLOT_SPECTRUM] = 0U;
    g_one_hmi.status.waveform_last_sequence = 0U;
    g_one_hmi.status.active_plot = ONE_HMI_PLOT_TIME_DOMAIN;
    g_one_hmi.status.current_page_known = 0U;
    g_one_hmi.status.waveform_snapshot_pending = 0U;
    one_hmi_waveform_schedule_next();
}

static HAL_StatusTypeDef one_hmi_waveform_fail(HAL_StatusTypeDef status)
{
    one_hmi_waveform_set_state(ONE_HMI_WAVEFORM_STATE_IDLE);
    g_one_hmi.status.waveform_last_hal_status = status;
    g_one_hmi.status.waveform_error_count++;
    if (status == HAL_TIMEOUT) {
        g_one_hmi.status.waveform_timeout_count++;
    }
    if (TJC_SCREEN_PLOT_SNAPSHOT_MODE != 0U) {
        g_one_hmi.waveform_snapshot_requested = 0U;
        g_one_hmi.status.waveform_snapshot_pending = 0U;
        g_one_hmi.status.active_plot = ONE_HMI_PLOT_TIME_DOMAIN;
    } else {
        g_one_hmi.status.active_plot =
            (g_one_hmi.status.active_plot == ONE_HMI_PLOT_TIME_DOMAIN) ?
                ONE_HMI_PLOT_SPECTRUM :
                ONE_HMI_PLOT_TIME_DOMAIN;
        one_hmi_waveform_schedule_next();
    }
    return status;
}

static uint32_t one_hmi_waveform_get_source_span(
    const waveform_analyzer_result_t *analysis,
    uint32_t sample_count)
{
    const signal_measurement_result_t *measurement =
        Signal_Measurement_Service_GetResult();
    uint32_t frequency_hz;
    uint32_t minimum_span;
    uint64_t span;

    if ((analysis == NULL) || (analysis->result_ready == 0U) ||
        (analysis->sample_rate_hz == 0U)) {
        return sample_count;
    }

    if ((measurement != NULL) && (measurement->result_ready != 0U) &&
        (measurement->fundamental_frequency_hz >= 1.0f)) {
        frequency_hz = (uint32_t)(
            measurement->fundamental_frequency_hz + 0.5f);
    } else if (analysis->fundamental_frequency_hz >= 1.0f) {
        frequency_hz = (uint32_t)(
            analysis->fundamental_frequency_hz + 0.5f);
    } else {
        return sample_count;
    }
    if (frequency_hz == 0U) {
        return sample_count;
    }

    span = (((uint64_t)analysis->sample_rate_hz *
             g_one_hmi.status.waveform_cycles) +
            (frequency_hz / 2U)) /
           frequency_hz;
    minimum_span =
        (uint32_t)g_one_hmi.status.waveform_cycles *
        ONE_HMI_WAVEFORM_MIN_SAMPLES_PER_CYCLE;
    if (span < minimum_span) {
        span = minimum_span;
    }
    if (span > sample_count) {
        span = sample_count;
    }

    return (uint32_t)span;
}

static uint32_t one_hmi_waveform_find_trigger(
    const uint16_t *samples,
    uint32_t sample_count,
    uint32_t source_span,
    uint16_t threshold)
{
    uint32_t max_start;

    if (source_span >= sample_count) {
        return 0U;
    }

    max_start = sample_count - source_span;
    for (uint32_t index = 1U; index <= max_start; ++index) {
        if ((samples[index - 1U] <= threshold) &&
            (samples[index] > threshold)) {
            return index;
        }
    }

    return 0U;
}

static uint16_t one_hmi_waveform_interpolate(
    const uint16_t *samples,
    uint32_t source_start,
    uint32_t source_span,
    uint16_t point_index)
{
    const uint32_t denominator = TJC_SCREEN_PLOT_POINT_COUNT - 1U;
    const uint64_t position =
        (uint64_t)point_index * (uint64_t)(source_span - 1U);
    const uint32_t source_offset = (uint32_t)(position / denominator);
    const uint32_t remainder = (uint32_t)(position % denominator);
    const uint32_t first_index = source_start + source_offset;
    const uint32_t second_index =
        (source_offset + 1U < source_span) ? first_index + 1U : first_index;
    const uint32_t first_weight = denominator - remainder;

    return (uint16_t)((((uint32_t)samples[first_index] * first_weight) +
                       ((uint32_t)samples[second_index] * remainder) +
                       (denominator / 2U)) /
                      denominator);
}

static uint8_t one_hmi_waveform_scale_sample(uint16_t sample,
                                               uint16_t min_sample,
                                               uint16_t max_sample)
{
    const uint32_t display_span =
        ONE_HMI_TIME_DISPLAY_MAX - ONE_HMI_TIME_DISPLAY_MIN;
    uint32_t scaled;

    if (max_sample <= min_sample) {
        return (uint8_t)((ONE_HMI_TIME_DISPLAY_MIN +
                          ONE_HMI_TIME_DISPLAY_MAX) /
                         2U);
    }

    if (sample < min_sample) {
        sample = min_sample;
    } else if (sample > max_sample) {
        sample = max_sample;
    }

    scaled = ONE_HMI_TIME_DISPLAY_MIN +
             ((((uint32_t)(sample - min_sample) * display_span) +
               ((uint32_t)(max_sample - min_sample) / 2U)) /
              (uint32_t)(max_sample - min_sample));
    return (uint8_t)scaled;
}

/* 根据串口屏的实际绘制方向调整点序，保证时间轴从左向右。 */
static void one_hmi_waveform_apply_display_direction(void)
{
#if TJC_SCREEN_REVERSE_TIME_PLOT_POINTS
    uint16_t left = 0U;
    uint16_t right = TJC_SCREEN_PLOT_POINT_COUNT - 1U;

    while (left < right) {
        const uint8_t temporary = g_one_hmi.waveform_points[left];
        g_one_hmi.waveform_points[left] = g_one_hmi.waveform_points[right];
        g_one_hmi.waveform_points[right] = temporary;
        ++left;
        --right;
    }
#endif
}

/* 频谱横轴必须保持低频在左、高频在右。 */
static void one_hmi_spectrum_apply_display_direction(void)
{
#if TJC_SCREEN_REVERSE_SPECTRUM_PLOT_POINTS
    uint16_t left = 0U;
    uint16_t right = TJC_SCREEN_PLOT_POINT_COUNT - 1U;

    while (left < right) {
        const uint8_t temporary = g_one_hmi.waveform_points[left];
        g_one_hmi.waveform_points[left] = g_one_hmi.waveform_points[right];
        g_one_hmi.waveform_points[right] = temporary;
        ++left;
        --right;
    }
#endif
}

/* 波形控件无法绘制真正的竖线，用窄脉冲增强单像素谱线的可见度。 */
static void one_hmi_spectrum_draw_line(uint32_t center_x, uint8_t y)
{
    const uint32_t first =
        (center_x > ONE_HMI_SPECTRUM_LINE_HALF_WIDTH_POINTS) ?
            center_x - ONE_HMI_SPECTRUM_LINE_HALF_WIDTH_POINTS :
            0U;
    uint32_t last = center_x + ONE_HMI_SPECTRUM_LINE_HALF_WIDTH_POINTS;

    if (last >= TJC_SCREEN_PLOT_POINT_COUNT) {
        last = TJC_SCREEN_PLOT_POINT_COUNT - 1U;
    }

    for (uint32_t x = first; x <= last; ++x) {
        if (y > g_one_hmi.waveform_points[x]) {
            g_one_hmi.waveform_points[x] = y;
        }
    }
}

static uint8_t one_hmi_waveform_prepare(void)
{
    const waveform_analyzer_result_t *analysis =
        Waveform_Analyzer_GetResult();
    const uint16_t *samples;
    uint32_t sample_count;
    uint32_t sequence;
    uint32_t source_span;
    uint32_t source_start;
    uint16_t threshold;
    uint16_t min_sample = UINT16_MAX;
    uint16_t max_sample = 0U;

    if (Signal_Acquisition_Service_GetLatestBlock(
            &samples, &sample_count, &sequence) == 0U) {
        return 0U;
    }
    if (sequence ==
        g_one_hmi.plot_last_sequence[ONE_HMI_PLOT_TIME_DOMAIN]) {
        return 0U;
    }

    source_span = one_hmi_waveform_get_source_span(analysis, sample_count);
    threshold = ((analysis != NULL) && (analysis->result_ready != 0U)) ?
                    analysis->average_code :
                    2048U;
    source_start = one_hmi_waveform_find_trigger(
        samples, sample_count, source_span, threshold);

    for (uint32_t index = 0U; index < source_span; ++index) {
        const uint16_t sample = samples[source_start + index];
        if (sample < min_sample) {
            min_sample = sample;
        }
        if (sample > max_sample) {
            max_sample = sample;
        }
    }

    for (uint16_t index = 0U;
         index < TJC_SCREEN_PLOT_POINT_COUNT;
         ++index) {
        const uint16_t sample = one_hmi_waveform_interpolate(
            samples, source_start, source_span, index);
        g_one_hmi.waveform_points[index] =
            one_hmi_waveform_scale_sample(sample, min_sample, max_sample);
    }

    one_hmi_waveform_apply_display_direction();

    g_one_hmi.waveform_pending_sequence = sequence;
    return 1U;
}

static uint8_t one_hmi_spectrum_prepare(void)
{
    const signal_measurement_result_t *measurement =
        Signal_Measurement_Service_GetResult();
    float max_amplitude = 0.0f;
    float max_frequency = 0.0f;

    if ((measurement == NULL) || (measurement->result_ready == 0U) ||
        (measurement->measurement_count ==
         g_one_hmi.plot_last_sequence[ONE_HMI_PLOT_SPECTRUM])) {
        return 0U;
    }

    for (uint16_t index = 0U;
         index < TJC_SCREEN_PLOT_POINT_COUNT;
         ++index) {
        g_one_hmi.waveform_points[index] =
            ONE_HMI_SPECTRUM_DISPLAY_MIN;
    }

    for (uint8_t index = 0U;
         index < measurement->component_count;
         ++index) {
        if ((measurement->components[index].valid != 0U) &&
            (measurement->components[index].amplitude_mv > max_amplitude)) {
            max_amplitude = measurement->components[index].amplitude_mv;
        }
        if ((measurement->components[index].valid != 0U) &&
            (measurement->components[index].frequency_hz > max_frequency)) {
            max_frequency = measurement->components[index].frequency_hz;
        }
    }

    if ((max_amplitude > 0.0f) && (max_frequency > 0.0f)) {
        const float frequency_span =
            max_frequency * ONE_HMI_SPECTRUM_RIGHT_HEADROOM_RATIO;
        const float display_span =
            (float)(ONE_HMI_SPECTRUM_DISPLAY_MAX -
                    ONE_HMI_SPECTRUM_DISPLAY_MIN);
        const uint32_t horizontal_span =
            TJC_SCREEN_PLOT_POINT_COUNT - 1U -
            ONE_HMI_SPECTRUM_LEFT_MARGIN_POINTS -
            ONE_HMI_SPECTRUM_RIGHT_MARGIN_POINTS;

        for (uint8_t index = 0U;
             index < measurement->component_count;
             ++index) {
            const float frequency =
                measurement->components[index].frequency_hz;
            uint32_t x;
            uint32_t y;

            if ((measurement->components[index].valid == 0U) ||
                (frequency <= 0.0f)) {
                continue;
            }

            x = ONE_HMI_SPECTRUM_LEFT_MARGIN_POINTS +
                (uint32_t)(((frequency * (float)horizontal_span) /
                            frequency_span) +
                           0.5f);
            if (x > (TJC_SCREEN_PLOT_POINT_COUNT - 1U -
                     ONE_HMI_SPECTRUM_RIGHT_MARGIN_POINTS)) {
                x = TJC_SCREEN_PLOT_POINT_COUNT - 1U -
                    ONE_HMI_SPECTRUM_RIGHT_MARGIN_POINTS;
            }
            y = ONE_HMI_SPECTRUM_DISPLAY_MIN +
                (uint32_t)(((measurement->components[index].amplitude_mv /
                             max_amplitude) *
                            display_span) +
                           0.5f);
            if (y > ONE_HMI_SPECTRUM_DISPLAY_MAX) {
                y = ONE_HMI_SPECTRUM_DISPLAY_MAX;
            }
            one_hmi_spectrum_draw_line(x, (uint8_t)y);
        }
    }

    one_hmi_spectrum_apply_display_direction();

    g_one_hmi.waveform_pending_sequence =
        measurement->measurement_count;
    return 1U;
}

static uint8_t one_hmi_plot_prepare(void)
{
    if (g_one_hmi.status.active_plot == ONE_HMI_PLOT_SPECTRUM) {
        return one_hmi_spectrum_prepare();
    }

    return one_hmi_waveform_prepare();
}

static uint8_t one_hmi_plot_component_id(void)
{
    return (g_one_hmi.status.active_plot == ONE_HMI_PLOT_SPECTRUM) ?
               TJC_SCREEN_SPECTRUM_PLOT_COMPONENT_ID :
               TJC_SCREEN_TIME_PLOT_COMPONENT_ID;
}

static int32_t one_hmi_round_nonnegative(float value)
{
    if (value <= 0.0f) {
        return 0;
    }
    if (value >= (float)INT32_MAX) {
        return INT32_MAX;
    }

    return (int32_t)(value + 0.5f);
}

static int32_t one_hmi_measurement_field_value(uint8_t field_index)
{
    const signal_measurement_result_t *measurement =
        &g_one_hmi.measurement_snapshot;
    uint8_t component_index;

    switch (field_index) {
    case 0U:
        return measurement->signal_valid;
    case 1U:
        return one_hmi_round_nonnegative(
            measurement->peak_to_peak_mv * 10.0f);
    case 2U:
        return one_hmi_round_nonnegative(
            measurement->true_rms_mv * 10.0f);
    case 3U:
        return one_hmi_round_nonnegative(
            measurement->fundamental_frequency_hz);
    case 4U:
        return measurement->component_count;
    case 11U:
        return g_one_hmi.status.waveform_cycles;
    default:
        break;
    }

    component_index = (uint8_t)((field_index - 5U) / 2U);
    if ((component_index >= SIGNAL_MEASUREMENT_COMPONENT_COUNT) ||
        (measurement->components[component_index].valid == 0U)) {
        return 0;
    }
    if (((field_index - 5U) & 1U) == 0U) {
        return one_hmi_round_nonnegative(
            measurement->components[component_index].frequency_hz);
    }

    return one_hmi_round_nonnegative(
        measurement->components[component_index].amplitude_mv * 10.0f);
}

#if TJC_SCREEN_ENABLE_MEASUREMENT_SELF_TEST
static void one_hmi_measurement_build_test_snapshot(void)
{
    signal_measurement_result_t *measurement =
        &g_one_hmi.measurement_snapshot;
    uint32_t sequence =
        g_one_hmi.status.measurement_last_sequence + 1U;

    if (sequence == 0U) {
        sequence = 1U;
    }

    *measurement = (signal_measurement_result_t){0};
    measurement->initialized = 1U;
    measurement->result_ready = 1U;
    measurement->signal_valid = 1U;
    measurement->component_count = SIGNAL_MEASUREMENT_COMPONENT_COUNT;
    measurement->measurement_count = sequence;
    measurement->peak_to_peak_mv = 600.0f;
    measurement->true_rms_mv = 212.1f;
    measurement->raw_rms_mv = 212.1f;
    measurement->fundamental_frequency_hz = 10000.0f;

    measurement->components[0].valid = 1U;
    measurement->components[0].harmonic_order = 1U;
    measurement->components[0].frequency_hz = 10000.0f;
    measurement->components[0].amplitude_mv = 100.0f;

    measurement->components[1].valid = 1U;
    measurement->components[1].harmonic_order = 2U;
    measurement->components[1].frequency_hz = 20000.0f;
    measurement->components[1].amplitude_mv = 200.0f;

    measurement->components[2].valid = 1U;
    measurement->components[2].harmonic_order = 3U;
    measurement->components[2].frequency_hz = 30000.0f;
    measurement->components[2].amplitude_mv = 300.0f;
}
#endif

static HAL_StatusTypeDef one_hmi_measurement_publish_process(void)
{
    const signal_measurement_result_t *measurement;
    HAL_StatusTypeDef status;

    if (g_one_hmi.measurement_publish_active != 0U) {
        if (g_one_hmi.measurement_field_index >=
            ONE_HMI_MEASUREMENT_FIELD_COUNT) {
            g_one_hmi.measurement_publish_active = 0U;
            g_one_hmi.status.measurement_last_sequence =
                g_one_hmi.measurement_pending_sequence;
            g_one_hmi.status.measurement_publish_count++;
            one_hmi_measurement_schedule_next();
            return HAL_OK;
        }

        status = Usart_HMI_Service_SetNumber(
            g_one_hmi_measurement_names[
                g_one_hmi.measurement_field_index],
            one_hmi_measurement_field_value(
                g_one_hmi.measurement_field_index));
        if (status == HAL_OK) {
            g_one_hmi.measurement_field_index++;
        } else if (status != HAL_BUSY) {
            g_one_hmi.measurement_publish_active = 0U;
            g_one_hmi.status.measurement_publish_error_count++;
            one_hmi_measurement_schedule_next();
        }
        return status;
    }

    if ((g_one_hmi.pending_sync != 0U) ||
        (g_one_hmi.active_query != 0U) ||
        (g_one_hmi.status.waveform_state !=
         ONE_HMI_WAVEFORM_STATE_IDLE) ||
        (one_hmi_time_reached(
             g_one_hmi.measurement_next_refresh_ms) == 0U)) {
        return HAL_OK;
    }

#if TJC_SCREEN_ENABLE_MEASUREMENT_SELF_TEST
    one_hmi_measurement_build_test_snapshot();
    measurement = &g_one_hmi.measurement_snapshot;
#else
    measurement = Signal_Measurement_Service_GetResult();
#endif
    if ((measurement == NULL) || (measurement->result_ready == 0U) ||
        (measurement->measurement_count ==
         g_one_hmi.status.measurement_last_sequence)) {
        one_hmi_measurement_schedule_next();
        return HAL_OK;
    }

    g_one_hmi.measurement_snapshot = *measurement;
    g_one_hmi.measurement_pending_sequence =
        measurement->measurement_count;
    g_one_hmi.measurement_field_index = 0U;
    g_one_hmi.measurement_publish_active = 1U;
    return HAL_OK;
}

#if TJC_SCREEN_ENABLE_GENERATOR_CONTROL
static uint8_t one_hmi_mode_is_valid(int32_t mode)
{
    return ((mode == ONE_HMI_MODE_SINGLE) ||
            (mode == ONE_HMI_MODE_RAM) ||
            (mode == ONE_HMI_MODE_SWEEP) ||
            (mode == ONE_HMI_MODE_PROFILE)) ?
               1U :
               0U;
}

static uint8_t one_hmi_waveform_is_valid(int32_t waveform)
{
    return ((waveform >= ONE_HMI_WAVE_SINE) &&
            (waveform <= ONE_HMI_WAVE_SAW)) ?
               1U :
               0U;
}
#endif

static HAL_StatusTypeDef one_hmi_send_bytes(const uint8_t *data,
                                             uint16_t length,
                                             void *user_context)
{
    (void)user_context;
    return TJC_UART_Driver_Transmit(data, length);
}

#if TJC_SCREEN_ENABLE_GENERATOR_CONTROL
static uint8_t one_hmi_u8_clamp(int32_t value, uint8_t max_value)
{
    if (value <= 0) {
        return 0U;
    }
    if (value > (int32_t)max_value) {
        return max_value;
    }

    return (uint8_t)value;
}

static uint16_t one_hmi_phase_clamp(int32_t value)
{
    if (value <= 0) {
        return 0U;
    }
    if (value > (int32_t)AD9910_MAX_PHASE_DEGREES) {
        return AD9910_MAX_PHASE_DEGREES;
    }

    return (uint16_t)value;
}

static uint32_t one_hmi_frequency_clamp(int32_t value)
{
    if (value <= 0) {
        return 1U;
    }
    if ((uint32_t)value > AD9910_MAX_FREQUENCY_HZ) {
        return AD9910_MAX_FREQUENCY_HZ;
    }

    return (uint32_t)value;
}

static ad9910_siggen_waveform_t one_hmi_build_waveform(int32_t value)
{
    switch (value) {
    case ONE_HMI_WAVE_TRIANGLE:
        return AD9910_SIGGEN_WAVEFORM_TRIANGLE;

    case ONE_HMI_WAVE_SQUARE:
        return AD9910_SIGGEN_WAVEFORM_SQUARE;

    case ONE_HMI_WAVE_SAW:
        return AD9910_SIGGEN_WAVEFORM_SAW_RISE;

    case ONE_HMI_WAVE_SINE:
    default:
        return AD9910_SIGGEN_WAVEFORM_SINE;
    }
}

static HAL_StatusTypeDef one_hmi_dispatch_command(
    const ad9910_siggen_command_t *command)
{
    HAL_StatusTypeDef status = TJC_AD9910_Interface_Dispatch(command);

    g_one_hmi.status.last_hal_status = status;
    if (status != HAL_OK) {
        g_one_hmi.status.error = ONE_HMI_ERROR_AD9910_COMMAND;
        g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_ERROR;
    }

    return status;
}

static HAL_StatusTypeDef one_hmi_apply_single_tone(
    const ad9910_siggen_tone_param_t *tone)
{
    ad9910_siggen_command_t command;
    HAL_StatusTypeDef status;

    command = (ad9910_siggen_command_t){0};
    command.type = AD9910_SIGGEN_COMMAND_SET_SINGLE_TONE;
    command.tone = *tone;
    status = one_hmi_dispatch_command(&command);
    if (status != HAL_OK) {
        return status;
    }

    command = (ad9910_siggen_command_t){0};
    command.type = AD9910_SIGGEN_COMMAND_SET_MODE;
    command.mode = AD9910_SIGGEN_MODE_SINGLE_TONE;
    return one_hmi_dispatch_command(&command);
}

static HAL_StatusTypeDef one_hmi_apply_ram_waveform(
    const ad9910_siggen_tone_param_t *tone,
    ad9910_siggen_waveform_t waveform,
    uint8_t profile_index)
{
    ad9910_siggen_command_t command;
    HAL_StatusTypeDef status;

    command = (ad9910_siggen_command_t){0};
    command.type = AD9910_SIGGEN_COMMAND_SET_RAM_PRESET_TONE;
    command.ram_preset_index = profile_index;
    command.tone = *tone;
    status = one_hmi_dispatch_command(&command);
    if (status != HAL_OK) {
        return status;
    }

    command = (ad9910_siggen_command_t){0};
    command.type = AD9910_SIGGEN_COMMAND_SET_RAM_PRESET_WAVEFORM;
    command.ram_preset_index = profile_index;
    command.waveform = waveform;
    status = one_hmi_dispatch_command(&command);
    if (status != HAL_OK) {
        return status;
    }

    command = (ad9910_siggen_command_t){0};
    command.type = AD9910_SIGGEN_COMMAND_SELECT_RAM_PRESET;
    command.ram_preset_index = profile_index;
    status = one_hmi_dispatch_command(&command);
    if (status != HAL_OK) {
        return status;
    }

    command = (ad9910_siggen_command_t){0};
    command.type = AD9910_SIGGEN_COMMAND_SET_MODE;
    command.mode = AD9910_SIGGEN_MODE_RAM_WAVEFORM;
    return one_hmi_dispatch_command(&command);
}

static HAL_StatusTypeDef one_hmi_apply_variables(void)
{
    const int32_t waveform_value =
        g_one_hmi.variables[ONE_HMI_VAR_WAVEFORM].value;
    const int32_t mode_value = g_one_hmi.variables[ONE_HMI_VAR_MODE].value;
    const int32_t profile_value =
        g_one_hmi.variables[ONE_HMI_VAR_PROFILE].value;
    const ad9910_siggen_waveform_t waveform =
        one_hmi_build_waveform(waveform_value);
    const uint8_t profile_index =
        one_hmi_u8_clamp(profile_value,
                         (uint8_t)(AD9910_SIGGEN_RAM_PRESET_COUNT - 1U));
    const uint8_t run_flag =
        one_hmi_u8_clamp(g_one_hmi.variables[ONE_HMI_VAR_RUN_FLAG].value,
                         1U);
    ad9910_siggen_tone_param_t tone;
    HAL_StatusTypeDef status;
    uint8_t values_changed;
    uint8_t use_ram;

    if ((one_hmi_mode_is_valid(mode_value) == 0U) ||
        (one_hmi_waveform_is_valid(waveform_value) == 0U)) {
        g_one_hmi.status.error = ONE_HMI_ERROR_INVALID_VALUE;
        g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_ERROR;
        return HAL_ERROR;
    }

    use_ram = ((mode_value == ONE_HMI_MODE_RAM) ||
               (mode_value == ONE_HMI_MODE_PROFILE) ||
               (waveform != AD9910_SIGGEN_WAVEFORM_SINE)) ?
                  1U :
                  0U;

    tone.frequency_hz = one_hmi_frequency_clamp(
        g_one_hmi.variables[ONE_HMI_VAR_FREQUENCY].value);
    if ((use_ram != 0U) &&
        (tone.frequency_hz > AD9910_SIGGEN_RAM_MAX_WAVE_FREQUENCY_HZ)) {
        tone.frequency_hz = AD9910_SIGGEN_RAM_MAX_WAVE_FREQUENCY_HZ;
    }
    tone.amplitude_percent = one_hmi_u8_clamp(
        g_one_hmi.variables[ONE_HMI_VAR_AMPLITUDE].value,
        ONE_HMI_MAX_AMPLITUDE_PERCENT);
    tone.phase_degrees = one_hmi_phase_clamp(
        g_one_hmi.variables[ONE_HMI_VAR_PHASE].value);

    values_changed = ((g_one_hmi.applied_snapshot_valid == 0U) ||
                      (g_one_hmi.status.frequency_hz != tone.frequency_hz) ||
                      (g_one_hmi.status.amplitude_percent !=
                       tone.amplitude_percent) ||
                      (g_one_hmi.status.phase_degrees !=
                       tone.phase_degrees) ||
                      (g_one_hmi.status.waveform !=
                       (one_hmi_wave_t)waveform_value) ||
                      (g_one_hmi.status.mode !=
                       (one_hmi_mode_t)mode_value) ||
                      (g_one_hmi.status.profile_index != profile_index) ||
                      (g_one_hmi.status.run_flag != run_flag)) ?
                         1U :
                         0U;

    g_one_hmi.status.frequency_hz = tone.frequency_hz;
    g_one_hmi.status.amplitude_percent = tone.amplitude_percent;
    g_one_hmi.status.phase_degrees = tone.phase_degrees;
    g_one_hmi.status.waveform = (one_hmi_wave_t)one_hmi_u8_clamp(
        waveform_value,
        ONE_HMI_WAVE_SAW);
    g_one_hmi.status.mode = (one_hmi_mode_t)mode_value;
    g_one_hmi.status.profile_index = profile_index;
    g_one_hmi.status.run_flag = run_flag;

    if (mode_value == ONE_HMI_MODE_SWEEP) {
        g_one_hmi.status.error = ONE_HMI_ERROR_SWEEP_NOT_SUPPORTED;
        g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_ERROR;
        return HAL_ERROR;
    }

    if (values_changed == 0U) {
        g_one_hmi.status.error = ONE_HMI_ERROR_NONE;
        g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_READY;
        g_one_hmi.status.sync_count++;
        return HAL_OK;
    }

    /* run_flag=0 时保留页面幅值，只向 AD9910 下发 0% 实现静音。 */
    if (run_flag == 0U) {
        tone.amplitude_percent = 0U;
    }

    if (use_ram != 0U) {
        status = one_hmi_apply_ram_waveform(&tone, waveform, profile_index);
    } else {
        status = one_hmi_apply_single_tone(&tone);
    }

    if (status == HAL_OK) {
        g_one_hmi.applied_snapshot_valid = 1U;
        g_one_hmi.status.error = ONE_HMI_ERROR_NONE;
        g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_READY;
        g_one_hmi.status.sync_count++;
    }

    return status;
}

static HAL_StatusTypeDef one_hmi_start_next_query(void)
{
    const char *object_name;
    HAL_StatusTypeDef status;

    if (g_one_hmi.query_index >= ONE_HMI_VAR_COUNT) {
        const uint8_t restart_sync = g_one_hmi.restart_sync_pending;
        HAL_StatusTypeDef apply_status;

        g_one_hmi.pending_sync = 0U;
        g_one_hmi.active_query = 0U;
        g_one_hmi.restart_sync_pending = 0U;
        apply_status = one_hmi_apply_variables();
        if (restart_sync != 0U) {
            g_one_hmi.pending_sync = 1U;
            g_one_hmi.query_index = 0U;
        }
        one_hmi_schedule_next_sync(ONE_HMI_PERIODIC_SYNC_MS);
        return apply_status;
    }

    object_name = g_one_hmi.variables[g_one_hmi.query_index].object_name;
    status = Usart_HMI_Service_GetProperty(object_name, "val");
    g_one_hmi.status.last_hal_status = status;
    if (status == HAL_OK) {
        g_one_hmi.active_query = 1U;
        g_one_hmi.query_deadline_ms =
            HAL_GetTick() + ONE_HMI_QUERY_TIMEOUT_MS;
        g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_QUERYING;
    } else if (status != HAL_BUSY) {
        g_one_hmi.status.error = ONE_HMI_ERROR_HMI_BUSY;
        g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_ERROR;
        one_hmi_schedule_next_sync(ONE_HMI_PERIODIC_SYNC_MS);
    }

    return status;
}

static void one_hmi_handle_number(uint32_t value)
{
    if ((g_one_hmi.active_query == 0U) ||
        (g_one_hmi.query_index >= ONE_HMI_VAR_COUNT)) {
        g_one_hmi.status.ignored_number_count++;
        return;
    }

    g_one_hmi.variables[g_one_hmi.query_index].value = (int32_t)value;
    g_one_hmi.query_index++;
    g_one_hmi.active_query = 0U;
}
#endif

static void one_hmi_waveform_handle_current_page(uint8_t page_id)
{
    g_one_hmi.status.current_page_known = 1U;
    g_one_hmi.status.current_page_id = page_id;

    if (g_one_hmi.status.waveform_state !=
        ONE_HMI_WAVEFORM_STATE_WAIT_PAGE) {
        return;
    }

    if (page_id == TJC_SCREEN_MEASUREMENT_PAGE_ID) {
        one_hmi_waveform_set_state(ONE_HMI_WAVEFORM_STATE_CLEAR);
        g_one_hmi.waveform_deadline_ms =
            HAL_GetTick() + ONE_HMI_WAVEFORM_TRANSFER_TIMEOUT_MS;
    } else {
        one_hmi_waveform_set_state(ONE_HMI_WAVEFORM_STATE_IDLE);
        g_one_hmi.status.waveform_page_skip_count++;
        if (TJC_SCREEN_PLOT_SNAPSHOT_MODE != 0U) {
            g_one_hmi.waveform_snapshot_requested = 0U;
            g_one_hmi.status.waveform_snapshot_pending = 0U;
        } else {
            one_hmi_waveform_schedule_next();
        }
    }
}

static void one_hmi_waveform_handle_ready(void)
{
    if (g_one_hmi.status.waveform_state ==
        ONE_HMI_WAVEFORM_STATE_WAIT_READY) {
        one_hmi_waveform_set_state(ONE_HMI_WAVEFORM_STATE_SEND_DATA);
        g_one_hmi.waveform_deadline_ms =
            HAL_GetTick() + ONE_HMI_WAVEFORM_TRANSFER_TIMEOUT_MS;
    } else {
        g_one_hmi.status.waveform_unexpected_event_count++;
    }
}

static void one_hmi_waveform_handle_finished(void)
{
    if (g_one_hmi.status.waveform_state ==
        ONE_HMI_WAVEFORM_STATE_WAIT_FINISHED) {
        one_hmi_waveform_set_state(ONE_HMI_WAVEFORM_STATE_IDLE);
        g_one_hmi.status.waveform_last_hal_status = HAL_OK;
        g_one_hmi.status.waveform_last_sequence =
            g_one_hmi.waveform_pending_sequence;
        g_one_hmi.plot_last_sequence[g_one_hmi.status.active_plot] =
            g_one_hmi.waveform_pending_sequence;
        g_one_hmi.status.waveform_transfer_count++;
        if ((TJC_SCREEN_PLOT_SNAPSHOT_MODE != 0U) &&
            (g_one_hmi.status.active_plot ==
             ONE_HMI_PLOT_TIME_DOMAIN)) {
            g_one_hmi.status.active_plot = ONE_HMI_PLOT_SPECTRUM;
            g_one_hmi.waveform_next_refresh_ms =
                HAL_GetTick() + ONE_HMI_PLOT_SWITCH_DELAY_MS;
        } else if (TJC_SCREEN_PLOT_SNAPSHOT_MODE != 0U) {
            /* 手动刷新一次包含时域和频谱两次透明传输。 */
            g_one_hmi.waveform_snapshot_requested = 0U;
            g_one_hmi.status.waveform_snapshot_pending = 0U;
            g_one_hmi.status.active_plot = ONE_HMI_PLOT_TIME_DOMAIN;
        } else if (g_one_hmi.status.active_plot ==
                   ONE_HMI_PLOT_TIME_DOMAIN) {
            g_one_hmi.status.active_plot = ONE_HMI_PLOT_SPECTRUM;
            g_one_hmi.waveform_next_refresh_ms =
                HAL_GetTick() + ONE_HMI_PLOT_SWITCH_DELAY_MS;
        } else {
            g_one_hmi.status.active_plot = ONE_HMI_PLOT_TIME_DOMAIN;
            one_hmi_waveform_schedule_next();
        }
    } else {
        g_one_hmi.status.waveform_unexpected_event_count++;
    }
}

static uint8_t one_hmi_waveform_cancel_for_sync(void)
{
    switch (g_one_hmi.status.waveform_state) {
    case ONE_HMI_WAVEFORM_STATE_REQUEST_PAGE:
    case ONE_HMI_WAVEFORM_STATE_WAIT_PAGE:
    case ONE_HMI_WAVEFORM_STATE_CLEAR:
    case ONE_HMI_WAVEFORM_STATE_BEGIN_TRANSFER:
        one_hmi_waveform_set_state(ONE_HMI_WAVEFORM_STATE_IDLE);
        one_hmi_waveform_schedule_next();
        return 1U;

    default:
        return 0U;
    }
}

static HAL_StatusTypeDef one_hmi_waveform_process(void)
{
    HAL_StatusTypeDef status;

    if (TJC_SCREEN_ENABLE_PLOT_TRANSFER == 0U) {
        return HAL_OK;
    }

    switch (g_one_hmi.status.waveform_state) {
    case ONE_HMI_WAVEFORM_STATE_IDLE:
        if ((TJC_SCREEN_PLOT_SNAPSHOT_MODE != 0U) &&
            (g_one_hmi.waveform_snapshot_requested == 0U)) {
            return HAL_OK;
        }
        if ((g_one_hmi.pending_sync != 0U) ||
            (g_one_hmi.active_query != 0U) ||
            (g_one_hmi.measurement_publish_active != 0U) ||
            (one_hmi_time_reached(
                 g_one_hmi.waveform_next_refresh_ms) == 0U)) {
            return HAL_OK;
        }
        if (one_hmi_plot_prepare() == 0U) {
            one_hmi_waveform_schedule_next();
            return HAL_OK;
        }
        one_hmi_waveform_set_state(ONE_HMI_WAVEFORM_STATE_REQUEST_PAGE);
        g_one_hmi.waveform_deadline_ms =
            HAL_GetTick() + ONE_HMI_WAVEFORM_TRANSFER_TIMEOUT_MS;
        return HAL_OK;

    case ONE_HMI_WAVEFORM_STATE_REQUEST_PAGE:
        if (g_one_hmi.pending_sync != 0U) {
            (void)one_hmi_waveform_cancel_for_sync();
            return HAL_OK;
        }
        if (one_hmi_time_reached(g_one_hmi.waveform_deadline_ms) != 0U) {
            return one_hmi_waveform_fail(HAL_TIMEOUT);
        }
        status = Usart_HMI_Service_RequestCurrentPage();
        if (status == HAL_OK) {
            one_hmi_waveform_set_state(ONE_HMI_WAVEFORM_STATE_WAIT_PAGE);
            g_one_hmi.waveform_deadline_ms =
                HAL_GetTick() + ONE_HMI_WAVEFORM_PAGE_TIMEOUT_MS;
        } else if (status != HAL_BUSY) {
            return one_hmi_waveform_fail(status);
        }
        return HAL_OK;

    case ONE_HMI_WAVEFORM_STATE_WAIT_PAGE:
        if (g_one_hmi.pending_sync != 0U) {
            (void)one_hmi_waveform_cancel_for_sync();
            return HAL_OK;
        }
        if (one_hmi_time_reached(g_one_hmi.waveform_deadline_ms) != 0U) {
            return one_hmi_waveform_fail(HAL_TIMEOUT);
        }
        return HAL_OK;

    case ONE_HMI_WAVEFORM_STATE_CLEAR:
        if (g_one_hmi.pending_sync != 0U) {
            (void)one_hmi_waveform_cancel_for_sync();
            return HAL_OK;
        }
        if (one_hmi_time_reached(g_one_hmi.waveform_deadline_ms) != 0U) {
            return one_hmi_waveform_fail(HAL_TIMEOUT);
        }
        status = Usart_HMI_Service_ClearWaveform(
            one_hmi_plot_component_id(),
            TJC_SCREEN_PLOT_CHANNEL);
        if (status == HAL_OK) {
            one_hmi_waveform_set_state(
                ONE_HMI_WAVEFORM_STATE_BEGIN_TRANSFER);
        } else if (status != HAL_BUSY) {
            return one_hmi_waveform_fail(status);
        }
        return HAL_OK;

    case ONE_HMI_WAVEFORM_STATE_BEGIN_TRANSFER:
        if (g_one_hmi.pending_sync != 0U) {
            (void)one_hmi_waveform_cancel_for_sync();
            return HAL_OK;
        }
        if (one_hmi_time_reached(g_one_hmi.waveform_deadline_ms) != 0U) {
            return one_hmi_waveform_fail(HAL_TIMEOUT);
        }
        status = Usart_HMI_Service_BeginWaveformTransfer(
            one_hmi_plot_component_id(),
            TJC_SCREEN_PLOT_CHANNEL,
            TJC_SCREEN_PLOT_POINT_COUNT);
        if (status == HAL_OK) {
            one_hmi_waveform_set_state(ONE_HMI_WAVEFORM_STATE_WAIT_READY);
            g_one_hmi.waveform_deadline_ms =
                HAL_GetTick() + ONE_HMI_WAVEFORM_TRANSFER_TIMEOUT_MS;
        } else if (status != HAL_BUSY) {
            return one_hmi_waveform_fail(status);
        }
        return HAL_OK;

    case ONE_HMI_WAVEFORM_STATE_WAIT_READY:
        if (one_hmi_time_reached(g_one_hmi.waveform_deadline_ms) != 0U) {
            return one_hmi_waveform_fail(HAL_TIMEOUT);
        }
        return HAL_OK;

    case ONE_HMI_WAVEFORM_STATE_SEND_DATA:
        if (one_hmi_time_reached(g_one_hmi.waveform_deadline_ms) != 0U) {
            return one_hmi_waveform_fail(HAL_TIMEOUT);
        }
        status = Usart_HMI_Service_SendTransparentData(
            g_one_hmi.waveform_points,
            TJC_SCREEN_PLOT_POINT_COUNT);
        if (status == HAL_OK) {
            one_hmi_waveform_set_state(
                ONE_HMI_WAVEFORM_STATE_WAIT_FINISHED);
            g_one_hmi.waveform_deadline_ms =
                HAL_GetTick() + ONE_HMI_WAVEFORM_TRANSFER_TIMEOUT_MS;
        } else if (status != HAL_BUSY) {
            return one_hmi_waveform_fail(status);
        }
        return HAL_OK;

    case ONE_HMI_WAVEFORM_STATE_WAIT_FINISHED:
        if (one_hmi_time_reached(g_one_hmi.waveform_deadline_ms) != 0U) {
            return one_hmi_waveform_fail(HAL_TIMEOUT);
        }
        return HAL_OK;

    default:
        return one_hmi_waveform_fail(HAL_ERROR);
    }
}

static void one_hmi_handle_event(const usart_hmi_event_t *event)
{
    HAL_StatusTypeDef touch_status;

    g_one_hmi.status.link_alive = 1U;
    g_one_hmi.status.last_rx_tick_ms = HAL_GetTick();

    switch (event->type) {
    case USART_HMI_EVENT_TOUCH:
        g_one_hmi.status.current_page_known = 1U;
        g_one_hmi.status.current_page_id = event->data.touch.page_id;
        if (event->data.touch.state == USART_HMI_TOUCH_RELEASE) {
            g_one_hmi.status.touch_count++;
            if (event->data.touch.page_id ==
                TJC_SCREEN_MEASUREMENT_PAGE_ID) {
                if (event->data.touch.component_id ==
                    TJC_SCREEN_ONE_CYCLE_BUTTON_ID) {
                    (void)Signal_HMI_App_SetWaveformCycles(1U);
                } else if (event->data.touch.component_id ==
                           TJC_SCREEN_THREE_CYCLE_BUTTON_ID) {
                    (void)Signal_HMI_App_SetWaveformCycles(3U);
                } else if (event->data.touch.component_id ==
                           TJC_SCREEN_SNAPSHOT_BUTTON_COMPONENT_ID) {
                    touch_status =
                        Signal_HMI_App_RequestWaveformSnapshot();
                    if (touch_status == HAL_BUSY) {
                        g_one_hmi.status.waveform_snapshot_busy_count++;
                    } else if (touch_status != HAL_OK) {
                        g_one_hmi.status.waveform_snapshot_rejected_count++;
                    }
                }
            }
#if TJC_SCREEN_ENABLE_GENERATOR_CONTROL
            else if (event->data.touch.page_id ==
                     TJC_SCREEN_GENERATOR_PAGE_ID) {
                (void)Signal_HMI_App_RequestSync();
            }
#endif
        }
        break;

    case USART_HMI_EVENT_CURRENT_PAGE:
        one_hmi_waveform_handle_current_page(
            event->data.current_page.page_id);
        break;

    case USART_HMI_EVENT_STARTUP:
        g_one_hmi.status.startup_count++;
        g_one_hmi.applied_snapshot_valid = 0U;
        one_hmi_waveform_reset();
        (void)Signal_HMI_App_RequestSync();
        break;

    case USART_HMI_EVENT_NUMBER:
#if TJC_SCREEN_ENABLE_GENERATOR_CONTROL
        one_hmi_handle_number(event->data.number.value);
#else
        g_one_hmi.status.ignored_number_count++;
#endif
        break;

    case USART_HMI_EVENT_RETURN_STATUS:
        g_one_hmi.status.last_return_status =
            event->data.return_status.code;
        if (event->data.return_status.code != 0x01U) {
            g_one_hmi.status.return_status_error_count++;
            if (g_one_hmi.active_query != 0U) {
                g_one_hmi.pending_sync = 0U;
                g_one_hmi.active_query = 0U;
                g_one_hmi.restart_sync_pending = 0U;
                g_one_hmi.query_index = 0U;
                g_one_hmi.status.error = ONE_HMI_ERROR_HMI_RESPONSE;
                g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_ERROR;
                g_one_hmi.status.last_hal_status = HAL_ERROR;
                one_hmi_schedule_next_sync(ONE_HMI_PERIODIC_SYNC_MS);
            }
        }
        break;

    case USART_HMI_EVENT_TRANSPARENT_READY:
        one_hmi_waveform_handle_ready();
        break;

    case USART_HMI_EVENT_TRANSPARENT_FINISHED:
        one_hmi_waveform_handle_finished();
        break;

    default:
        break;
    }
}

HAL_StatusTypeDef Signal_HMI_App_Init(UART_HandleTypeDef *uart)
{
    const usart_hmi_service_config_t service_config = {
        .send_bytes = one_hmi_send_bytes,
        .user_context = NULL,
        .async_tx = 1U,
    };
    HAL_StatusTypeDef status;

    if ((uart == NULL) ||
        (uart->Init.BaudRate != TJC_SCREEN_UART_BAUD_RATE)) {
        return HAL_ERROR;
    }

    for (uint8_t index = 0U; index < ONE_HMI_VAR_COUNT; ++index) {
        g_one_hmi.variables[index].object_name =
            g_one_hmi_variable_names[index];
        g_one_hmi.variables[index].value = 0;
    }

    g_one_hmi.pending_sync = 0U;
    g_one_hmi.active_query = 0U;
    g_one_hmi.restart_sync_pending = 0U;
    g_one_hmi.applied_snapshot_valid = 0U;
    g_one_hmi.query_index = 0U;
    g_one_hmi.query_deadline_ms = 0U;
    one_hmi_schedule_next_sync(ONE_HMI_STARTUP_SYNC_DELAY_MS);
    g_one_hmi.observed_rx_restart_count = 0U;
    g_one_hmi.observed_rx_dropped_count = 0U;
    g_one_hmi.status = (signal_hmi_status_t){0};
    g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_IDLE;
    g_one_hmi.status.error = ONE_HMI_ERROR_NONE;
    g_one_hmi.status.last_hal_status = HAL_OK;
    g_one_hmi.status.waveform_last_hal_status = HAL_OK;
    g_one_hmi.status.waveform_cycles = 3U;
    g_one_hmi.status.measurement_self_test_active =
        TJC_SCREEN_ENABLE_MEASUREMENT_SELF_TEST;
    g_one_hmi.measurement_publish_active = 0U;
    g_one_hmi.measurement_field_index = 0U;
    g_one_hmi.measurement_pending_sequence = 0U;
    g_one_hmi.measurement_snapshot =
        (signal_measurement_result_t){0};
    g_one_hmi.measurement_next_refresh_ms =
        HAL_GetTick() + ONE_HMI_STARTUP_SYNC_DELAY_MS;
    one_hmi_waveform_reset();

    status = Usart_HMI_Service_Init(&service_config);
    if (status == HAL_OK) {
        status = TJC_UART_Driver_Init(uart);
    }
    if (status != HAL_OK) {
        g_one_hmi.status.error = ONE_HMI_ERROR_TRANSPORT;
        g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_ERROR;
        g_one_hmi.status.last_hal_status = status;
        return status;
    }

    return HAL_OK;
}

HAL_StatusTypeDef Signal_HMI_App_Process(void)
{
    const tjc_uart_driver_status_t *driver_status;
    uint8_t rx_buffer[ONE_HMI_RX_TRANSFER_BUFFER_SIZE];
    usart_hmi_event_t event;
    HAL_StatusTypeDef result;
    HAL_StatusTypeDef status;
    uint16_t rx_length;

    result = TJC_UART_Driver_Process();
    if (result != HAL_OK) {
        g_one_hmi.status.error = ONE_HMI_ERROR_TRANSPORT;
        g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_ERROR;
        g_one_hmi.status.last_hal_status = result;
        g_one_hmi.status.transport_error_count++;
    }

    if (TJC_UART_Driver_TakeTxComplete() != 0U) {
        Usart_HMI_Service_NotifyTxComplete();
    }

    do {
        rx_length = TJC_UART_Driver_Read(rx_buffer, sizeof(rx_buffer));
        if (rx_length != 0U) {
            status = Usart_HMI_Service_PushRxBytes(rx_buffer, rx_length);
            if (status != HAL_OK) {
                g_one_hmi.status.error = ONE_HMI_ERROR_TRANSPORT;
                g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_ERROR;
                g_one_hmi.status.last_hal_status = status;
                g_one_hmi.status.rx_push_error_count++;
                result = status;
            }
            Usart_HMI_Service_Process();
        }
    } while (rx_length != 0U);

    driver_status = TJC_UART_Driver_GetStatus();
    if ((driver_status->rx_restart_count !=
         g_one_hmi.observed_rx_restart_count) ||
        (driver_status->rx_bytes_dropped !=
         g_one_hmi.observed_rx_dropped_count)) {
        g_one_hmi.observed_rx_restart_count =
            driver_status->rx_restart_count;
        g_one_hmi.observed_rx_dropped_count =
            driver_status->rx_bytes_dropped;
        Usart_HMI_Service_ResetRx();
        g_one_hmi.pending_sync = 1U;
        g_one_hmi.active_query = 0U;
        g_one_hmi.restart_sync_pending = 0U;
        g_one_hmi.applied_snapshot_valid = 0U;
        g_one_hmi.query_index = 0U;
        g_one_hmi.measurement_publish_active = 0U;
        one_hmi_measurement_schedule_next();
        one_hmi_waveform_reset();
        g_one_hmi.status.transport_error_count++;
        (void)Signal_HMI_App_RequestSync();
    }

    Usart_HMI_Service_Process();
    while (Usart_HMI_Service_ReadEvent(&event) != 0U) {
        one_hmi_handle_event(&event);
    }

    if ((g_one_hmi.active_query != 0U) &&
        (one_hmi_time_reached(g_one_hmi.query_deadline_ms) != 0U)) {
        g_one_hmi.pending_sync = g_one_hmi.restart_sync_pending;
        g_one_hmi.active_query = 0U;
        g_one_hmi.restart_sync_pending = 0U;
        g_one_hmi.query_index = 0U;
        g_one_hmi.status.error = ONE_HMI_ERROR_QUERY_TIMEOUT;
        g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_ERROR;
        g_one_hmi.status.last_hal_status = HAL_TIMEOUT;
        g_one_hmi.status.query_timeout_count++;
        one_hmi_schedule_next_sync(ONE_HMI_PERIODIC_SYNC_MS);
        result = HAL_TIMEOUT;
    }

    if ((g_one_hmi.status.link_alive != 0U) &&
        (one_hmi_time_reached(g_one_hmi.status.last_rx_tick_ms +
                              ONE_HMI_LINK_TIMEOUT_MS) != 0U)) {
        g_one_hmi.status.link_alive = 0U;
    }

    if ((g_one_hmi.pending_sync == 0U) &&
        (g_one_hmi.active_query == 0U) &&
        (g_one_hmi.measurement_publish_active == 0U) &&
        (g_one_hmi.status.waveform_state ==
         ONE_HMI_WAVEFORM_STATE_IDLE) &&
        (one_hmi_time_reached(g_one_hmi.next_sync_ms) != 0U)) {
        status = Signal_HMI_App_RequestSync();
        g_one_hmi.status.periodic_sync_count++;
        if ((status != HAL_OK) && (status != HAL_BUSY)) {
            result = status;
        }
    }

#if TJC_SCREEN_ENABLE_GENERATOR_CONTROL
    if ((g_one_hmi.pending_sync != 0U) &&
        (g_one_hmi.active_query == 0U) &&
        (g_one_hmi.measurement_publish_active == 0U) &&
        (g_one_hmi.status.waveform_state ==
         ONE_HMI_WAVEFORM_STATE_IDLE)) {
        status = one_hmi_start_next_query();
        if ((status != HAL_OK) && (status != HAL_BUSY)) {
            result = status;
        }
    }
#endif

    status = one_hmi_measurement_publish_process();
    if ((status != HAL_OK) && (status != HAL_BUSY)) {
        result = status;
    }

    status = one_hmi_waveform_process();
    if ((status != HAL_OK) && (status != HAL_BUSY)) {
        result = status;
    }

    return result;
}

HAL_StatusTypeDef Signal_HMI_App_RequestSync(void)
{
    if (TJC_SCREEN_ENABLE_GENERATOR_CONTROL == 0U) {
        g_one_hmi.pending_sync = 0U;
        g_one_hmi.active_query = 0U;
        g_one_hmi.restart_sync_pending = 0U;
        g_one_hmi.query_index = 0U;
        g_one_hmi.status.error = ONE_HMI_ERROR_NONE;
        g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_IDLE;
        one_hmi_schedule_next_sync(ONE_HMI_PERIODIC_SYNC_MS);
        return HAL_OK;
    }

    if (g_one_hmi.active_query != 0U) {
        g_one_hmi.restart_sync_pending = 1U;
        return HAL_BUSY;
    }

    g_one_hmi.pending_sync = 1U;
    g_one_hmi.restart_sync_pending = 0U;
    g_one_hmi.query_index = 0U;
    g_one_hmi.status.error = ONE_HMI_ERROR_NONE;
    g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_QUERYING;
    one_hmi_schedule_next_sync(ONE_HMI_PERIODIC_SYNC_MS);

    if (g_one_hmi.status.waveform_state !=
        ONE_HMI_WAVEFORM_STATE_IDLE) {
        return HAL_BUSY;
    }

    return HAL_OK;
}

HAL_StatusTypeDef Signal_HMI_App_SetWaveformCycles(
    uint8_t cycle_count)
{
    if ((cycle_count != 1U) && (cycle_count != 3U)) {
        return HAL_ERROR;
    }

    g_one_hmi.status.waveform_cycles = cycle_count;
    g_one_hmi.plot_last_sequence[ONE_HMI_PLOT_TIME_DOMAIN] = 0U;
    if (g_one_hmi.status.waveform_state ==
        ONE_HMI_WAVEFORM_STATE_IDLE) {
        g_one_hmi.status.active_plot = ONE_HMI_PLOT_TIME_DOMAIN;
    }
    g_one_hmi.waveform_next_refresh_ms = HAL_GetTick();
    g_one_hmi.status.measurement_last_sequence = 0U;
    g_one_hmi.measurement_next_refresh_ms = HAL_GetTick();
    return HAL_OK;
}

HAL_StatusTypeDef Signal_HMI_App_RequestWaveformSnapshot(void)
{
    if ((TJC_SCREEN_ENABLE_PLOT_TRANSFER == 0U) ||
        (TJC_SCREEN_TIME_PLOT_COMPONENT_ID ==
         TJC_SCREEN_UNASSIGNED_COMPONENT_ID) ||
        (TJC_SCREEN_SNAPSHOT_BUTTON_COMPONENT_ID ==
         TJC_SCREEN_UNASSIGNED_COMPONENT_ID)) {
        return HAL_ERROR;
    }

    if (g_one_hmi.status.waveform_state !=
        ONE_HMI_WAVEFORM_STATE_IDLE) {
        return HAL_BUSY;
    }

    g_one_hmi.status.waveform_snapshot_request_count++;
    g_one_hmi.status.active_plot = ONE_HMI_PLOT_TIME_DOMAIN;
    g_one_hmi.plot_last_sequence[ONE_HMI_PLOT_TIME_DOMAIN] = 0U;
    g_one_hmi.plot_last_sequence[ONE_HMI_PLOT_SPECTRUM] = 0U;
    g_one_hmi.waveform_next_refresh_ms = HAL_GetTick();

    if (TJC_SCREEN_PLOT_SNAPSHOT_MODE != 0U) {
        g_one_hmi.waveform_snapshot_requested = 1U;
        g_one_hmi.status.waveform_snapshot_pending = 1U;
    }
    return HAL_OK;
}

const signal_hmi_status_t *Signal_HMI_App_GetStatus(void)
{
    return &g_one_hmi.status;
}
