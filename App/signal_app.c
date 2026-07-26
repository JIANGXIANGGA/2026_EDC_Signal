#include "signal_app.h"

#include <stddef.h>

#include "ad9910_demo_app.h"
#include "ad9910_ram_waveform_app.h"
#include "adc_dac_loopback_service.h"
#include "adc121s101.h"
#include "dac_output.h"

#define SIGNAL_APP_USE_AD9910_RAM_PLAYBACK 1U
#define SIGNAL_APP_ENABLE_ADC121S101 1U
#define SIGNAL_APP_ENABLE_ADC_DAC_LOOPBACK 1U
#define SIGNAL_APP_ADC_DAC_SAMPLE_RATE_HZ 1000000U

signal_app_status_t g_signal_app_status;

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

HAL_StatusTypeDef Signal_App_Init(SPI_HandleTypeDef *ad9910_spi,
                                  SPI_HandleTypeDef *adc_spi,
                                  TIM_HandleTypeDef *adc_timer,
                                  DAC_HandleTypeDef *dac,
                                  TIM_HandleTypeDef *dac_timer)
{
    HAL_StatusTypeDef status;

#if SIGNAL_APP_USE_AD9910_RAM_PLAYBACK
    status = AD9910_Ram_Waveform_App_Init(ad9910_spi);
#else
    status = AD9910_Demo_App_Init(ad9910_spi);
#endif
    if (status != HAL_OK) {
        return status;
    }

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
#if SIGNAL_APP_USE_AD9910_RAM_PLAYBACK
    AD9910_Ram_Waveform_App_Process();
#else
    AD9910_Demo_App_Process();
#endif

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
