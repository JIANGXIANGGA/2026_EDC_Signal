#include "ad9910_sweep_planner.h"

#include <stddef.h>

#include "ad9910.h"

#define AD9910_SWEEP_RATE_MIN 1U
#define AD9910_SWEEP_RATE_MAX 65535U
#define AD9910_NANOSECONDS_PER_MILLISECOND 1000000ULL

typedef struct {
    uint32_t step_hz;
    uint16_t rate;
    uint32_t step_count;
    uint64_t actual_time_us;
} ad9910_sweep_direction_plan_t;

static uint64_t ad9910_div_round_up_u64(uint64_t numerator,
                                        uint64_t denominator)
{
    return (numerator + denominator - 1ULL) / denominator;
}

static uint32_t ad9910_div_round_up_u32(uint32_t numerator,
                                        uint32_t denominator)
{
    return (numerator + denominator - 1U) / denominator;
}

static uint64_t ad9910_abs_diff_u64(uint64_t first, uint64_t second)
{
    return (first >= second) ? (first - second) : (second - first);
}

static uint8_t ad9910_sweep_direction_candidate_is_better(
    const ad9910_sweep_direction_plan_t *candidate,
    const ad9910_sweep_direction_plan_t *best,
    uint64_t requested_time_us)
{
    uint64_t candidate_error;
    uint64_t best_error;

    candidate_error = ad9910_abs_diff_u64(candidate->actual_time_us,
                                          requested_time_us);
    best_error = ad9910_abs_diff_u64(best->actual_time_us,
                                     requested_time_us);

    if (candidate_error != best_error) {
        return (candidate_error < best_error) ? 1U : 0U;
    }

    return (candidate->step_count > best->step_count) ? 1U : 0U;
}

static uint8_t ad9910_sweep_direction_build_candidate(
    uint32_t span_hz,
    uint32_t step_hz,
    uint64_t requested_time_ns,
    ad9910_sweep_direction_plan_t *candidate)
{
    uint64_t rate_denominator;
    uint64_t rounded_rate;
    uint64_t actual_time_ns;

    if ((step_hz == 0U) || (candidate == NULL)) {
        return 0U;
    }

    candidate->step_count = ad9910_div_round_up_u32(span_hz, step_hz);
    if (candidate->step_count == 0U) {
        return 0U;
    }

    rate_denominator = 4ULL * (uint64_t)candidate->step_count;
    rounded_rate = (requested_time_ns + (rate_denominator / 2ULL)) /
                   rate_denominator;
    if ((rounded_rate < AD9910_SWEEP_RATE_MIN) ||
        (rounded_rate > AD9910_SWEEP_RATE_MAX)) {
        return 0U;
    }

    actual_time_ns = rate_denominator * rounded_rate;
    candidate->step_hz = step_hz;
    candidate->rate = (uint16_t)rounded_rate;
    candidate->actual_time_us = (actual_time_ns + 500ULL) / 1000ULL;

    return 1U;
}

