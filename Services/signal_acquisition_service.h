#ifndef SIGNAL_ACQUISITION_SERVICE_H
#define SIGNAL_ACQUISITION_SERVICE_H

#include <stdint.h>

#include "signal_measurement_service.h"
#include "stm32g4xx_hal.h"
#include "waveform_analyzer_service.h"

#ifndef SIGNAL_ACQUISITION_ENABLE_DAC_LOOPBACK
#define SIGNAL_ACQUISITION_ENABLE_DAC_LOOPBACK 0U
#endif

typedef enum {
    SIGNAL_ACQUISITION_ERROR_NONE = 0,
    SIGNAL_ACQUISITION_ERROR_INVALID_CONFIG,
    SIGNAL_ACQUISITION_ERROR_ADC_INIT,
    SIGNAL_ACQUISITION_ERROR_ANALYZER_INIT,
    SIGNAL_ACQUISITION_ERROR_MEASUREMENT_INIT,
    SIGNAL_ACQUISITION_ERROR_LOOPBACK_INIT,
    SIGNAL_ACQUISITION_ERROR_ADC_START,
    SIGNAL_ACQUISITION_ERROR_ADC_RUNTIME,
    SIGNAL_ACQUISITION_ERROR_ANALYZER_RUNTIME,
    SIGNAL_ACQUISITION_ERROR_MEASUREMENT_RUNTIME
} signal_acquisition_error_t;

typedef struct {
    TIM_HandleTypeDef *adc_timer;
    DAC_HandleTypeDef *dac;
    TIM_HandleTypeDef *dac_timer;
    const signal_measurement_calibration_t *measurement_calibration;
} signal_acquisition_config_t;

typedef struct {
    uint8_t initialized;
    signal_acquisition_error_t error;
    HAL_StatusTypeDef last_hal_status;
    uint8_t adc_running;
    uint32_t adc_block_count;
    uint32_t adc_half_complete_count;
    uint32_t adc_complete_count;
    uint32_t adc_error_count;
    uint32_t adc_overrun_count;
    uint16_t adc_last_min;
    uint16_t adc_last_max;
    uint16_t adc_last_average;
    uint32_t dac_half_complete_count;
    uint32_t dac_complete_count;
    uint32_t dac_error_count;
    uint32_t dac_underrun_count;
    uint8_t dac_loopback_running;
    uint32_t dac_loopback_dropped_block_count;
    uint32_t dac_loopback_error_count;
    uint8_t fft_ready;
    uint32_t fft_analysis_count;
    uint32_t fft_sample_rate_hz;
    float fft_bin_resolution_hz;
    float detected_frequency_hz;
    uint16_t waveform_peak_to_peak_code;
    uint16_t waveform_average_code;
    float waveform_rms_code;
    signal_measurement_result_t measurement;
} signal_acquisition_status_t;

HAL_StatusTypeDef Signal_Acquisition_Service_Init(
    const signal_acquisition_config_t *config);
HAL_StatusTypeDef Signal_Acquisition_Service_Process(void);
/**
 * @brief 获取最近一次完成分析的 ADC 采样块。
 * @note 返回指针在下一次 Signal_Acquisition_Service_Process() 前有效。
 */
uint8_t Signal_Acquisition_Service_GetLatestBlock(
    const uint16_t **samples,
    uint32_t *length,
    uint32_t *sequence);
const signal_acquisition_status_t *Signal_Acquisition_Service_GetStatus(void);

#endif
