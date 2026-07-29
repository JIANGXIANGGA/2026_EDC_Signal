#include "signal_measurement_service.h"

#include <math.h>
#include <stddef.h>

#define SIGNAL_MEASUREMENT_ADC_MAX_CODE 4095.0f
#define SIGNAL_MEASUREMENT_MIN_VALID_P2P_MV 20.0f
#define SIGNAL_MEASUREMENT_SMOOTHING_ALPHA 0.25f
#define SIGNAL_MEASUREMENT_HARMONIC_TOLERANCE_HZ 1000.0f

typedef struct {
    uint8_t initialized;
    uint32_t observed_analysis_count;
    signal_measurement_calibration_t calibration;
    signal_measurement_result_t result;
} signal_measurement_context_t;

static signal_measurement_context_t g_signal_measurement;

static uint8_t signal_measurement_calibration_valid(
    const signal_measurement_calibration_t *calibration)
{
    if ((calibration == NULL) ||
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
        if ((calibration->response[index].correction_gain <= 0.0f) ||
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

static float signal_measurement_filter(float previous,
                                       float current,
                                       uint8_t reset)
{
    if (reset != 0U) {
        return current;
    }

    return previous +
           (SIGNAL_MEASUREMENT_SMOOTHING_ALPHA * (current - previous));
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
    float spectrum_square_sum = 0.0f;
    uint8_t reset_filter;
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
    reset_filter = ((g_signal_measurement.result.result_ready == 0U) ||
                    (g_signal_measurement.result.component_count !=
                     component_count)) ?
                       1U :
                       0U;

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
        spectrum_square_sum += amplitude_mv * amplitude_mv;
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
        next.true_rms_mv =
            sqrtf(spectrum_square_sum * 0.5f) *
            g_signal_measurement.calibration.rms_gain;
    } else {
        next.true_rms_mv = next.raw_rms_mv;
    }

    next.signal_valid =
        ((next.peak_to_peak_mv >=
          SIGNAL_MEASUREMENT_MIN_VALID_P2P_MV) &&
         (component_count > 0U) && (next.clipped == 0U)) ?
            1U :
            0U;

    next.peak_to_peak_mv = signal_measurement_filter(
        g_signal_measurement.result.peak_to_peak_mv,
        next.peak_to_peak_mv,
        reset_filter);
    next.true_rms_mv = signal_measurement_filter(
        g_signal_measurement.result.true_rms_mv,
        next.true_rms_mv,
        reset_filter);
    next.raw_rms_mv = signal_measurement_filter(
        g_signal_measurement.result.raw_rms_mv,
        next.raw_rms_mv,
        reset_filter);
    next.fundamental_frequency_hz = signal_measurement_filter(
        g_signal_measurement.result.fundamental_frequency_hz,
        next.fundamental_frequency_hz,
        reset_filter);
    for (uint8_t index = 0U; index < component_count; ++index) {
        next.components[index].frequency_hz = signal_measurement_filter(
            g_signal_measurement.result.components[index].frequency_hz,
            next.components[index].frequency_hz,
            reset_filter);
        next.components[index].amplitude_mv = signal_measurement_filter(
            g_signal_measurement.result.components[index].amplitude_mv,
            next.components[index].amplitude_mv,
            reset_filter);
    }

    g_signal_measurement.observed_analysis_count =
        analysis->analysis_count;
    g_signal_measurement.result = next;
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
