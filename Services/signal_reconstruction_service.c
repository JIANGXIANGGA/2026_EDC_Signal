#include "signal_reconstruction_service.h"

#include <math.h>
#include <stddef.h>

#define SIGNAL_RECONSTRUCTION_TWO_PI 6.28318530717958647692f
#define SIGNAL_RECONSTRUCTION_NORMALIZE_INTERVAL 128U

static uint8_t signal_reconstruction_measurement_valid(
    const signal_measurement_result_t *measurement)
{
    if ((measurement == NULL) || (measurement->result_ready == 0U) ||
        (measurement->component_count == 0U) ||
        (measurement->component_count > SIGNAL_MEASUREMENT_COMPONENT_COUNT)) {
        return 0U;
    }

    for (uint8_t index = 0U;
         index < measurement->component_count;
         ++index) {
        const signal_measurement_component_t *component =
            &measurement->components[index];

        if ((component->valid == 0U) ||
            (component->harmonic_order == 0U) ||
            (isfinite(component->amplitude_mv) == 0) ||
            (component->amplitude_mv < 0.0f)) {
            return 0U;
        }
    }
    return 1U;
}

static void signal_reconstruction_normalize(float *sine, float *cosine)
{
    const float magnitude = sqrtf((*sine * *sine) + (*cosine * *cosine));

    if (magnitude > 0.0f) {
        *sine /= magnitude;
        *cosine /= magnitude;
    }
}

uint8_t Signal_Reconstruction_Service_Generate(
    const signal_measurement_result_t *measurement,
    uint8_t cycle_count,
    float *samples_mv,
    uint32_t point_count,
    float *minimum_mv,
    float *maximum_mv)
{
    float sine[SIGNAL_MEASUREMENT_COMPONENT_COUNT] = {0.0f};
    float cosine[SIGNAL_MEASUREMENT_COMPONENT_COUNT] = {
        1.0f, 1.0f, 1.0f
    };
    float step_sine[SIGNAL_MEASUREMENT_COMPONENT_COUNT] = {0.0f};
    float step_cosine[SIGNAL_MEASUREMENT_COMPONENT_COUNT] = {0.0f};
    float minimum = 0.0f;
    float maximum = 0.0f;

    if ((signal_reconstruction_measurement_valid(measurement) == 0U) ||
        (cycle_count == 0U) || (point_count < 2U) ||
        ((minimum_mv == NULL) && (maximum_mv == NULL) &&
         (samples_mv == NULL))) {
        return 0U;
    }

    for (uint8_t index = 0U;
         index < measurement->component_count;
         ++index) {
        const float step =
            (SIGNAL_RECONSTRUCTION_TWO_PI *
             (float)measurement->components[index].harmonic_order *
             (float)cycle_count) /
            (float)(point_count - 1U);

        step_sine[index] = sinf(step);
        step_cosine[index] = cosf(step);
    }

    for (uint32_t point = 0U; point < point_count; ++point) {
        float value = 0.0f;

        for (uint8_t index = 0U;
             index < measurement->component_count;
             ++index) {
            value += measurement->components[index].amplitude_mv * sine[index];
        }
        if (samples_mv != NULL) {
            samples_mv[point] = value;
        }
        if (value < minimum) {
            minimum = value;
        }
        if (value > maximum) {
            maximum = value;
        }

        for (uint8_t index = 0U;
             index < measurement->component_count;
             ++index) {
            const float next_sine =
                (sine[index] * step_cosine[index]) +
                (cosine[index] * step_sine[index]);
            const float next_cosine =
                (cosine[index] * step_cosine[index]) -
                (sine[index] * step_sine[index]);

            sine[index] = next_sine;
            cosine[index] = next_cosine;
            if (((point + 1U) % SIGNAL_RECONSTRUCTION_NORMALIZE_INTERVAL) ==
                0U) {
                signal_reconstruction_normalize(&sine[index], &cosine[index]);
            }
        }
    }

    if (minimum_mv != NULL) {
        *minimum_mv = minimum;
    }
    if (maximum_mv != NULL) {
        *maximum_mv = maximum;
    }
    return 1U;
}
