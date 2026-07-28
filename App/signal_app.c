#include "signal_app.h"

#include <stddef.h>

#include "ad9910_auto_test_app.h"
#include "ad9910_signal_generator_app.h"
#include "signal_acquisition_service.h"

static signal_app_status_t g_signal_app_status;

static void signal_app_update_status(void)
{
    const ad9910_siggen_status_t *signal_generator_status =
        AD9910_SignalGenerator_GetStatus();
    const signal_acquisition_status_t *acquisition_status =
        Signal_Acquisition_Service_GetStatus();

    if (signal_generator_status != NULL) {
        g_signal_app_status.signal_generator = *signal_generator_status;
    }
    if (acquisition_status != NULL) {
        g_signal_app_status.acquisition = *acquisition_status;
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

    if ((config == NULL) || (config->ad9910_spi == NULL) ||
        (config->adc_spi == NULL) || (config->adc_timer == NULL)) {
        return signal_app_set_error(SIGNAL_APP_ERROR_INVALID_CONFIG,
                                    HAL_ERROR);
    }

    status = AD9910_SignalGenerator_App_Init(config->ad9910_spi);
    if (status != HAL_OK) {
        return signal_app_set_error(SIGNAL_APP_ERROR_AD9910_INIT, status);
    }

    acquisition_config.adc_spi = config->adc_spi;
    acquisition_config.adc_timer = config->adc_timer;
    acquisition_config.dac = config->dac;
    acquisition_config.dac_timer = config->dac_timer;

    status = Signal_Acquisition_Service_Init(&acquisition_config);
    if (status != HAL_OK) {
        return signal_app_set_error(SIGNAL_APP_ERROR_ACQUISITION_INIT,
                                    status);
    }

#if SIGNAL_APP_ENABLE_AD9910_AUTO_TEST
    AD9910_AutoTest_App_Init();
#endif

    g_signal_app_status.initialized = 1U;
    g_signal_app_status.error = SIGNAL_APP_ERROR_NONE;
    g_signal_app_status.last_hal_status = HAL_OK;
    signal_app_update_status();
    return HAL_OK;
}

void Signal_App_Process(void)
{
    HAL_StatusTypeDef acquisition_status;

    if (g_signal_app_status.initialized == 0U) {
        (void)signal_app_set_error(SIGNAL_APP_ERROR_INVALID_CONFIG,
                                   HAL_ERROR);
        return;
    }

    AD9910_SignalGenerator_App_Process();

#if SIGNAL_APP_ENABLE_AD9910_AUTO_TEST
    AD9910_AutoTest_App_Process();
    if (AD9910_AutoTest_App_GetStatus()->state ==
        AD9910_AUTO_TEST_STATE_ERROR) {
        (void)signal_app_set_error(SIGNAL_APP_ERROR_AUTO_TEST, HAL_ERROR);
    }
#endif

    acquisition_status = Signal_Acquisition_Service_Process();
    if (acquisition_status != HAL_OK) {
        (void)signal_app_set_error(SIGNAL_APP_ERROR_ACQUISITION_RUNTIME,
                                   acquisition_status);
    }

    signal_app_update_status();
    if (g_signal_app_status.signal_generator.app_error !=
        AD9910_SIGGEN_ERROR_NONE) {
        g_signal_app_status.error = SIGNAL_APP_ERROR_AD9910_RUNTIME;
        g_signal_app_status.last_hal_status = HAL_ERROR;
    } else if ((acquisition_status == HAL_OK) &&
               (g_signal_app_status.error != SIGNAL_APP_ERROR_AUTO_TEST)) {
        g_signal_app_status.error = SIGNAL_APP_ERROR_NONE;
        g_signal_app_status.last_hal_status = HAL_OK;
    }

    g_signal_app_status.process_count++;
}

const signal_app_status_t *Signal_App_GetStatus(void)
{
    return &g_signal_app_status;
}
