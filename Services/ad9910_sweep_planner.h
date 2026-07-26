#ifndef AD9910_SWEEP_PLANNER_H
#define AD9910_SWEEP_PLANNER_H

#include <stdint.h>

#include "ad9910_service.h"

#define AD9910_SWEEP_PLANNER_DEFAULT_STEPS 10000U

typedef struct {
    ad9910_frequency_sweep_config_t register_config;
    uint32_t positive_step_count;
    uint32_t negative_step_count;
    uint64_t actual_sweep_time_us;
    uint64_t actual_return_time_us;
} ad9910_sweep_plan_t;

HAL_StatusTypeDef AD9910_SweepPlanner_Create(uint32_t start_frequency_hz,
                                              uint32_t stop_frequency_hz,
                                              uint32_t sweep_time_ms,
                                              uint32_t return_time_ms,
                                              uint32_t target_steps,
                                              ad9910_sweep_plan_t *plan);

#endif
