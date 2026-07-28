#include "ad9910_auto_test_app.h"

#include <stddef.h>

#include "ad9910_signal_generator_app.h"

#define AD9910_AUTO_TEST_BOOT_DELAY_MS 1000U
#define AD9910_AUTO_TEST_RAM_PRESET_INDEX 5U
#define AD9910_AUTO_TEST_FREQUENCY_HZ 10000U
#define AD9910_AUTO_TEST_AMPLITUDE_PERCENT 60U
#define AD9910_AUTO_TEST_PHASE_DEGREES 0U
#define AD9910_AUTO_TEST_HARMONIC2_PERCENT 35U
#define AD9910_AUTO_TEST_HARMONIC3_PERCENT 20U

typedef struct {
    ad9910_auto_test_status_t status;
    uint32_t deadline_ms;
} ad9910_auto_test_context_t;

static ad9910_auto_test_context_t g_ad9910_auto_test;

static uint8_t ad9910_auto_test_time_reached(uint32_t deadline_ms)
{
    return ((int32_t)(HAL_GetTick() - deadline_ms) >= 0) ? 1U : 0U;
}

static void ad9910_auto_test_set_error(HAL_StatusTypeDef status)
{
    g_ad9910_auto_test.status.state = AD9910_AUTO_TEST_STATE_ERROR;
    g_ad9910_auto_test.status.last_hal_status = status;
}

void AD9910_AutoTest_App_Init(void)
{
    g_ad9910_auto_test.status.state = AD9910_AUTO_TEST_STATE_WAIT_BOOT;
    g_ad9910_auto_test.status.last_hal_status = HAL_OK;
    g_ad9910_auto_test.deadline_ms =
        HAL_GetTick() + AD9910_AUTO_TEST_BOOT_DELAY_MS;
}

void AD9910_AutoTest_App_Process(void)
{
    const ad9910_siggen_status_t *signal_status =
        AD9910_SignalGenerator_GetStatus();

    if ((signal_status != NULL) &&
        (signal_status->app_error != AD9910_SIGGEN_ERROR_NONE)) {
        ad9910_auto_test_set_error(HAL_ERROR);
        return;
    }

    switch (g_ad9910_auto_test.status.state) {
    case AD9910_AUTO_TEST_STATE_WAIT_BOOT:
        if (ad9910_auto_test_time_reached(
                g_ad9910_auto_test.deadline_ms) != 0U) {
            const ad9910_siggen_tone_param_t tone = {
                .frequency_hz = AD9910_AUTO_TEST_FREQUENCY_HZ,
                .amplitude_percent = AD9910_AUTO_TEST_AMPLITUDE_PERCENT,
                .phase_degrees = AD9910_AUTO_TEST_PHASE_DEGREES,
            };

            if ((AD9910_SignalGenerator_SetRamPresetTone(
                     AD9910_AUTO_TEST_RAM_PRESET_INDEX,
                     &tone) != HAL_OK) ||
                (AD9910_SignalGenerator_SetRamPresetComposite(
                     AD9910_AUTO_TEST_RAM_PRESET_INDEX,
                     AD9910_AUTO_TEST_HARMONIC2_PERCENT,
                     AD9910_AUTO_TEST_HARMONIC3_PERCENT) != HAL_OK) ||
                (AD9910_SignalGenerator_SelectRamPreset(
                     AD9910_AUTO_TEST_RAM_PRESET_INDEX) != HAL_OK) ||
                (AD9910_SignalGenerator_SetMode(
                     AD9910_SIGGEN_MODE_RAM_WAVEFORM) != HAL_OK)) {
                ad9910_auto_test_set_error(HAL_ERROR);
                break;
            }

            g_ad9910_auto_test.status.state =
                AD9910_AUTO_TEST_STATE_WAIT_RAM_ACTIVE;
        }
        break;

    case AD9910_AUTO_TEST_STATE_WAIT_RAM_ACTIVE:
        if ((signal_status != NULL) &&
            (signal_status->active_mode ==
             AD9910_SIGGEN_MODE_RAM_WAVEFORM) &&
            (signal_status->active_ram_preset ==
             AD9910_AUTO_TEST_RAM_PRESET_INDEX) &&
            (signal_status->pending_apply == 0U)) {
            g_ad9910_auto_test.status.state =
                AD9910_AUTO_TEST_STATE_RUNNING;
        }
        break;

    case AD9910_AUTO_TEST_STATE_RUNNING:
    case AD9910_AUTO_TEST_STATE_ERROR:
    default:
        break;
    }
}

const ad9910_auto_test_status_t *AD9910_AutoTest_App_GetStatus(void)
{
    return &g_ad9910_auto_test.status;
}
