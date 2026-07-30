#include "signal_measurement_service.h"

#include <math.h>
#include <stddef.h>

#define SIGNAL_MEASUREMENT_ADC_MAX_CODE 4095.0f
#define SIGNAL_MEASUREMENT_MIN_VALID_P2P_MV 20.0f
#define SIGNAL_MEASUREMENT_HARMONIC_TOLERANCE_HZ 1000.0f
#define SIGNAL_MEASUREMENT_HISTORY_FREQUENCY_RESET_HZ 1500.0f

typedef struct {
    uint8_t initialized;
    uint32_t observed_analysis_count;
    signal_measurement_calibration_t calibration;
    signal_measurement_result_t result;
    signal_measurement_result_t
        history[SIGNAL_MEASUREMENT_AVERAGING_FRAME_COUNT];
    uint8_t history_count;
    uint8_t history_write_index;
} signal_measurement_context_t;

static signal_measurement_context_t g_signal_measurement;

static void signal_measurement_sort_values(float *values, uint8_t count)
{
    for (uint8_t index = 1U; index < count; ++index) {
        const float value = values[index];
        uint8_t position = index;

        while ((position > 0U) && (value < values[position - 1U])) {
            values[position] = values[position - 1U];
            --position;
        }
        values[position] = value;
    }
}

static float signal_measurement_robust_average(float *values,
                                                uint8_t count,
                                                float *spread)
{
    uint8_t first = 0U;
    uint8_t last = count;
    float sum = 0.0f;

    if (count == 0U) {
        if (spread != NULL) {
            *spread = 0.0f;
        }
        return 0.0f;
    }

    signal_measurement_sort_values(values, count);
    if (spread != NULL) {
        *spread = values[count - 1U] - values[0U];
    }

    /* 五帧以上去掉单个最大值和最小值，再对其余数据求均值。 */
    if (count >= 5U) {
        first = 1U;
        last = count - 1U;
    }
    for (uint8_t index = first; index < last; ++index) {
        sum += values[index];
    }
    return sum / (float)(last - first);
}

static uint8_t signal_measurement_history_compatible(
    const signal_measurement_result_t *sample)
{
    const signal_measurement_result_t *previous;
    uint8_t previous_index;

    if (g_signal_measurement.history_count == 0U) {
        return 1U;
    }

    previous_index = (g_signal_measurement.history_write_index == 0U) ?
                         (SIGNAL_MEASUREMENT_AVERAGING_FRAME_COUNT - 1U) :
                         (g_signal_measurement.history_write_index - 1U);
    previous = &g_signal_measurement.history[previous_index];
    if (sample->component_count != previous->component_count) {
        return 0U;
    }

    for (uint8_t index = 0U; index < sample->component_count; ++index) {
        if ((sample->components[index].harmonic_order !=
             previous->components[index].harmonic_order) ||
            (fabsf(sample->components[index].frequency_hz -
                   previous->components[index].frequency_hz) >
             SIGNAL_MEASUREMENT_HISTORY_FREQUENCY_RESET_HZ)) {
            return 0U;
        }
    }
    return 1U;
}

static void signal_measurement_push_history(
    const signal_measurement_result_t *sample)
{
    if (signal_measurement_history_compatible(sample) == 0U) {
        g_signal_measurement.history_count = 0U;
        g_signal_measurement.history_write_index = 0U;
    }

    g_signal_measurement.history[g_signal_measurement.history_write_index] =
        *sample;
    g_signal_measurement.history_write_index =
        (uint8_t)((g_signal_measurement.history_write_index + 1U) %
                  SIGNAL_MEASUREMENT_AVERAGING_FRAME_COUNT);
    if (g_signal_measurement.history_count <
        SIGNAL_MEASUREMENT_AVERAGING_FRAME_COUNT) {
        g_signal_measurement.history_count++;
    }
}

