#include "ad9910_demo_app.h"

#include <stddef.h>

#include "ad9910.h"
#include "ad9910_demo_presets.h"
#include "ad9910_sweep_planner.h"

typedef struct {
    ad9910_demo_config_t config;
    ad9910_sweep_plan_t sweep_plan;
    ad9910_demo_preset_id_t preset;
    ad9910_demo_run_state_t run_state;
    ad9910_demo_error_t error;
    uint32_t deadline_ms;
    uint32_t observed_ramp_event_count;
    uint8_t service_cycle_started;
} ad9910_demo_context_t;

ad9910_demo_status_t g_ad9910_demo_status;

static ad9910_demo_context_t g_ad9910_demo;

static uint8_t ad9910_demo_time_expired(uint32_t deadline_ms)
{
    return ((int32_t)(HAL_GetTick() - deadline_ms) >= 0) ? 1U : 0U;
}

static uint8_t ad9910_demo_config_is_valid(
    const ad9910_demo_config_t *config)
{
    if ((config == NULL) ||
        (config->mode > AD9910_DEMO_MODE_CONTINUOUS_SWEEP) ||
        (config->start_frequency_hz > AD9910_MAX_FREQUENCY_HZ) ||
        (config->amplitude_percent > 100U) ||
        (config->phase_degrees > AD9910_MAX_PHASE_DEGREES)) {
        return 0U;
    }

    if (config->mode == AD9910_DEMO_MODE_FIXED) {
        return 1U;
    }

    return (config->start_frequency_hz < config->stop_frequency_hz) &&
           (config->stop_frequency_hz <= AD9910_MAX_FREQUENCY_HZ) &&
           (config->sweep_time_ms > 0U) &&
           ((config->mode == AD9910_DEMO_MODE_SINGLE_SWEEP) ||
            (config->return_time_ms > 0U));
}

static void ad9910_demo_set_error(ad9910_demo_error_t error)
{
    g_ad9910_demo.error = error;
    g_ad9910_demo.run_state = AD9910_DEMO_RUN_ERROR;
}

static HAL_StatusTypeDef ad9910_demo_apply_start_tone(void)
{
    const ad9910_tone_config_t tone = {
        .frequency_hz = g_ad9910_demo.config.start_frequency_hz,
        .amplitude = AD9910_AmplitudePercentToASF(
            g_ad9910_demo.config.amplitude_percent),
        .phase_offset = AD9910_PhaseDegreesToPOW(
            g_ad9910_demo.config.phase_degrees),
    };

    return AD9910_Service_SetTone(&tone);
}

static void ad9910_demo_begin_service_cycle(
    ad9910_demo_run_state_t wait_state)
{
    g_ad9910_demo.service_cycle_started = 0U;
    g_ad9910_demo.run_state = wait_state;
}

static uint8_t ad9910_demo_service_cycle_completed(void)
{
    if (AD9910_Service_GetState() != AD9910_SERVICE_STATE_READY) {
        g_ad9910_demo.service_cycle_started = 1U;
        return 0U;
    }

    return g_ad9910_demo.service_cycle_started;
}

static void ad9910_demo_enter_upper_hold(void)
{
    if (AD9910_Service_SetFrequencySweepHold(1U) != HAL_OK) {
        ad9910_demo_set_error(AD9910_DEMO_ERROR_SERVICE_COMMAND);
        return;
    }

    g_ad9910_demo.deadline_ms = HAL_GetTick() +
                                g_ad9910_demo.config.stop_hold_ms;
    g_ad9910_demo.run_state = AD9910_DEMO_RUN_HOLD_UPPER;
}

static void ad9910_demo_enter_lower_hold(void)
{
    if (AD9910_Service_SetFrequencySweepHold(1U) != HAL_OK) {
        ad9910_demo_set_error(AD9910_DEMO_ERROR_SERVICE_COMMAND);
        return;
    }

    g_ad9910_demo.deadline_ms = HAL_GetTick() +
                                g_ad9910_demo.config.start_hold_ms;
    g_ad9910_demo.run_state = AD9910_DEMO_RUN_HOLD_LOWER;
}

