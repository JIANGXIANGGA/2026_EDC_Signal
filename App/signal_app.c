#include "signal_app.h"

#include <stddef.h>

#include "ad9910_signal_generator_app.h"
#include "adc_dac_loopback_service.h"
#include "adc121s101.h"
#include "dac_output.h"

#define SIGNAL_APP_ENABLE_ADC121S101 1U
#define SIGNAL_APP_ENABLE_ADC_DAC_LOOPBACK 0U
#define SIGNAL_APP_ENABLE_AD9910_RAM_AUTO_TEST 1U
#define SIGNAL_APP_ADC_DAC_SAMPLE_RATE_HZ 1000000U
#define SIGNAL_APP_AD9910_AUTO_TEST_BOOT_DELAY_MS 1000U
#define SIGNAL_APP_AD9910_AUTO_TEST_HOLD_MS 2000U

typedef enum {
    SIGNAL_APP_AD9910_AUTO_TEST_WAIT_BOOT = 0,
    SIGNAL_APP_AD9910_AUTO_TEST_WAIT_RAM_ACTIVE,
    SIGNAL_APP_AD9910_AUTO_TEST_HOLD
} signal_app_ad9910_auto_test_state_t;

typedef struct {
    signal_app_ad9910_auto_test_state_t state;
    uint8_t preset_index;
    uint32_t deadline_ms;
} signal_app_ad9910_auto_test_context_t;

signal_app_status_t g_signal_app_status;

static signal_app_ad9910_auto_test_context_t g_signal_app_ad9910_auto_test;

#if SIGNAL_APP_ENABLE_ADC121S101
static uint16_t g_signal_app_adc_block[ADC_INPUT_BLOCK_SIZE];

static void Signal_App_UpdateAdcStatistics(const uint16_t *samples,
                                           uint32_t length)
{
    uint16_t min_value = 0x0FFFU;
    uint16_t max_value = 0U;
    uint32_t sum = 0U;

    for (uint32_t index = 0U; index < length; ++index) {
        const uint16_t sample = samples[index];
        if (sample < min_value) {
            min_value = sample;
        }
        if (sample > max_value) {
            max_value = sample;
        }
        sum += sample;
    }

    g_signal_app_status.adc_last_min = min_value;
    g_signal_app_status.adc_last_max = max_value;
    g_signal_app_status.adc_last_average = (uint16_t)(sum / length);
}
#endif

static void Signal_App_UpdateStatus(void)
{
    g_signal_app_status.adc_running = ADC121S101_IsRunning();
    g_signal_app_status.adc_half_complete_count =
        ADC121S101_GetHalfCompleteCount();
    g_signal_app_status.adc_complete_count =
        ADC121S101_GetCompleteCount();
    g_signal_app_status.adc_error_count =
        ADC121S101_GetErrorCount();
    g_signal_app_status.adc_overrun_count =
        ADC121S101_GetOverrunCount();
    g_signal_app_status.dac_half_complete_count =
        DAC_Output_GetHalfCompleteCount();
    g_signal_app_status.dac_complete_count =
        DAC_Output_GetCompleteCount();
    g_signal_app_status.dac_error_count =
        DAC_Output_GetErrorCount();
    g_signal_app_status.dac_underrun_count =
        DAC_Output_GetUnderrunCount();
#if SIGNAL_APP_ENABLE_ADC_DAC_LOOPBACK
    g_signal_app_status.dac_loopback_running =
        ADC_DAC_Loopback_IsRunning();
    g_signal_app_status.dac_loopback_dropped_block_count =
        ADC_DAC_Loopback_GetDroppedBlockCount();
    g_signal_app_status.dac_loopback_error_count =
        ADC_DAC_Loopback_GetErrorCount();
#endif
}

static uint8_t Signal_App_TimeReached(uint32_t deadline_ms)
{
    return ((int32_t)(HAL_GetTick() - deadline_ms) >= 0) ? 1U : 0U;
}

static void Signal_App_Ad9910AutoTest_Init(void)
{
    g_signal_app_ad9910_auto_test.state =
        SIGNAL_APP_AD9910_AUTO_TEST_WAIT_BOOT;
    g_signal_app_ad9910_auto_test.preset_index = 0U;
    g_signal_app_ad9910_auto_test.deadline_ms =
        HAL_GetTick() + SIGNAL_APP_AD9910_AUTO_TEST_BOOT_DELAY_MS;
}