static HAL_StatusTypeDef ad9910_sweep_planner_build_direction(
    uint32_t span_hz,
    uint32_t time_ms,
    uint32_t target_steps,
    ad9910_sweep_direction_plan_t *direction_plan)
{
    uint64_t requested_time_ns;
    uint64_t requested_time_us;
    uint64_t minimum_steps;
    uint64_t maximum_steps;
    uint64_t desired_steps;
    uint64_t base_step;
    uint32_t candidates[8];
    uint32_t candidate_count = 0U;
    uint8_t best_valid = 0U;

    if ((span_hz == 0U) || (time_ms == 0U) || (direction_plan == NULL)) {
        return HAL_ERROR;
    }

    requested_time_ns = (uint64_t)time_ms *
                        AD9910_NANOSECONDS_PER_MILLISECOND;
    requested_time_us = (uint64_t)time_ms * 1000ULL;

    minimum_steps = ad9910_div_round_up_u64(
        requested_time_ns,
        4ULL * AD9910_SWEEP_RATE_MAX);
    maximum_steps = requested_time_ns / (4ULL * AD9910_SWEEP_RATE_MIN);
    if (maximum_steps > span_hz) {
        maximum_steps = span_hz;
    }

    if ((minimum_steps == 0ULL) || (minimum_steps > maximum_steps)) {
        return HAL_ERROR;
    }

    desired_steps = (target_steps == 0U) ?
                        AD9910_SWEEP_PLANNER_DEFAULT_STEPS : target_steps;
    if (desired_steps < minimum_steps) {
        desired_steps = minimum_steps;
    }
    if (desired_steps > maximum_steps) {
        desired_steps = maximum_steps;
    }

    base_step = ad9910_div_round_up_u64(span_hz, desired_steps);
    candidates[candidate_count++] = (uint32_t)base_step;
    if (base_step > 1ULL) {
        candidates[candidate_count++] = (uint32_t)(base_step - 1ULL);
    }
    if (base_step < span_hz) {
        candidates[candidate_count++] = (uint32_t)(base_step + 1ULL);
    }

    base_step = ad9910_div_round_up_u64(span_hz, minimum_steps);
    candidates[candidate_count++] = (uint32_t)base_step;
    if (base_step > 1ULL) {
        candidates[candidate_count++] = (uint32_t)(base_step - 1ULL);
    }

    base_step = ad9910_div_round_up_u64(span_hz, maximum_steps);
    candidates[candidate_count++] = (uint32_t)base_step;
    if (base_step < span_hz) {
        candidates[candidate_count++] = (uint32_t)(base_step + 1ULL);
    }
    candidates[candidate_count++] = 1U;

    for (uint32_t index = 0U; index < candidate_count; ++index) {
        ad9910_sweep_direction_plan_t candidate;

        if (ad9910_sweep_direction_build_candidate(span_hz,
                                                    candidates[index],
                                                    requested_time_ns,
                                                    &candidate) == 0U) {
            continue;
        }

        if ((best_valid == 0U) ||
            (ad9910_sweep_direction_candidate_is_better(
                 &candidate,
                 direction_plan,
                 requested_time_us) != 0U)) {
            *direction_plan = candidate;
            best_valid = 1U;
        }
    }

    return (best_valid != 0U) ? HAL_OK : HAL_ERROR;
}

HAL_StatusTypeDef AD9910_SweepPlanner_Create(uint32_t start_frequency_hz,
                                              uint32_t stop_frequency_hz,
                                              uint32_t sweep_time_ms,
                                              uint32_t return_time_ms,
                                              uint32_t target_steps,
                                              ad9910_sweep_plan_t *plan)
{
    ad9910_sweep_direction_plan_t positive_plan;
    ad9910_sweep_direction_plan_t negative_plan;
    uint32_t span_hz;

    if ((plan == NULL) ||
        (start_frequency_hz >= stop_frequency_hz) ||
        (stop_frequency_hz > AD9910_MAX_FREQUENCY_HZ)) {
        return HAL_ERROR;
    }

    span_hz = stop_frequency_hz - start_frequency_hz;
    if (ad9910_sweep_planner_build_direction(span_hz,
                                              sweep_time_ms,
                                              target_steps,
                                              &positive_plan) != HAL_OK) {
        return HAL_ERROR;
    }

    if (ad9910_sweep_planner_build_direction(span_hz,
                                              return_time_ms,
                                              target_steps,
                                              &negative_plan) != HAL_OK) {
        return HAL_ERROR;
    }

    plan->register_config.lower_frequency_hz = start_frequency_hz;
    plan->register_config.upper_frequency_hz = stop_frequency_hz;
    plan->register_config.positive_step_hz = positive_plan.step_hz;
    plan->register_config.negative_step_hz = negative_plan.step_hz;
    plan->register_config.positive_rate = positive_plan.rate;
    plan->register_config.negative_rate = negative_plan.rate;
    plan->positive_step_count = positive_plan.step_count;
    plan->negative_step_count = negative_plan.step_count;
    plan->actual_sweep_time_us = positive_plan.actual_time_us;
    plan->actual_return_time_us = negative_plan.actual_time_us;

    return HAL_OK;
}
