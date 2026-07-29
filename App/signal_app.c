#include "signal_app.h"

#include <stddef.h>

#include "signal_hmi_app.h"
#include "signal_acquisition_service.h"

static signal_app_status_t g_signal_app_status;

static void signal_app_update_status(void)
{
    const signal_acquisition_status_t *acquisition_status =
        Signal_Acquisition_Service_GetStatus();
    const signal_hmi_status_t *hmi_status = Signal_HMI_App_GetStatus();

    if (acquisition_status != NULL) {
        g_signal_app_status.acquisition = *acquisition_status;
    }
    if (hmi_status != NULL) {
        g_signal_app_status.hmi = *hmi_status;
    }
}

static HAL_StatusTypeDef signal_app_set_error(signal_app_error_t error,
                                               HAL_StatusTypeDef status)
{
    g_signal_app_status.error = error;
    g_signal_app_status.last_hal_status = status;
    signal_app_update_status();
    return status;
}

HAL_StatusTypeDef Signal_App_Init(const signal_app_config_t *config)
{
    signal_acquisition_config_t acquisition_config;
    HAL_StatusTypeDef status;

    g_signal_app_status = (signal_app_status_t){0};

    if ((config == NULL) || (config->adc_timer == NULL) ||
        (config->hmi_uart == NULL)) {
        return signal_app_set_error(SIGNAL_APP_ERROR_INVALID_CONFIG,
                                    HAL_ERROR);
    }

    status = Signal_HMI_App_Init(config->hmi_uart);
    if (status != HAL_OK) {
        return signal_app_set_error(SIGNAL_APP_ERROR_HMI_INIT, status);
    }

    acquisition_config.adc_timer = config->adc_timer;
    acquisition_config.dac = config->dac;
    acquisition_config.dac_timer = config->dac_timer;
    acquisition_config.measurement_calibration =
        config->measurement_calibration;

    status = Signal_Acquisition_Service_Init(&acquisition_config);
    if (status != HAL_OK) {
        return signal_app_set_error(SIGNAL_APP_ERROR_ACQUISITION_INIT,
                                    status);
    }

    g_signal_app_status.initialized = 1U;
    g_signal_app_status.error = SIGNAL_APP_ERROR_NONE;
    g_signal_app_status.last_hal_status = HAL_OK;
    signal_app_update_status();
    return HAL_OK;
}

void Signal_App_Process(void)
{
    HAL_StatusTypeDef acquisition_status;
    HAL_StatusTypeDef hmi_status;

    if (g_signal_app_status.initialized == 0U) {
        (void)signal_app_set_error(SIGNAL_APP_ERROR_INVALID_CONFIG,
                                   HAL_ERROR);
        return;
    }

    hmi_status = Signal_HMI_App_Process();

    acquisition_status = Signal_Acquisition_Service_Process();
    if (acquisition_status != HAL_OK) {
        (void)signal_app_set_error(SIGNAL_APP_ERROR_ACQUISITION_RUNTIME,
                                   acquisition_status);
    }

    signal_app_update_status();
    if (acquisition_status != HAL_OK) {
        g_signal_app_status.error = SIGNAL_APP_ERROR_ACQUISITION_RUNTIME;
        g_signal_app_status.last_hal_status = acquisition_status;
    } else if (hmi_status != HAL_OK) {
        g_signal_app_status.error = SIGNAL_APP_ERROR_HMI_RUNTIME;
        g_signal_app_status.last_hal_status = hmi_status;
    } else {
        g_signal_app_status.error = SIGNAL_APP_ERROR_NONE;
        g_signal_app_status.last_hal_status = HAL_OK;
    }

    g_signal_app_status.process_count++;
}

const signal_app_status_t *Signal_App_GetStatus(void)
{
    return &g_signal_app_status;
}
