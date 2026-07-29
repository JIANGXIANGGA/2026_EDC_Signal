#ifndef WAVEFORM_ANALYZER_SERVICE_H
#define WAVEFORM_ANALYZER_SERVICE_H

#include <stdint.h>

#include "stm32g4xx_hal.h"

#define WAVEFORM_ANALYZER_FFT_SIZE 4096U
#define WAVEFORM_ANALYZER_BIN_COUNT (WAVEFORM_ANALYZER_FFT_SIZE / 2U)
#define WAVEFORM_ANALYZER_PEAK_COUNT 6U
/* 题目允许的最大 FFT 频率栅格间隔。 */
#define WAVEFORM_ANALYZER_MAX_BIN_RESOLUTION_HZ 500U

typedef enum {
    WAVEFORM_ANALYZER_TYPE_UNKNOWN = 0,
    WAVEFORM_ANALYZER_TYPE_DC,
    WAVEFORM_ANALYZER_TYPE_SINE,
    WAVEFORM_ANALYZER_TYPE_SQUARE,
    WAVEFORM_ANALYZER_TYPE_TRIANGLE,
    WAVEFORM_ANALYZER_TYPE_SAWTOOTH
} waveform_analyzer_type_t;

typedef struct {
    uint8_t initialized;
    uint8_t result_ready;
    uint32_t analysis_count;
    uint32_t sample_rate_hz;
    float bin_resolution_hz;
    waveform_analyzer_type_t waveform_type;
    uint8_t clipped_low;
    uint8_t clipped_high;
    uint16_t min_code;
    uint16_t max_code;
    uint16_t average_code;
    uint16_t peak_to_peak_code;
    float dc_code;
    float rms_code;
    uint16_t fundamental_bin;
    float fundamental_frequency_hz;
    float fundamental_amplitude_code;
    float harmonic2_percent;
    float harmonic3_percent;
    float harmonic4_percent;
    float harmonic5_percent;
    float thd_percent;
    float duty_percent;
    uint8_t peak_count;
    /* 有效谱线按频率从低到高排列，第一条即基波。 */
    uint16_t peak_bins[WAVEFORM_ANALYZER_PEAK_COUNT];
    float peak_frequencies_hz[WAVEFORM_ANALYZER_PEAK_COUNT];
    float peak_amplitudes_code[WAVEFORM_ANALYZER_PEAK_COUNT];
} waveform_analyzer_result_t;

HAL_StatusTypeDef Waveform_Analyzer_Init(uint32_t sample_rate_hz);
HAL_StatusTypeDef Waveform_Analyzer_ProcessBlock(const uint16_t *samples,
                                                 uint32_t length);
const waveform_analyzer_result_t *Waveform_Analyzer_GetResult(void);
const float *Waveform_Analyzer_GetMagnitudeBins(void);
float Waveform_Analyzer_GetMagnitudeAtBin(uint16_t bin_index);

#endif