static void ad9910_demo_update_status(void)
{
    g_ad9910_demo_status.state = AD9910_Service_GetState();
    g_ad9910_demo_status.run_state = g_ad9910_demo.run_state;
    g_ad9910_demo_status.app_error = g_ad9910_demo.error;
    g_ad9910_demo_status.active_preset = g_ad9910_demo.preset;
    g_ad9910_demo_status.mode = g_ad9910_demo.config.mode;
    g_ad9910_demo_status.frequency_hz = AD9910_Service_GetFrequency();
    g_ad9910_demo_status.amplitude = AD9910_Service_GetAmplitude();
    g_ad9910_demo_status.phase_offset = AD9910_Service_GetPhaseOffset();
    g_ad9910_demo_status.selected_profile =
        AD9910_Service_GetSelectedProfile();
    g_ad9910_demo_status.frequency_sweep_active =
        AD9910_Service_IsFrequencySweepActive();
    g_ad9910_demo_status.frequency_sweep_direction_up =
        AD9910_Service_GetFrequencySweepDirection();
    g_ad9910_demo_status.frequency_sweep_hold =
        AD9910_Service_GetFrequencySweepHold();
    g_ad9910_demo_status.ramp_limit_event_count =
        AD9910_Service_GetRampLimitEventCount();
    g_ad9910_demo_status.positive_step_hz =
        g_ad9910_demo.sweep_plan.register_config.positive_step_hz;
    g_ad9910_demo_status.negative_step_hz =
        g_ad9910_demo.sweep_plan.register_config.negative_step_hz;
    g_ad9910_demo_status.positive_rate =
        g_ad9910_demo.sweep_plan.register_config.positive_rate;
    g_ad9910_demo_status.negative_rate =
        g_ad9910_demo.sweep_plan.register_config.negative_rate;
    g_ad9910_demo_status.actual_sweep_time_us =
        g_ad9910_demo.sweep_plan.actual_sweep_time_us;
    g_ad9910_demo_status.actual_return_time_us =
        g_ad9910_demo.sweep_plan.actual_return_time_us;
    g_ad9910_demo_status.last_hal_error = AD9910_Service_GetLastHalError();
}

HAL_StatusTypeDef AD9910_Demo_App_Init(SPI_HandleTypeDef *spi)
{
    const ad9910_demo_config_t *preset_config;
    HAL_StatusTypeDef status;

    g_ad9910_demo.preset = AD9910_Demo_Presets_GetBootPreset();
    preset_config = AD9910_Demo_Presets_Get(g_ad9910_demo.preset);
    if (preset_config == NULL) {
        g_ad9910_demo.error = AD9910_DEMO_ERROR_INVALID_PRESET;
        g_ad9910_demo.run_state = AD9910_DEMO_RUN_ERROR;
        ad9910_demo_update_status();
        return HAL_ERROR;
    }

    g_ad9910_demo.config = *preset_config;
    if (ad9910_demo_config_is_valid(&g_ad9910_demo.config) == 0U) {
        g_ad9910_demo.error = AD9910_DEMO_ERROR_INVALID_CONFIG;
        g_ad9910_demo.run_state = AD9910_DEMO_RUN_ERROR;
        ad9910_demo_update_status();
        return HAL_ERROR;
    }

    if (g_ad9910_demo.config.mode != AD9910_DEMO_MODE_FIXED) {
        const uint32_t planner_return_time_ms =
            (g_ad9910_demo.config.return_time_ms > 0U) ?
                g_ad9910_demo.config.return_time_ms :
                g_ad9910_demo.config.sweep_time_ms;

        status = AD9910_SweepPlanner_Create(
            g_ad9910_demo.config.start_frequency_hz,
            g_ad9910_demo.config.stop_frequency_hz,
            g_ad9910_demo.config.sweep_time_ms,
            planner_return_time_ms,
            g_ad9910_demo.config.target_steps,
            &g_ad9910_demo.sweep_plan);
        if (status != HAL_OK) {
            g_ad9910_demo.error = AD9910_DEMO_ERROR_SWEEP_PLAN;
            g_ad9910_demo.run_state = AD9910_DEMO_RUN_ERROR;
            ad9910_demo_update_status();
            return HAL_ERROR;
        }
    }

    status = AD9910_Service_Init(spi);
    if (status != HAL_OK) {
        g_ad9910_demo.error = AD9910_DEMO_ERROR_SERVICE_COMMAND;
        g_ad9910_demo.run_state = AD9910_DEMO_RUN_ERROR;
        ad9910_demo_update_status();
        return status;
    }

    g_ad9910_demo.error = AD9910_DEMO_ERROR_NONE;
    g_ad9910_demo.run_state = AD9910_DEMO_RUN_WAIT_SERVICE;
    g_ad9910_demo.observed_ramp_event_count = 0U;
    g_ad9910_demo.service_cycle_started = 0U;
    ad9910_demo_update_status();

    return HAL_OK;
}

