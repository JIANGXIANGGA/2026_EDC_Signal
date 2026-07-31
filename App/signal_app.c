#include "signal_app.h"

#include <stddef.h>

#include "signal_hmi_app.h"
#include "signal_acquisition_service.h"
#include "vofa_telemetry_service.h"

static signal_app_status_t g_signal_app_status;

static void signal_app_update_status(void)
{
    const signal_acquisition_status_t *acquisition_status =
        Signal_Acquisition_Service_GetStatus();
    const signal_hmi_status_t *hmi_status = Signal_HMI_App_GetStatus();
    const vofa_telemetry_status_t *vofa_status =
        VOFA_Telemetry_Service_GetStatus();

    if (acquisition_status != NULL) {
        g_signal_app_status.acquisition = *acquisition_status;
    }
    if (hmi_status != NULL) {
        g_signal_app_status.hmi = *hmi_status;
    }
    if (vofa_status != NULL) {
        g_signal_app_status.vofa = *vofa_status;
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

    if ((config == NULL) || (config->hmi_uart == NULL)) {
        return signal_app_set_error(SIGNAL_APP_ERROR_INVALID_CONFIG,
                                    HAL_ERROR);
    }

    status = Signal_HMI_App_Init(config->hmi_uart);
    if (status != HAL_OK) {
        return signal_app_set_error(SIGNAL_APP_ERROR_HMI_INIT, status);
    }

    acquisition_config.measurement_calibration =
        config->measurement_calibration;

    status = Signal_Acquisition_Service_Init(&acquisition_config);
    if (status != HAL_OK) {
        return signal_app_set_error(SIGNAL_APP_ERROR_ACQUISITION_INIT,
                                    status);
    }

    status = VOFA_Telemetry_Service_Init();
    if (status != HAL_OK) {
        return signal_app_set_error(SIGNAL_APP_ERROR_VOFA_INIT, status);
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
    HAL_StatusTypeDef vofa_status;

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

    vofa_status = VOFA_Telemetry_Service_Process();
    if ((vofa_status != HAL_OK) && (vofa_status != HAL_BUSY)) {
        (void)signal_app_set_error(SIGNAL_APP_ERROR_VOFA_RUNTIME,
                                   vofa_status);
    }

    signal_app_update_status();
    if (acquisition_status != HAL_OK) {
        g_signal_app_status.error = SIGNAL_APP_ERROR_ACQUISITION_RUNTIME;
        g_signal_app_status.last_hal_status = acquisition_status;
    } else if (hmi_status != HAL_OK) {
        g_signal_app_status.error = SIGNAL_APP_ERROR_HMI_RUNTIME;
        g_signal_app_status.last_hal_status = hmi_status;
    } else if ((vofa_status != HAL_OK) && (vofa_status != HAL_BUSY)) {
        g_signal_app_status.error = SIGNAL_APP_ERROR_VOFA_RUNTIME;
        g_signal_app_status.last_hal_status = vofa_status;
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