static void signal_measurement_aggregate_history(
    const signal_measurement_result_t *latest,
    signal_measurement_result_t *output)
{
    float values[SIGNAL_MEASUREMENT_AVERAGING_FRAME_COUNT];
    uint8_t valid_count = 0U;

    *output = *latest;
    output->averaging_count = g_signal_measurement.history_count;

    for (uint8_t frame = 0U;
         frame < g_signal_measurement.history_count;
         ++frame) {
        const signal_measurement_result_t *history =
            &g_signal_measurement.history[frame];
        values[frame] = history->peak_to_peak_mv;
        if (history->signal_valid != 0U) {
            valid_count++;
        }
    }
    output->peak_to_peak_mv = signal_measurement_robust_average(
        values,
        g_signal_measurement.history_count,
        &output->peak_to_peak_spread_mv);

    for (uint8_t frame = 0U;
         frame < g_signal_measurement.history_count;
         ++frame) {
        values[frame] = g_signal_measurement.history[frame].raw_rms_mv;
    }
    output->raw_rms_mv = signal_measurement_robust_average(
        values, g_signal_measurement.history_count, NULL);
    output->true_rms_mv = output->raw_rms_mv;

    output->max_component_spread_mv = 0.0f;
    for (uint8_t component = 0U;
         component < output->component_count;
         ++component) {
        float spread;

        for (uint8_t frame = 0U;
             frame < g_signal_measurement.history_count;
             ++frame) {
            values[frame] = g_signal_measurement
                                .history[frame]
                                .components[component]
                                .frequency_hz;
        }
        output->components[component].frequency_hz =
            signal_measurement_robust_average(
                values, g_signal_measurement.history_count, NULL);

        for (uint8_t frame = 0U;
             frame < g_signal_measurement.history_count;
             ++frame) {
            values[frame] = g_signal_measurement
                                .history[frame]
                                .components[component]
                                .amplitude_mv;
        }
        output->components[component].amplitude_mv =
            signal_measurement_robust_average(
                values, g_signal_measurement.history_count, &spread);
        if (spread > output->max_component_spread_mv) {
            output->max_component_spread_mv = spread;
        }
    }

    output->fundamental_frequency_hz =
        (output->component_count > 0U) ?
            output->components[0].frequency_hz :
            0.0f;
    output->signal_valid =
        ((valid_count * 2U) >= g_signal_measurement.history_count) ? 1U : 0U;
}

static uint8_t signal_measurement_calibration_valid(
    const signal_measurement_calibration_t *calibration)
{
    if ((calibration == NULL) ||
        (isfinite(calibration->input_mv_per_code) == 0) ||
        (isfinite(calibration->peak_to_peak_gain) == 0) ||
        (isfinite(calibration->rms_gain) == 0) ||
        (isfinite(calibration->spectrum_gain) == 0) ||
        (calibration->input_mv_per_code <= 0.0f) ||
        (calibration->peak_to_peak_gain <= 0.0f) ||
        (calibration->rms_gain <= 0.0f) ||
        (calibration->spectrum_gain <= 0.0f) ||
        (calibration->response_point_count >
         SIGNAL_MEASUREMENT_RESPONSE_POINT_COUNT)) {
        return 0U;
    }

    for (uint8_t index = 0U;
         index < calibration->response_point_count;
         ++index) {
        if ((isfinite(calibration->response[index].correction_gain) == 0) ||
            (calibration->response[index].correction_gain <= 0.0f) ||
            ((index > 0U) &&
             (calibration->response[index].frequency_hz <=
              calibration->response[index - 1U].frequency_hz))) {
            return 0U;
        }
    }

    return 1U;
}

static float signal_measurement_response_gain(float frequency_hz)
{
    const signal_measurement_calibration_t *calibration =
        &g_signal_measurement.calibration;

    if (calibration->response_point_count == 0U) {
        return 1.0f;
    }
    if (frequency_hz <=
        (float)calibration->response[0].frequency_hz) {
        return calibration->response[0].correction_gain;
    }

    for (uint8_t index = 1U;
         index < calibration->response_point_count;
         ++index) {
        const signal_measurement_response_point_t *lower =
            &calibration->response[index - 1U];
        const signal_measurement_response_point_t *upper =
            &calibration->response[index];

        if (frequency_hz <= (float)upper->frequency_hz) {
            const float span =
                (float)(upper->frequency_hz - lower->frequency_hz);
            const float ratio =
                (frequency_hz - (float)lower->frequency_hz) / span;
            return lower->correction_gain +
                   ((upper->correction_gain - lower->correction_gain) *
                    ratio);
        }
    }

    return calibration
        ->response[calibration->response_point_count - 1U]
        .correction_gain;
}

static uint8_t signal_measurement_harmonic_order(float frequency_hz,
                                                 float fundamental_hz)
{
    float ratio;
    uint32_t order;

    if ((frequency_hz <= 0.0f) || (fundamental_hz <= 0.0f)) {
        return 0U;
    }

    ratio = frequency_hz / fundamental_hz;
    order = (uint32_t)(ratio + 0.5f);
    if ((order == 0U) || (order > UINT8_MAX)) {
        return 0U;
    }

    return (uint8_t)order;
}

