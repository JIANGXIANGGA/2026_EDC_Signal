#ifndef WAVEFORM_ANALYZER_SERVICE_H
#define WAVEFORM_ANALYZER_SERVICE_H

#include <stdint.h>

#include "fft.h"
#include "stm32g4xx_hal.h"

#define WAVEFORM_ANALYZER_FFT_SIZE FFT_LENGTH
#define WAVEFORM_ANALYZER_BIN_COUNT FFT_BIN_COUNT
#define WAVEFORM_ANALYZER_PEAK_COUNT 6U
#define WAVEFORM_ANALYZER_MAX_BIN_RESOLUTION_HZ 500U

typedef struct {
    uint8_t initialized;
    uint8_t result_ready;
    uint32_t analysis_count;
    uint32_t sample_rate_hz;
    float bin_resolution_hz;
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
    uint8_t peak_count;
    uint16_t peak_bins[WAVEFORM_ANALYZER_PEAK_COUNT];
    float peak_frequencies_hz[WAVEFORM_ANALYZER_PEAK_COUNT];
    float peak_amplitudes_code[WAVEFORM_ANALYZER_PEAK_COUNT];
    float peak_phases_rad[WAVEFORM_ANALYZER_PEAK_COUNT];
} waveform_analyzer_result_t;

HAL_StatusTypeDef Waveform_Analyzer_Init(uint32_t sample_rate_hz);
HAL_StatusTypeDef Waveform_Analyzer_ProcessBlock(const uint16_t *samples,
                                                 uint32_t length);
const waveform_analyzer_result_t *Waveform_Analyzer_GetResult(void);
const float *Waveform_Analyzer_GetMagnitudeBins(void);
float Waveform_Analyzer_GetMagnitudeAtBin(uint16_t bin_index);

#endif
