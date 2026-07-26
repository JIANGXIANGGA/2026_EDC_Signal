#ifndef AD9910_DEMO_APP_H
#define AD9910_DEMO_APP_H

#include <stdint.h>

#include "ad9910_service.h"

typedef enum {
    AD9910_DEMO_MODE_FIXED = 0,
    AD9910_DEMO_MODE_SINGLE_SWEEP,
    AD9910_DEMO_MODE_CONTINUOUS_SWEEP
} ad9910_demo_mode_t;

typedef enum {
    AD9910_DEMO_PRESET_FIXED_1KHZ = 0,
    AD9910_DEMO_PRESET_FIXED_1MHZ,
    AD9910_DEMO_PRESET_AUDIO_SWEEP,
    AD9910_DEMO_PRESET_RF_SWEEP,
    AD9910_DEMO_PRESET_SINGLE_RF_SWEEP,
    AD9910_DEMO_PRESET_USER,
    AD9910_DEMO_PRESET_COUNT
} ad9910_demo_preset_id_t;

typedef enum {
    AD9910_DEMO_RUN_WAIT_SERVICE = 0,
    AD9910_DEMO_RUN_WAIT_TONE_APPLY,
    AD9910_DEMO_RUN_START_HOLD,
    AD9910_DEMO_RUN_REQUEST_SWEEP,
    AD9910_DEMO_RUN_WAIT_SWEEP_APPLY,
    AD9910_DEMO_RUN_SWEEP_UP,
    AD9910_DEMO_RUN_HOLD_UPPER,
    AD9910_DEMO_RUN_SWEEP_DOWN,
    AD9910_DEMO_RUN_HOLD_LOWER,
    AD9910_DEMO_RUN_WAIT_SINGLE_STOP,
    AD9910_DEMO_RUN_FIXED_READY,
    AD9910_DEMO_RUN_SINGLE_COMPLETE,
    AD9910_DEMO_RUN_ERROR
} ad9910_demo_run_state_t;

typedef enum {
    AD9910_DEMO_ERROR_NONE = 0,
    AD9910_DEMO_ERROR_INVALID_PRESET,
    AD9910_DEMO_ERROR_INVALID_CONFIG,
    AD9910_DEMO_ERROR_SWEEP_PLAN,
    AD9910_DEMO_ERROR_SERVICE_COMMAND,
    AD9910_DEMO_ERROR_SERVICE_STATE
} ad9910_demo_error_t;

typedef struct {
    ad9910_demo_mode_t mode;
    uint32_t start_frequency_hz;
    uint32_t stop_frequency_hz;
    uint32_t sweep_time_ms;
    uint32_t return_time_ms;
    uint32_t start_hold_ms;
    uint32_t stop_hold_ms;
    uint32_t target_steps;
    uint8_t amplitude_percent;
    uint16_t phase_degrees;
} ad9910_demo_config_t;

typedef struct {
    ad9910_service_state_t state;
    ad9910_demo_run_state_t run_state;
    ad9910_demo_error_t app_error;
    ad9910_demo_preset_id_t active_preset;
    ad9910_demo_mode_t mode;
    uint32_t frequency_hz;
    uint16_t amplitude;
    uint16_t phase_offset;
    uint8_t selected_profile;
    uint8_t frequency_sweep_active;
    uint8_t frequency_sweep_direction_up;
    uint8_t frequency_sweep_hold;
    uint32_t ramp_limit_event_count;
    uint32_t positive_step_hz;
    uint32_t negative_step_hz;
    uint16_t positive_rate;
    uint16_t negative_rate;
    uint64_t actual_sweep_time_us;
    uint64_t actual_return_time_us;
    uint32_t last_hal_error;
} ad9910_demo_status_t;

extern ad9910_demo_status_t g_ad9910_demo_status;

HAL_StatusTypeDef AD9910_Demo_App_Init(SPI_HandleTypeDef *spi);
void AD9910_Demo_App_Process(void);
const ad9910_demo_config_t *AD9910_Demo_App_GetConfig(void);

HAL_StatusTypeDef AD9910_Demo_App_SetFrequency(uint32_t frequency_hz);
HAL_StatusTypeDef AD9910_Demo_App_SetAmplitude(uint16_t amplitude);
HAL_StatusTypeDef AD9910_Demo_App_SetPhaseDegrees(uint16_t phase_degrees);
HAL_StatusTypeDef AD9910_Demo_App_ProgramProfile(
    uint8_t profile_index,
    uint32_t frequency_hz,
    uint8_t amplitude_percent,
    uint16_t phase_degrees);
HAL_StatusTypeDef AD9910_Demo_App_SelectProfile(uint8_t profile_index);
HAL_StatusTypeDef AD9910_Demo_App_StartFrequencySweep(
    const ad9910_frequency_sweep_config_t *config);
HAL_StatusTypeDef AD9910_Demo_App_StopFrequencySweep(uint32_t frequency_hz);
HAL_StatusTypeDef AD9910_Demo_App_SetFrequencySweepDirection(uint8_t direction_up);
HAL_StatusTypeDef AD9910_Demo_App_SetFrequencySweepHold(uint8_t hold);

#endif