static uint8_t signal_measurement_matches_harmonic(
    float frequency_hz,
    float fundamental_hz,
    float bin_resolution_hz,
    uint8_t *harmonic_order)
{
    const uint8_t order = signal_measurement_harmonic_order(
        frequency_hz, fundamental_hz);
    float tolerance_hz = SIGNAL_MEASUREMENT_HARMONIC_TOLERANCE_HZ;
    float error_hz;

    if (order == 0U) {
        return 0U;
    }
    if (((float)order * fundamental_hz) >
        (FFT_MAX_FREQUENCY_HZ + FFT_FREQUENCY_RANGE_TOLERANCE_HZ)) {
        return 0U;
    }
    if ((2.0f * bin_resolution_hz) > tolerance_hz) {
        tolerance_hz = 2.0f * bin_resolution_hz;
    }

    error_hz = fabsf(frequency_hz -
                     ((float)order * fundamental_hz));
    if (error_hz > tolerance_hz) {
        return 0U;
    }

    if (harmonic_order != NULL) {
        *harmonic_order = order;
    }
    return 1U;
}

static uint8_t signal_measurement_select_harmonic_family(
    const waveform_analyzer_result_t *analysis,
    uint8_t *selected_indices)
{
    uint8_t best_base = 0U;
    uint8_t best_count = 0U;
    float best_score = 0.0f;

    if ((analysis == NULL) || (selected_indices == NULL) ||
        (analysis->peak_count == 0U)) {
        return 0U;
    }

    for (uint8_t base = 0U; base < analysis->peak_count; ++base) {
        uint8_t count = 0U;
        float score = 0.0f;

        for (uint8_t candidate = base;
             candidate < analysis->peak_count;
             ++candidate) {
            if (signal_measurement_matches_harmonic(
                    analysis->peak_frequencies_hz[candidate],
                    analysis->peak_frequencies_hz[base],
                    analysis->bin_resolution_hz,
                    NULL) != 0U) {
                count++;
                score += analysis->peak_amplitudes_code[candidate];
            }
        }

        if ((count > best_count) ||
            ((count == best_count) && (score > best_score))) {
            best_base = base;
            best_count = count;
            best_score = score;
        }
    }

    if (best_count <= 1U) {
        float strongest = 0.0f;
        for (uint8_t index = 0U; index < analysis->peak_count; ++index) {
            if (analysis->peak_amplitudes_code[index] > strongest) {
                strongest = analysis->peak_amplitudes_code[index];
                best_base = index;
            }
        }
    }

    selected_indices[0] = best_base;
    best_count = 1U;
    while (best_count < SIGNAL_MEASUREMENT_COMPONENT_COUNT) {
        uint8_t strongest_index = analysis->peak_count;
        float strongest_amplitude = 0.0f;

        for (uint8_t candidate = (uint8_t)(best_base + 1U);
             candidate < analysis->peak_count;
             ++candidate) {
            uint8_t already_selected = 0U;

            for (uint8_t selected = 0U;
                 selected < best_count;
                 ++selected) {
                if (selected_indices[selected] == candidate) {
                    already_selected = 1U;
                    break;
                }
            }
            if ((already_selected == 0U) &&
                (analysis->peak_amplitudes_code[candidate] >
                 strongest_amplitude) &&
                (signal_measurement_matches_harmonic(
                     analysis->peak_frequencies_hz[candidate],
                     analysis->peak_frequencies_hz[best_base],
                     analysis->bin_resolution_hz,
                     NULL) != 0U)) {
                strongest_index = candidate;
                strongest_amplitude =
                    analysis->peak_amplitudes_code[candidate];
            }
        }

        if (strongest_index >= analysis->peak_count) {
            break;
        }
        selected_indices[best_count] = strongest_index;
        best_count++;
    }

    /* 输出仍按频率排列，便于第一项固定表示基波。 */
    for (uint8_t index = 1U; index < best_count; ++index) {
        uint8_t move = index;
        while ((move > 0U) &&
               (selected_indices[move] < selected_indices[move - 1U])) {
            const uint8_t temporary = selected_indices[move - 1U];
            selected_indices[move - 1U] = selected_indices[move];
            selected_indices[move] = temporary;
            --move;
        }
    }

    return best_count;
}

void Signal_Measurement_Service_GetDefaultCalibration(
    signal_measurement_calibration_t *calibration)
{
    if (calibration == NULL) {
        return;
    }

    *calibration = (signal_measurement_calibration_t){0};
    calibration->input_mv_per_code =
        SIGNAL_MEASUREMENT_DEFAULT_ADC_REFERENCE_MV /
        (SIGNAL_MEASUREMENT_ADC_MAX_CODE *
         SIGNAL_MEASUREMENT_DEFAULT_FRONT_END_GAIN);
    calibration->peak_to_peak_gain = 1.0f;
    calibration->rms_gain = 1.0f;
    calibration->spectrum_gain = 1.0f;
}

