#include "adc_dac_loopback_service.h"

#include "dac_output.h"

#define ADC_DAC_LOOPBACK_DATA_MASK 0x0FFFU

static uint8_t g_adc_dac_loopback_started;
static uint8_t g_adc_dac_loopback_primed_count;
static uint32_t g_adc_dac_loopback_dropped_block_count;
static uint32_t g_adc_dac_loopback_error_count;

HAL_StatusTypeDef ADC_DAC_Loopback_Init(DAC_HandleTypeDef *hdac,
                                        TIM_HandleTypeDef *trigger_timer,
                                        uint32_t sample_rate_hz)
{
    HAL_StatusTypeDef status;

    g_adc_dac_loopback_started = 0U;
    g_adc_dac_loopback_primed_count = 0U;
    g_adc_dac_loopback_dropped_block_count = 0U;
    g_adc_dac_loopback_error_count = 0U;

    status = DAC_Output_Init(hdac, trigger_timer);
    if (status != HAL_OK) {
        g_adc_dac_loopback_error_count++;
        return status;
    }

    status = DAC_Output_ConfigSampleRate(sample_rate_hz);
    if (status != HAL_OK) {
        g_adc_dac_loopback_error_count++;
        return status;
    }

    return HAL_OK;
}

uint8_t ADC_DAC_Loopback_PushBlock(const uint16_t *samples, uint32_t length)
{
    uint16_t *buffer;
    uint8_t index;

    if ((samples == NULL) || (length < DAC_OUTPUT_BLOCK_SIZE)) {
        g_adc_dac_loopback_error_count++;
        return 0U;
    }

    if (DAC_Output_AcquireBuffer(&buffer, &index) == 0U) {
        g_adc_dac_loopback_dropped_block_count++;
        return 0U;
    }

    for (uint32_t sample_index = 0U;
         sample_index < DAC_OUTPUT_BLOCK_SIZE;
         ++sample_index) {
        buffer[sample_index] =
            samples[sample_index] & ADC_DAC_LOOPBACK_DATA_MASK;
    }

    if (DAC_Output_CommitBuffer(index) == 0U) {
        g_adc_dac_loopback_error_count++;
        return 0U;
    }

    if (g_adc_dac_loopback_started == 0U) {
        if (g_adc_dac_loopback_primed_count < DAC_OUTPUT_HALF_SIZE) {
            g_adc_dac_loopback_primed_count++;
        }

        /* 先预装两个半区，再启动 DAC，避免初始阶段输出欠载。 */
        if (g_adc_dac_loopback_primed_count >= DAC_OUTPUT_HALF_SIZE) {
            if (DAC_Output_Start() != HAL_OK) {
                g_adc_dac_loopback_error_count++;
                return 0U;
            }
            g_adc_dac_loopback_started = 1U;
        }
    }

    return 1U;
}

uint8_t ADC_DAC_Loopback_IsRunning(void)
{
    return g_adc_dac_loopback_started;
}

uint32_t ADC_DAC_Loopback_GetDroppedBlockCount(void)
{
    return g_adc_dac_loopback_dropped_block_count;
}

uint32_t ADC_DAC_Loopback_GetErrorCount(void)
{
    return g_adc_dac_loopback_error_count;
}
