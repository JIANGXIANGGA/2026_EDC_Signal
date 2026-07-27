#ifndef AD9910_SIGNAL_GENERATOR_APP_H
#define AD9910_SIGNAL_GENERATOR_APP_H

#include <stdint.h>

#include "ad9910_service.h"

#define AD9910_SIGGEN_RAM_PRESET_COUNT AD9910_PROFILE_COUNT
#define AD9910_SIGGEN_RAM_SAMPLE_COUNT 64U
#define AD9910_SIGGEN_SINGLE_TONE_PROFILE_INDEX 0U
#define AD9910_SIGGEN_RAM_MAX_WAVE_FREQUENCY_HZ \
    ((uint32_t)(AD9910_SYSTEM_CLOCK_HZ / (4ULL * AD9910_SIGGEN_RAM_SAMPLE_COUNT)))
#define AD9910_SIGGEN_DEFAULT_SINGLE_FREQUENCY_HZ 1000000U
#define AD9910_SIGGEN_DEFAULT_RAM_FREQUENCY_HZ 100000U
#define AD9910_SIGGEN_DEFAULT_AMPLITUDE_PERCENT 100U
#define AD9910_SIGGEN_DEFAULT_PHASE_DEGREES 0U

typedef enum {
    AD9910_SIGGEN_MODE_SINGLE_TONE = 0,
    AD9910_SIGGEN_MODE_RAM_WAVEFORM
} ad9910_siggen_mode_t;

typedef enum {
    AD9910_SIGGEN_WAVEFORM_SINE = 0,
    AD9910_SIGGEN_WAVEFORM_SQUARE,
    AD9910_SIGGEN_WAVEFORM_TRIANGLE,
    AD9910_SIGGEN_WAVEFORM_SAW_RISE,
    AD9910_SIGGEN_WAVEFORM_SAW_FALL,
    AD9910_SIGGEN_WAVEFORM_COMPOSITE,
    AD9910_SIGGEN_WAVEFORM_COUNT
} ad9910_siggen_waveform_t;

typedef enum {
    AD9910_SIGGEN_STATE_WAIT_SERVICE = 0,
    AD9910_SIGGEN_STATE_READY,
    AD9910_SIGGEN_STATE_WAIT_SINGLE_TONE,
    AD9910_SIGGEN_STATE_WAIT_RAM_START,
    AD9910_SIGGEN_STATE_WAIT_RAM_STOP,
    AD9910_SIGGEN_STATE_ERROR
} ad9910_siggen_state_t;

typedef enum {
    AD9910_SIGGEN_ERROR_NONE = 0,
    AD9910_SIGGEN_ERROR_INVALID_COMMAND,
    AD9910_SIGGEN_ERROR_SERVICE_COMMAND,
    AD9910_SIGGEN_ERROR_SERVICE_STATE
} ad9910_siggen_error_t;

typedef enum {
    AD9910_SIGGEN_COMMAND_SET_MODE = 0,
    AD9910_SIGGEN_COMMAND_SET_SINGLE_TONE,
    AD9910_SIGGEN_COMMAND_SELECT_RAM_PRESET,
    AD9910_SIGGEN_COMMAND_SET_RAM_PRESET_TONE,
    AD9910_SIGGEN_COMMAND_SET_RAM_PRESET_WAVEFORM,
    AD9910_SIGGEN_COMMAND_SET_RAM_PRESET_DUTY,
    AD9910_SIGGEN_COMMAND_SET_RAM_PRESET_COMPOSITE,
    AD9910_SIGGEN_COMMAND_APPLY_ACTIVE
} ad9910_siggen_command_type_t;

typedef struct {
    uint32_t frequency_hz;
    uint8_t amplitude_percent;
    uint16_t phase_degrees;
} ad9910_siggen_tone_param_t;

typedef struct {
    ad9910_siggen_waveform_t waveform;
    ad9910_siggen_tone_param_t tone;
    uint8_t duty_percent;
    uint8_t harmonic2_percent;
    uint8_t harmonic3_percent;
} ad9910_siggen_ram_preset_t;

typedef struct {
    ad9910_siggen_command_type_t type;
    uint8_t ram_preset_index;
    ad9910_siggen_mode_t mode;
    ad9910_siggen_waveform_t waveform;
    ad9910_siggen_tone_param_t tone;
    uint8_t duty_percent;
    uint8_t harmonic2_percent;
    uint8_t harmonic3_percent;
} ad9910_siggen_command_t;

typedef struct {
    ad9910_siggen_state_t app_state;
    ad9910_siggen_error_t app_error;
    ad9910_service_state_t service_state;
    ad9910_siggen_mode_t active_mode;
    ad9910_siggen_mode_t requested_mode;
    ad9910_siggen_waveform_t active_ram_waveform;
    uint8_t active_ram_preset;
    uint8_t ram_active;
    uint8_t pending_apply;
    uint8_t pending_single_tone_update;
    uint32_t frequency_hz;
    uint8_t amplitude_percent;
    uint16_t phase_degrees;
    uint32_t ram_playback_sample_rate_hz;
    uint16_t ram_sample_count;
    uint32_t last_hal_error;
} ad9910_siggen_status_t;

extern ad9910_siggen_status_t g_ad9910_siggen_status;

HAL_StatusTypeDef AD9910_SignalGenerator_App_Init(SPI_HandleTypeDef *spi);
void AD9910_SignalGenerator_App_Process(void);
HAL_StatusTypeDef AD9910_SignalGenerator_HandleCommand(
    const ad9910_siggen_command_t *command);
HAL_StatusTypeDef AD9910_SignalGenerator_SetMode(ad9910_siggen_mode_t mode);
HAL_StatusTypeDef AD9910_SignalGenerator_SetSingleTone(
    const ad9910_siggen_tone_param_t *tone);
HAL_StatusTypeDef AD9910_SignalGenerator_SelectRamPreset(
    uint8_t preset_index);
HAL_StatusTypeDef AD9910_SignalGenerator_SetRamPresetTone(
    uint8_t preset_index,
    const ad9910_siggen_tone_param_t *tone);
HAL_StatusTypeDef AD9910_SignalGenerator_SetRamPresetWaveform(
    uint8_t preset_index,
    ad9910_siggen_waveform_t waveform);
HAL_StatusTypeDef AD9910_SignalGenerator_SetRamPresetDuty(
    uint8_t preset_index,
    uint8_t duty_percent);
HAL_StatusTypeDef AD9910_SignalGenerator_SetRamPresetComposite(
    uint8_t preset_index,
    uint8_t harmonic2_percent,
    uint8_t harmonic3_percent);
const ad9910_siggen_tone_param_t *AD9910_SignalGenerator_GetSingleTone(void);
const ad9910_siggen_ram_preset_t *AD9910_SignalGenerator_GetRamPreset(
    uint8_t preset_index);
const ad9910_siggen_status_t *AD9910_SignalGenerator_GetStatus(void);

#endif