void AD9910_Demo_App_Process(void)
{
    uint32_t ramp_event_count;

    AD9910_Service_Process();

    if (AD9910_Service_GetState() == AD9910_SERVICE_STATE_ERROR) {
        ad9910_demo_set_error(AD9910_DEMO_ERROR_SERVICE_STATE);
    }

    switch (g_ad9910_demo.run_state) {
    case AD9910_DEMO_RUN_WAIT_SERVICE:
        if (AD9910_Service_GetState() == AD9910_SERVICE_STATE_READY) {
            if (ad9910_demo_apply_start_tone() != HAL_OK) {
                ad9910_demo_set_error(AD9910_DEMO_ERROR_SERVICE_COMMAND);
                break;
            }
            ad9910_demo_begin_service_cycle(
                AD9910_DEMO_RUN_WAIT_TONE_APPLY);
        }
        break;

    case AD9910_DEMO_RUN_WAIT_TONE_APPLY:
        if (ad9910_demo_service_cycle_completed() != 0U) {
            if (g_ad9910_demo.config.mode == AD9910_DEMO_MODE_FIXED) {
                g_ad9910_demo.run_state = AD9910_DEMO_RUN_FIXED_READY;
            } else if (g_ad9910_demo.config.start_hold_ms > 0U) {
                g_ad9910_demo.deadline_ms = HAL_GetTick() +
                                            g_ad9910_demo.config.start_hold_ms;
                g_ad9910_demo.run_state = AD9910_DEMO_RUN_START_HOLD;
            } else {
                g_ad9910_demo.run_state = AD9910_DEMO_RUN_REQUEST_SWEEP;
            }
        }
        break;

    case AD9910_DEMO_RUN_START_HOLD:
        if (ad9910_demo_time_expired(g_ad9910_demo.deadline_ms) != 0U) {
            g_ad9910_demo.run_state = AD9910_DEMO_RUN_REQUEST_SWEEP;
        }
        break;

    case AD9910_DEMO_RUN_REQUEST_SWEEP:
        if (AD9910_Service_StartFrequencySweep(
                &g_ad9910_demo.sweep_plan.register_config) != HAL_OK) {
            ad9910_demo_set_error(AD9910_DEMO_ERROR_SERVICE_COMMAND);
            break;
        }
        ad9910_demo_begin_service_cycle(
            AD9910_DEMO_RUN_WAIT_SWEEP_APPLY);
        break;

    case AD9910_DEMO_RUN_WAIT_SWEEP_APPLY:
        if ((ad9910_demo_service_cycle_completed() != 0U) &&
            (AD9910_Service_IsFrequencySweepActive() != 0U)) {
            g_ad9910_demo.observed_ramp_event_count =
                AD9910_Service_GetRampLimitEventCount();
            g_ad9910_demo.run_state = AD9910_DEMO_RUN_SWEEP_UP;
        }
        break;

    case AD9910_DEMO_RUN_SWEEP_UP:
        ramp_event_count = AD9910_Service_GetRampLimitEventCount();
        if (ramp_event_count != g_ad9910_demo.observed_ramp_event_count) {
            g_ad9910_demo.observed_ramp_event_count = ramp_event_count;
            ad9910_demo_enter_upper_hold();
        }
        break;

    case AD9910_DEMO_RUN_HOLD_UPPER:
        if (ad9910_demo_time_expired(g_ad9910_demo.deadline_ms) != 0U) {
            if (g_ad9910_demo.config.mode ==
                AD9910_DEMO_MODE_SINGLE_SWEEP) {
                if (AD9910_Service_StopFrequencySweep(
                        g_ad9910_demo.config.stop_frequency_hz) != HAL_OK) {
                    ad9910_demo_set_error(
                        AD9910_DEMO_ERROR_SERVICE_COMMAND);
                    break;
                }
                ad9910_demo_begin_service_cycle(
                    AD9910_DEMO_RUN_WAIT_SINGLE_STOP);
            } else {
                if ((AD9910_Service_SetFrequencySweepDirection(0U) != HAL_OK) ||
                    (AD9910_Service_SetFrequencySweepHold(0U) != HAL_OK)) {
                    ad9910_demo_set_error(
                        AD9910_DEMO_ERROR_SERVICE_COMMAND);
                    break;
                }
                g_ad9910_demo.observed_ramp_event_count =
                    AD9910_Service_GetRampLimitEventCount();
                g_ad9910_demo.run_state = AD9910_DEMO_RUN_SWEEP_DOWN;
            }
        }
        break;

    case AD9910_DEMO_RUN_SWEEP_DOWN:
        ramp_event_count = AD9910_Service_GetRampLimitEventCount();
        if (ramp_event_count != g_ad9910_demo.observed_ramp_event_count) {
            g_ad9910_demo.observed_ramp_event_count = ramp_event_count;
            ad9910_demo_enter_lower_hold();
        }
        break;

    case AD9910_DEMO_RUN_HOLD_LOWER:
        if (ad9910_demo_time_expired(g_ad9910_demo.deadline_ms) != 0U) {
            if ((AD9910_Service_SetFrequencySweepDirection(1U) != HAL_OK) ||
                (AD9910_Service_SetFrequencySweepHold(0U) != HAL_OK)) {
                ad9910_demo_set_error(AD9910_DEMO_ERROR_SERVICE_COMMAND);
                break;
            }
            g_ad9910_demo.observed_ramp_event_count =
                AD9910_Service_GetRampLimitEventCount();
            g_ad9910_demo.run_state = AD9910_DEMO_RUN_SWEEP_UP;
        }
        break;

    case AD9910_DEMO_RUN_WAIT_SINGLE_STOP:
        if (ad9910_demo_service_cycle_completed() != 0U) {
            g_ad9910_demo.run_state = AD9910_DEMO_RUN_SINGLE_COMPLETE;
        }
        break;

    case AD9910_DEMO_RUN_FIXED_READY:
    case AD9910_DEMO_RUN_SINGLE_COMPLETE:
    case AD9910_DEMO_RUN_ERROR:
    default:
        break;
    }

    ad9910_demo_update_status();
}

