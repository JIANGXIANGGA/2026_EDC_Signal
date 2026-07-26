#ifndef AD9910_SERVICE_H
#define AD9910_SERVICE_H

#include <stdint.h>

#include "ad9910.h"
#include "stm32g4xx_hal.h"

typedef struct {
    uint32_t frequency_hz;
    uint16_t amplitude;
    uint16_t phase_offset;
} ad9910_tone_config_t;

typedef struct {
    uint32_t lower_frequency_hz;
    uint32_t upper_frequency_hz;
    uint32_t positive_step_hz;
    uint32_t negative_step_hz;
    uint16_t positive_rate;
    uint16_t negative_rate;
} ad9910_frequency_sweep_config_t;

typedef struct {
    uint8_t profile_index;
    uint16_t start_address;
    uint16_t sample_count;
    uint16_t address_step_rate;
    uint32_t playback_sample_rate_hz;
    ad9910_ram_destination_t destination;
    ad9910_ram_mode_t mode;
    uint8_t no_dwell_high;
    uint8_t zero_crossing;
    ad9910_tone_config_t base_tone;
    const uint32_t *ram_words;
    uint16_t ram_word_count;
} ad9910_ram_playback_config_t;

typedef enum {
    AD9910_SERVICE_STATE_POWER_UP_DELAY = 0,
    AD9910_SERVICE_STATE_WRITE_CFR1,
    AD9910_SERVICE_STATE_WAIT_CFR1,
    AD9910_SERVICE_STATE_WRITE_CFR2,
    AD9910_SERVICE_STATE_WAIT_CFR2,
    AD9910_SERVICE_STATE_WRITE_CFR3,
    AD9910_SERVICE_STATE_WAIT_CFR3,
    AD9910_SERVICE_STATE_APPLY_CFR,
    AD9910_SERVICE_STATE_PLL_LOCK_WAIT,
    AD9910_SERVICE_STATE_WRITE_PROFILE0,
    AD9910_SERVICE_STATE_WAIT_PROFILE0,
    AD9910_SERVICE_STATE_APPLY_PROFILE0,
    AD9910_SERVICE_STATE_WRITE_SWEEP_CFR2,
    AD9910_SERVICE_STATE_WAIT_SWEEP_CFR2,
    AD9910_SERVICE_STATE_WRITE_DRL,
    AD9910_SERVICE_STATE_WAIT_DRL,
    AD9910_SERVICE_STATE_WRITE_DRS,
    AD9910_SERVICE_STATE_WAIT_DRS,
    AD9910_SERVICE_STATE_WRITE_DRR,
    AD9910_SERVICE_STATE_WAIT_DRR,
    AD9910_SERVICE_STATE_APPLY_SWEEP,
    AD9910_SERVICE_STATE_WRITE_FIXED_CFR2,
    AD9910_SERVICE_STATE_WAIT_FIXED_CFR2,
    AD9910_SERVICE_STATE_WRITE_RAM_DISABLE_CFR1,
    AD9910_SERVICE_STATE_WAIT_RAM_DISABLE_CFR1,
    AD9910_SERVICE_STATE_APPLY_RAM_DISABLE,
    AD9910_SERVICE_STATE_WRITE_RAM_BASE_FTW,
    AD9910_SERVICE_STATE_WAIT_RAM_BASE_FTW,
    AD9910_SERVICE_STATE_WRITE_RAM_BASE_POW,
    AD9910_SERVICE_STATE_WAIT_RAM_BASE_POW,
    AD9910_SERVICE_STATE_WRITE_RAM_BASE_ASF,
    AD9910_SERVICE_STATE_WAIT_RAM_BASE_ASF,
    AD9910_SERVICE_STATE_WRITE_RAM_PROFILE,
    AD9910_SERVICE_STATE_WAIT_RAM_PROFILE,
    AD9910_SERVICE_STATE_APPLY_RAM_PROFILE,
    AD9910_SERVICE_STATE_WRITE_RAM_DATA,
    AD9910_SERVICE_STATE_WAIT_RAM_DATA,
    AD9910_SERVICE_STATE_WRITE_RAM_PLAYBACK_CFR1,
    AD9910_SERVICE_STATE_WAIT_RAM_PLAYBACK_CFR1,
    AD9910_SERVICE_STATE_APPLY_RAM_PLAYBACK,
    AD9910_SERVICE_STATE_WRITE_RAM_STOP_CFR1,
    AD9910_SERVICE_STATE_WAIT_RAM_STOP_CFR1,
    AD9910_SERVICE_STATE_APPLY_RAM_STOP,
    AD9910_SERVICE_STATE_READY,
    AD9910_SERVICE_STATE_ERROR
} ad9910_service_state_t;

HAL_StatusTypeDef AD9910_Service_Init(SPI_HandleTypeDef *spi);
void AD9910_Service_Process(void);
HAL_StatusTypeDef AD9910_Service_SetFrequency(uint32_t frequency_hz);
HAL_StatusTypeDef AD9910_Service_SetAmplitude(uint16_t amplitude);
HAL_StatusTypeDef AD9910_Service_SetPhaseOffset(uint16_t phase_offset);
HAL_StatusTypeDef AD9910_Service_SetTone(const ad9910_tone_config_t *config);
HAL_StatusTypeDef AD9910_Service_ProgramProfile(
    uint8_t profile_index,
    const ad9910_tone_config_t *config);
HAL_StatusTypeDef AD9910_Service_SelectProfile(uint8_t profile_index);
HAL_StatusTypeDef AD9910_Service_StartFrequencySweep(
    const ad9910_frequency_sweep_config_t *config);
HAL_StatusTypeDef AD9910_Service_StopFrequencySweep(uint32_t frequency_hz);
HAL_StatusTypeDef AD9910_Service_StartRamPlayback(
    const ad9910_ram_playback_config_t *config);
HAL_StatusTypeDef AD9910_Service_StopRamPlayback(uint32_t frequency_hz);
HAL_StatusTypeDef AD9910_Service_SetFrequencySweepDirection(uint8_t direction_up);
HAL_StatusTypeDef AD9910_Service_SetFrequencySweepHold(uint8_t hold);
void AD9910_Service_OnRampLimitEvent(void);
uint8_t AD9910_Service_IsFrequencySweepActive(void);
uint8_t AD9910_Service_GetFrequencySweepDirection(void);
uint8_t AD9910_Service_GetFrequencySweepHold(void);
uint32_t AD9910_Service_GetRampLimitEventCount(void);
uint8_t AD9910_Service_IsRamPlaybackActive(void);
ad9910_ram_destination_t AD9910_Service_GetRamDestination(void);
ad9910_ram_mode_t AD9910_Service_GetRamMode(void);
uint16_t AD9910_Service_GetRamSampleCount(void);
uint16_t AD9910_Service_GetRamAddressStepRate(void);
ad9910_service_state_t AD9910_Service_GetState(void);
uint32_t AD9910_Service_GetFrequency(void);
uint16_t AD9910_Service_GetAmplitude(void);
uint16_t AD9910_Service_GetPhaseOffset(void);
uint8_t AD9910_Service_GetSelectedProfile(void);
uint32_t AD9910_Service_GetLastHalError(void);

#endif