static void Signal_App_Ad9910AutoTest_Process(void)
{
#if SIGNAL_APP_ENABLE_AD9910_RAM_AUTO_TEST
    const ad9910_siggen_status_t *status = AD9910_SignalGenerator_GetStatus();
    const uint32_t now_tick = HAL_GetTick();

    switch (g_signal_app_ad9910_auto_test.state) {
    case SIGNAL_APP_AD9910_AUTO_TEST_WAIT_BOOT:
        if (Signal_App_TimeReached(
                g_signal_app_ad9910_auto_test.deadline_ms) != 0U) {
            (void)AD9910_SignalGenerator_SelectRamPreset(0U);
            if (AD9910_SignalGenerator_SetMode(
                    AD9910_SIGGEN_MODE_RAM_WAVEFORM) == HAL_OK) {
                g_signal_app_ad9910_auto_test.preset_index = 0U;
                g_signal_app_ad9910_auto_test.state =
                    SIGNAL_APP_AD9910_AUTO_TEST_WAIT_RAM_ACTIVE;
            }
        }
        break;

    case SIGNAL_APP_AD9910_AUTO_TEST_WAIT_RAM_ACTIVE:
        if ((status != NULL) &&
            (status->active_mode == AD9910_SIGGEN_MODE_RAM_WAVEFORM) &&
            (status->active_ram_preset ==
             g_signal_app_ad9910_auto_test.preset_index) &&
            (status->pending_apply == 0U)) {
            g_signal_app_ad9910_auto_test.deadline_ms =
                now_tick + SIGNAL_APP_AD9910_AUTO_TEST_HOLD_MS;
            g_signal_app_ad9910_auto_test.state =
                SIGNAL_APP_AD9910_AUTO_TEST_HOLD;
        }
        break;

    case SIGNAL_APP_AD9910_AUTO_TEST_HOLD:
        if (Signal_App_TimeReached(
                g_signal_app_ad9910_auto_test.deadline_ms) != 0U) {
            g_signal_app_ad9910_auto_test.preset_index =
                (uint8_t)(g_signal_app_ad9910_auto_test.preset_index + 1U);
            if (g_signal_app_ad9910_auto_test.preset_index >=
                AD9910_SIGGEN_RAM_PRESET_COUNT) {
                g_signal_app_ad9910_auto_test.preset_index = 0U;
            }

            (void)AD9910_SignalGenerator_SelectRamPreset(
                g_signal_app_ad9910_auto_test.preset_index);
            if (AD9910_SignalGenerator_SetMode(
                    AD9910_SIGGEN_MODE_RAM_WAVEFORM) == HAL_OK) {
                g_signal_app_ad9910_auto_test.state =
                    SIGNAL_APP_AD9910_AUTO_TEST_WAIT_RAM_ACTIVE;
            }
        }
        break;

    default:
        g_signal_app_ad9910_auto_test.state =
            SIGNAL_APP_AD9910_AUTO_TEST_WAIT_BOOT;
        break;
    }
#else
    (void)g_signal_app_ad9910_auto_test;
#endif
}

HAL_StatusTypeDef Signal_App_Init(SPI_HandleTypeDef *ad9910_spi,
                                  SPI_HandleTypeDef *adc_spi,
                                  TIM_HandleTypeDef *adc_timer,
                                  DAC_HandleTypeDef *dac,
                                  TIM_HandleTypeDef *dac_timer)
{
    HAL_StatusTypeDef status;

    status = AD9910_SignalGenerator_App_Init(ad9910_spi);
    if (status != HAL_OK) {
        return status;
    }

    Signal_App_Ad9910AutoTest_Init();

#if SIGNAL_APP_ENABLE_ADC121S101
    status = ADC_Input_Init(adc_spi, adc_timer);
    if (status != HAL_OK) {
        return status;
    }
#else
    (void)adc_spi;
    (void)adc_timer;
#endif

#if SIGNAL_APP_ENABLE_ADC_DAC_LOOPBACK
    status = ADC_DAC_Loopback_Init(dac,
                                   dac_timer,
                                   SIGNAL_APP_ADC_DAC_SAMPLE_RATE_HZ);
    if (status != HAL_OK) {
        return status;
    }
#else
    (void)dac;
    (void)dac_timer;
#endif

#if SIGNAL_APP_ENABLE_ADC121S101
    status = ADC_Start();
    if (status != HAL_OK) {
        return status;
    }
#endif

    g_signal_app_status.initialized = 1U;
    Signal_App_UpdateStatus();
    return HAL_OK;
}

void Signal_App_Process(void)
{
    AD9910_SignalGenerator_App_Process();
    Signal_App_Ad9910AutoTest_Process();

#if SIGNAL_APP_ENABLE_ADC121S101
    (void)ADC_Process();
#endif

#if SIGNAL_APP_ENABLE_ADC121S101
    if (ADC121S101_CopyLatestBlock(g_signal_app_adc_block,
                                   ADC_INPUT_BLOCK_SIZE) != 0U) {
        g_signal_app_status.adc_block_count++;
        Signal_App_UpdateAdcStatistics(g_signal_app_adc_block,
                                       ADC_INPUT_BLOCK_SIZE);
#if SIGNAL_APP_ENABLE_ADC_DAC_LOOPBACK
        (void)ADC_DAC_Loopback_PushBlock(g_signal_app_adc_block,
                                         ADC_INPUT_BLOCK_SIZE);
#endif
    }
#endif

    Signal_App_UpdateStatus();
}
