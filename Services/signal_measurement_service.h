#ifndef SIGNAL_MEASUREMENT_SERVICE_H
#define SIGNAL_MEASUREMENT_SERVICE_H

#include <stdint.h>

#include "stm32g4xx_hal.h"
#include "waveform_analyzer_service.h"

#define SIGNAL_MEASUREMENT_COMPONENT_COUNT 3U
#define SIGNAL_MEASUREMENT_RESPONSE_POINT_COUNT 6U
#define SIGNAL_MEASUREMENT_AVERAGING_FRAME_COUNT 9U

/*
 * 默认按 ADC 满量程 3.3 V、模拟前端增益 1 倍换算。
 * 实机必须使用标准信号源标定 SIGNAL_MEASUREMENT_DEFAULT_FRONT_END_GAIN，
 * 或在运行时调用 Signal_Measurement_Service_SetCalibration()。
 */
#ifndef SIGNAL_MEASUREMENT_DEFAULT_ADC_REFERENCE_MV
#define SIGNAL_MEASUREMENT_DEFAULT_ADC_REFERENCE_MV 3300.0f
#endif

#ifndef SIGNAL_MEASUREMENT_DEFAULT_FRONT_END_GAIN
#define SIGNAL_MEASUREMENT_DEFAULT_FRONT_END_GAIN 1.0f
#endif

typedef struct {
    uint32_t frequency_hz;
    float correction_gain;
} signal_measurement_response_point_t;

typedef struct {
    float input_mv_per_code;
    float peak_to_peak_gain;
    float rms_gain;
    float spectrum_gain;
    uint8_t response_point_count;
    signal_measurement_response_point_t
        response[SIGNAL_MEASUREMENT_RESPONSE_POINT_COUNT];
} signal_measurement_calibration_t;

typedef struct {
    uint8_t valid;
    uint8_t harmonic_order;
    float frequency_hz;
    float amplitude_mv;
} signal_measurement_component_t;

typedef struct {
    uint8_t initialized;
    uint8_t result_ready;
    uint8_t signal_valid;
    uint8_t clipped;
    uint8_t component_count;
    uint8_t averaging_count;
    uint32_t measurement_count;
    float peak_to_peak_mv;
    float peak_to_peak_spread_mv;
    float true_rms_mv;
    float raw_rms_mv;
    float fundamental_frequency_hz;
    float max_component_spread_mv;
    signal_measurement_component_t
        components[SIGNAL_MEASUREMENT_COMPONENT_COUNT];
} signal_measurement_result_t;

HAL_StatusTypeDef Signal_Measurement_Service_Init(
    const signal_measurement_calibration_t *calibration);
HAL_StatusTypeDef Signal_Measurement_Service_Process(
    const waveform_analyzer_result_t *analysis);
HAL_StatusTypeDef Signal_Measurement_Service_SetCalibration(
    const signal_measurement_calibration_t *calibration);
void Signal_Measurement_Service_GetDefaultCalibration(
    signal_measurement_calibration_t *calibration);
const signal_measurement_calibration_t *
Signal_Measurement_Service_GetCalibration(void);
const signal_measurement_result_t *Signal_Measurement_Service_GetResult(void);

#endif