const ad9910_demo_config_t *AD9910_Demo_App_GetConfig(void)
{
    return &g_ad9910_demo.config;
}

HAL_StatusTypeDef AD9910_Demo_App_SetFrequency(uint32_t frequency_hz)
{
    return AD9910_Service_SetFrequency(frequency_hz);
}

HAL_StatusTypeDef AD9910_Demo_App_SetAmplitude(uint16_t amplitude)
{
    return AD9910_Service_SetAmplitude(amplitude);
}

HAL_StatusTypeDef AD9910_Demo_App_SetPhaseDegrees(uint16_t phase_degrees)
{
    if (phase_degrees > AD9910_MAX_PHASE_DEGREES) {
        return HAL_ERROR;
    }

    return AD9910_Service_SetPhaseOffset(
        AD9910_PhaseDegreesToPOW(phase_degrees));
}

HAL_StatusTypeDef AD9910_Demo_App_ProgramProfile(
    uint8_t profile_index,
    uint32_t frequency_hz,
    uint8_t amplitude_percent,
    uint16_t phase_degrees)
{
    const ad9910_tone_config_t tone = {
        .frequency_hz = frequency_hz,
        .amplitude = AD9910_AmplitudePercentToASF(amplitude_percent),
        .phase_offset = AD9910_PhaseDegreesToPOW(phase_degrees),
    };

    if ((amplitude_percent > 100U) ||
        (phase_degrees > AD9910_MAX_PHASE_DEGREES)) {
        return HAL_ERROR;
    }

    return AD9910_Service_ProgramProfile(profile_index, &tone);
}

HAL_StatusTypeDef AD9910_Demo_App_SelectProfile(uint8_t profile_index)
{
    return AD9910_Service_SelectProfile(profile_index);
}

HAL_StatusTypeDef AD9910_Demo_App_StartFrequencySweep(
    const ad9910_frequency_sweep_config_t *config)
{
    return AD9910_Service_StartFrequencySweep(config);
}

HAL_StatusTypeDef AD9910_Demo_App_StopFrequencySweep(uint32_t frequency_hz)
{
    return AD9910_Service_StopFrequencySweep(frequency_hz);
}

HAL_StatusTypeDef AD9910_Demo_App_SetFrequencySweepDirection(uint8_t direction_up)
{
    return AD9910_Service_SetFrequencySweepDirection(direction_up);
}

HAL_StatusTypeDef AD9910_Demo_App_SetFrequencySweepHold(uint8_t hold)
{
    return AD9910_Service_SetFrequencySweepHold(hold);
}
