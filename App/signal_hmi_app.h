#ifndef SIGNAL_HMI_APP_H
#define SIGNAL_HMI_APP_H

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
    ONE_HMI_WAVEFORM_STATE_IDLE = 0,
    ONE_HMI_WAVEFORM_STATE_REQUEST_PAGE,
    ONE_HMI_WAVEFORM_STATE_WAIT_PAGE,
    ONE_HMI_WAVEFORM_STATE_CLEAR,
    ONE_HMI_WAVEFORM_STATE_BEGIN_TRANSFER,
    ONE_HMI_WAVEFORM_STATE_WAIT_READY,
    ONE_HMI_WAVEFORM_STATE_SEND_DATA,
    ONE_HMI_WAVEFORM_STATE_WAIT_FINISHED
} one_hmi_waveform_state_t;

typedef enum {
    ONE_HMI_PLOT_TIME_DOMAIN = 0,
    ONE_HMI_PLOT_SPECTRUM
} one_hmi_plot_type_t;

typedef enum {
    ONE_HMI_ERROR_NONE = 0,
    ONE_HMI_ERROR_HMI_BUSY,
    ONE_HMI_ERROR_INVALID_VALUE,
    ONE_HMI_ERROR_SWEEP_NOT_SUPPORTED,
    ONE_HMI_ERROR_AD9910_COMMAND,
    ONE_HMI_ERROR_TRANSPORT,
    ONE_HMI_ERROR_QUERY_TIMEOUT,
    ONE_HMI_ERROR_HMI_RESPONSE
} one_hmi_error_t;

typedef struct {
    one_hmi_sync_state_t sync_state;
    one_hmi_error_t error;
    HAL_StatusTypeDef last_hal_status;
    one_hmi_waveform_state_t waveform_state;
    HAL_StatusTypeDef waveform_last_hal_status;
    uint32_t sync_count;
    uint32_t touch_count;
    uint32_t startup_count;
    uint32_t ignored_number_count;
    uint32_t query_timeout_count;
    uint32_t periodic_sync_count;
    uint32_t return_status_error_count;
    uint32_t transport_error_count;
    uint32_t rx_push_error_count;
    uint32_t waveform_transfer_count;
    uint32_t waveform_snapshot_request_count;
    uint32_t waveform_snapshot_busy_count;
    uint32_t waveform_snapshot_rejected_count;
    uint32_t waveform_timeout_count;
    uint32_t waveform_error_count;
    uint32_t waveform_page_skip_count;
    uint32_t waveform_unexpected_event_count;
    uint32_t waveform_last_sequence;
    uint32_t measurement_publish_count;
    uint32_t measurement_publish_error_count;
    uint32_t measurement_last_sequence;
    uint32_t frequency_hz;
    uint8_t amplitude_percent;
    uint16_t phase_degrees;
    one_hmi_wave_t waveform;
    one_hmi_mode_t mode;
    uint8_t profile_index;
    uint8_t run_flag;
    uint8_t current_page_known;
    uint8_t current_page_id;
    uint8_t link_alive;
    uint8_t last_return_status;
    uint8_t waveform_cycles;
    uint8_t measurement_self_test_active;
    uint8_t waveform_snapshot_pending;
    one_hmi_plot_type_t active_plot;
    uint32_t last_rx_tick_ms;
} signal_hmi_status_t;

HAL_StatusTypeDef Signal_HMI_App_Init(UART_HandleTypeDef *uart);
HAL_StatusTypeDef Signal_HMI_App_Process(void);
HAL_StatusTypeDef Signal_HMI_App_RequestSync(void);
HAL_StatusTypeDef Signal_HMI_App_SetWaveformCycles(
    uint8_t cycle_count);
HAL_StatusTypeDef Signal_HMI_App_RequestWaveformSnapshot(void);
const signal_hmi_status_t *Signal_HMI_App_GetStatus(void);

#endif
