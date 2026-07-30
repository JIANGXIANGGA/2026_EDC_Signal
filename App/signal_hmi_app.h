#ifndef SIGNAL_HMI_APP_H
#define SIGNAL_HMI_APP_H

#include <stdint.h>

#include "stm32g4xx_hal.h"

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
    ONE_HMI_ERROR_TRANSPORT
} one_hmi_error_t;

typedef struct {
    one_hmi_error_t error;
    HAL_StatusTypeDef last_hal_status;
    one_hmi_waveform_state_t waveform_state;
    HAL_StatusTypeDef waveform_last_hal_status;
    uint32_t touch_count;
    uint32_t startup_count;
    uint32_t ignored_number_count;
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
    uint8_t current_page_known;
    uint8_t current_page_id;
    uint8_t link_alive;
    uint8_t last_return_status;
    uint8_t waveform_cycles;
    uint8_t waveform_snapshot_pending;
    one_hmi_plot_type_t active_plot;
    uint32_t last_rx_tick_ms;
} signal_hmi_status_t;

HAL_StatusTypeDef Signal_HMI_App_Init(UART_HandleTypeDef *uart);
HAL_StatusTypeDef Signal_HMI_App_Process(void);
HAL_StatusTypeDef Signal_HMI_App_SetWaveformCycles(
    uint8_t cycle_count);
HAL_StatusTypeDef Signal_HMI_App_RequestWaveformSnapshot(void);
const signal_hmi_status_t *Signal_HMI_App_GetStatus(void);

#endif