HAL_StatusTypeDef Signal_Measurement_Service_SetCalibration(
    const signal_measurement_calibration_t *calibration)
{
    if (signal_measurement_calibration_valid(calibration) == 0U) {
        return HAL_ERROR;
    }

    g_signal_measurement.calibration = *calibration;
    return HAL_OK;
}

HAL_StatusTypeDef Signal_Measurement_Service_Init(
    const signal_measurement_calibration_t *calibration)
{
    signal_measurement_calibration_t default_calibration;
    HAL_StatusTypeDef status;

    g_signal_measurement = (signal_measurement_context_t){0};
    if (calibration == NULL) {
        Signal_Measurement_Service_GetDefaultCalibration(
            &default_calibration);
        calibration = &default_calibration;
    }

    status = Signal_Measurement_Service_SetCalibration(calibration);
    if (status != HAL_OK) {
        return status;
    }

    g_signal_measurement.initialized = 1U;
    g_signal_measurement.result.initialized = 1U;
    return HAL_OK;
}

HAL_StatusTypeDef Signal_Measurement_Service_Process(
    const waveform_analyzer_result_t *analysis)
{
    signal_measurement_result_t next = {0};
    uint8_t component_count;
    uint8_t selected_indices[SIGNAL_MEASUREMENT_COMPONENT_COUNT] = {0U};

    if ((g_signal_measurement.initialized == 0U) ||
        (analysis == NULL) || (analysis->result_ready == 0U)) {
        return HAL_ERROR;
    }
    if (analysis->analysis_count ==
        g_signal_measurement.observed_analysis_count) {
        return HAL_OK;
    }

    component_count = signal_measurement_select_harmonic_family(
        analysis, selected_indices);

    next.initialized = 1U;
    next.result_ready = 1U;
    next.component_count = component_count;
    next.measurement_count =
        g_signal_measurement.result.measurement_count + 1U;
    next.clipped = ((analysis->clipped_low != 0U) ||
                    (analysis->clipped_high != 0U)) ?
                       1U :
                       0U;
    next.peak_to_peak_mv =
        (float)analysis->peak_to_peak_code *
        g_signal_measurement.calibration.input_mv_per_code *
        g_signal_measurement.calibration.peak_to_peak_gain;
    next.raw_rms_mv =
        analysis->rms_code *
        g_signal_measurement.calibration.input_mv_per_code *
        g_signal_measurement.calibration.rms_gain;

    for (uint8_t index = 0U; index < component_count; ++index) {
        signal_measurement_component_t *component =
            &next.components[index];
        const uint8_t analysis_index = selected_indices[index];
        const float frequency_hz =
            analysis->peak_frequencies_hz[analysis_index];
        const float amplitude_mv =
            analysis->peak_amplitudes_code[analysis_index] *
            g_signal_measurement.calibration.input_mv_per_code *
            g_signal_measurement.calibration.spectrum_gain *
            signal_measurement_response_gain(frequency_hz);

        component->valid = 1U;
        component->frequency_hz = frequency_hz;
        component->amplitude_mv = amplitude_mv;
    }

    if (component_count > 0U) {
        next.fundamental_frequency_hz =
            next.components[0].frequency_hz;
        for (uint8_t index = 0U; index < component_count; ++index) {
            next.components[index].harmonic_order =
                signal_measurement_harmonic_order(
                    next.components[index].frequency_hz,
                    next.fundamental_frequency_hz);
        }
    }
    next.true_rms_mv = next.raw_rms_mv;

    next.signal_valid =
        ((next.peak_to_peak_mv >=
          SIGNAL_MEASUREMENT_MIN_VALID_P2P_MV) &&
         (component_count >= 2U) && (next.clipped == 0U)) ?
            1U :
            0U;

    signal_measurement_push_history(&next);
    signal_measurement_aggregate_history(
        &next, &g_signal_measurement.result);
    g_signal_measurement.observed_analysis_count = analysis->analysis_count;
    return HAL_OK;
}

const signal_measurement_calibration_t *
Signal_Measurement_Service_GetCalibration(void)
{
    return &g_signal_measurement.calibration;
}

const signal_measurement_result_t *Signal_Measurement_Service_GetResult(void)
{
    return &g_signal_measurement.result;
}
