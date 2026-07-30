#ifndef FFT_H
#define FFT_H

#include <stdint.h>

#define FFT_LENGTH 8192U
#define FFT_BIN_COUNT (FFT_LENGTH / 2U + 1U)
#define FFT_MIN_FREQUENCY_HZ 10000.0f
#define FFT_MAX_FREQUENCY_HZ 500000.0f
/*
 * 量程端点允许一个最大 FFT 栅格的误差。
 * 实际信号源频偏和三点插值可能让 10 kHz/500 kHz 略微越界，
 * 不能因为亚 kHz 的估计偏差丢掉端点有效分量。
 */
#define FFT_FREQUENCY_RANGE_TOLERANCE_HZ 500.0f

extern float FFT_Magnitude[FFT_BIN_COUNT];
extern uint32_t FFT_PeakIndex;
extern float FFT_PeakFrequency;
extern float FFT_PeakAmplitude;

void FFT_Process(const uint16_t adc_data[FFT_LENGTH],
                 float sample_rate_hz,
                 float mean_code);
uint8_t FFT_GetInterpolatedPeak(uint32_t bin_index,
                                 float sample_rate_hz,
                                 float *frequency_hz,
                                 float *amplitude_code);
/**
 * @brief 在 FFT 插值得到的精确频率上执行 Hann 加权最小二乘估计。
 * @note 模型为 x[n] = dc + a*cos(wn) + b*sin(wn)，幅值为峰值。
 */
uint8_t FFT_EstimateTone(const uint16_t adc_data[FFT_LENGTH],
                         float sample_rate_hz,
                         float frequency_hz,
                         float *amplitude_code,
                         float *phase_rad);

#endif
