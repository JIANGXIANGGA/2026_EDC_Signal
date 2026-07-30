#ifndef FFT_H
#define FFT_H

#include <stdint.h>

#define FFT_LENGTH 4096U
#define FFT_BIN_COUNT (FFT_LENGTH / 2U + 1U)
#define FFT_MIN_FREQUENCY_HZ 10000.0f
#define FFT_MAX_FREQUENCY_HZ 500000.0f

extern float FFT_Magnitude[FFT_BIN_COUNT];
extern uint32_t FFT_PeakIndex;
extern float FFT_PeakFrequency;
extern float FFT_PeakAmplitude;

void FFT_Process(const uint16_t adc_data[FFT_LENGTH], float sample_rate_hz);
uint8_t FFT_GetInterpolatedPeak(uint32_t bin_index,
                                 float sample_rate_hz,
                                 float *frequency_hz,
                                 float *amplitude_code);

#endif
