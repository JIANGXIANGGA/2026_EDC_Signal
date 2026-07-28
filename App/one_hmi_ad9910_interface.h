#ifndef ONE_HMI_AD9910_INTERFACE_H
#define ONE_HMI_AD9910_INTERFACE_H

#include <stdint.h>

#include "stm32g4xx_hal.h"

typedef enum {
    ONE_HMI_MODE_SINGLE = 0,
    ONE_HMI_MODE_RAM = 2,
    ONE_HMI_MODE_SWEEP = 3,
    ONE_HMI_MODE_PROFILE = 4
} one_hmi_mode_t;

typedef enum {
    ONE_HMI_WAVE_SINE = 0,
    ONE_HMI_WAVE_TRIANGLE = 1,
    ONE_HMI_WAVE_SQUARE = 2,
    ONE_HMI_WAVE_SAW = 3
} one_hmi_wave_t;

typedef enum {
    ONE_HMI_SYNC_STATE_IDLE = 0,
    ONE_HMI_SYNC_STATE_QUERYING,
    ONE_HMI_SYNC_STATE_READY,
    ONE_HMI_SYNC_STATE_ERROR
} one_hmi_sync_state_t;

typedef enum {
    ONE_HMI_ERROR_NONE = 0,
    ONE_HMI_ERROR_HMI_BUSY,
    ONE_HMI_ERROR_INVALID_VALUE,
    ONE_HMI_ERROR_SWEEP_NOT_SUPPORTED,
    ONE_HMI_ERROR_AD9910_COMMAND
} one_hmi_error_t;

typedef struct {
    one_hmi_sync_state_t sync_state;
    one_hmi_error_t error;
    HAL_StatusTypeDef last_hal_status;
    uint32_t sync_count;
    uint32_t touch_count;
    uint32_t startup_count;
    uint32_t ignored_number_count;
    uint32_t frequency_hz;
    uint8_t amplitude_percent;
    uint16_t phase_degrees;
    one_hmi_wave_t waveform;
    one_hmi_mode_t mode;
    uint8_t profile_index;
    uint8_t run_flag;
} one_hmi_ad9910_status_t;

void One_HMI_AD9910_Interface_Init(void);
void One_HMI_AD9910_Interface_Process(void);
HAL_StatusTypeDef One_HMI_AD9910_Interface_RequestSync(void);
const one_hmi_ad9910_status_t *One_HMI_AD9910_Interface_GetStatus(void);

#endif
