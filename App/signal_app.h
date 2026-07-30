#ifndef SIGNAL_APP_H
#define SIGNAL_APP_H

#include <stdint.h>

#include "signal_hmi_app.h"
#include "signal_acquisition_service.h"
#include "stm32g4xx_hal.h"

typedef enum {
    SIGNAL_APP_ERROR_NONE = 0,
    SIGNAL_APP_ERROR_INVALID_CONFIG,
    SIGNAL_APP_ERROR_HMI_INIT,
    SIGNAL_APP_ERROR_ACQUISITION_INIT,
    SIGNAL_APP_ERROR_HMI_RUNTIME,
    SIGNAL_APP_ERROR_ACQUISITION_RUNTIME
} signal_app_error_t;

typedef struct {
    TIM_HandleTypeDef *adc_timer;
    UART_HandleTypeDef *hmi_uart;
    const signal_measurement_calibration_t *measurement_calibration;
} signal_app_config_t;

typedef struct {
    uint8_t initialized;
    signal_app_error_t error;
    HAL_StatusTypeDef last_hal_status;
    uint32_t process_count;
    signal_hmi_status_t hmi;
    signal_acquisition_status_t acquisition;
} signal_app_status_t;

HAL_StatusTypeDef Signal_App_Init(const signal_app_config_t *config);
void Signal_App_Process(void);
const signal_app_status_t *Signal_App_GetStatus(void);

#endif
