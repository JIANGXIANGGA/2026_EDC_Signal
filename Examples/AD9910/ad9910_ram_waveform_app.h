#ifndef AD9910_RAM_WAVEFORM_APP_H
#define AD9910_RAM_WAVEFORM_APP_H

#include <stdint.h>

#include "ad9910_service.h"

#define AD9910_RAM_WAVEFORM_APP_MAX_SAMPLES AD9910_RAM_WORD_COUNT

typedef enum {
    AD9910_RAM_WAVEFORM_APP_STATE_WAIT_SERVICE = 0,
    AD9910_RAM_WAVEFORM_APP_STATE_REQUEST_START,
    AD9910_RAM_WAVEFORM_APP_STATE_WAIT_PLAYBACK,
    AD9910_RAM_WAVEFORM_APP_STATE_RUNNING,
    AD9910_RAM_WAVEFORM_APP_STATE_ERROR
} ad9910_ram_waveform_app_state_t;

typedef enum {
    AD9910_RAM_WAVEFORM_APP_ERROR_NONE = 0,
    AD9910_RAM_WAVEFORM_APP_ERROR_INVALID_PRESET,
    AD9910_RAM_WAVEFORM_APP_ERROR_SERVICE_COMMAND,
    AD9910_RAM_WAVEFORM_APP_ERROR_SERVICE_STATE
} ad9910_ram_waveform_app_error_t;

typedef struct {
    ad9910_ram_waveform_app_state_t app_state;
    ad9910_ram_waveform_app_error_t app_error;
    ad9910_service_state_t service_state;
    uint8_t ram_active;
    ad9910_ram_destination_t destination;
    ad9910_ram_mode_t mode;
    uint16_t sample_count;
    uint16_t address_step_rate;
    uint32_t carrier_frequency_hz;
    uint8_t carrier_amplitude_percent;
    uint32_t playback_sample_rate_hz;
    uint32_t last_hal_error;
} ad9910_ram_waveform_app_status_t;

extern ad9910_ram_waveform_app_status_t g_ad9910_ram_waveform_status;

HAL_StatusTypeDef AD9910_Ram_Waveform_App_Init(SPI_HandleTypeDef *spi);
void AD9910_Ram_Waveform_App_Process(void);

#endif
