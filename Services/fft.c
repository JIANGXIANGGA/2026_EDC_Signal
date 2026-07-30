#include "fft.h"

#include <math.h>
#include <stddef.h>

#include "arm_const_structs.h"
#include "arm_math.h"

#define FFT_PI 3.14159265358979323846f
#define FFT_MIN_PEAK_AMPLITUDE_CODE 1.0f
#define FFT_CCMRAM __attribute__((section(".ccmram"), aligned(4)))

static float FFT_Buffer[FFT_LENGTH * 2U] FFT_CCMRAM;
static float FFT_HannWindow[FFT_LENGTH];
static float FFT_HannCoherentGain = 1.0f;
static uint8_t FFT_HannWindowReady;

float FFT_Magnitude[FFT_BIN_COUNT];
uint32_t FFT_PeakIndex;
float FFT_PeakFrequency;
float FFT_PeakAmplitude;

static void FFT_ClearResult(void)
{
    for (uint32_t index = 0U; index < FFT_BIN_COUNT; ++index) {
        FFT_Magnitude[index] = 0.0f;
    }

    FFT_PeakIndex = 0U;
    FFT_PeakFrequency = 0.0f;
    FFT_PeakAmplitude = 0.0f;
}

static void FFT_InitHannWindow(void)
{
    float sum = 0.0f;

    if (FFT_HannWindowReady != 0U) {
        return;
    }

    for (uint32_t index = 0U; index < FFT_LENGTH; ++index) {
        FFT_HannWindow[index] =
            0.5f - (0.5f * cosf((2.0f * FFT_PI * (float)index) /
                                 (float)(FFT_LENGTH - 1U)));
        sum += FFT_HannWindow[index];
    }

    FFT_HannCoherentGain = sum / (float)FFT_LENGTH;
    FFT_HannWindowReady = 1U;
}

static float FFT_HannResponse(float delta)
{
    const float denominator = 1.0f - (delta * delta);

    if ((fabsf(delta) < 0.000001f) ||
        (fabsf(denominator) < 0.000001f)) {
        return 1.0f;
    }

    return fabsf((sinf(FFT_PI * delta) / (FFT_PI * delta)) / denominator);
}

uint8_t FFT_GetInterpolatedPeak(uint32_t bin_index,
                                 float sample_rate_hz,
                                 float *frequency_hz,
                                 float *amplitude_code)
{
    float left;
    float center;
    float right;
    float denominator;
    float delta = 0.0f;
    float response;

    if ((frequency_hz == NULL) || (amplitude_code == NULL) ||
        (sample_rate_hz <= 0.0f) || (bin_index == 0U) ||
        (bin_index >= (FFT_BIN_COUNT - 1U))) {
        return 0U;
    }

    left = FFT_Magnitude[bin_index - 1U];
    center = FFT_Magnitude[bin_index];
    right = FFT_Magnitude[bin_index + 1U];
    denominator = left + (2.0f * center) + right;
    if (denominator > 0.0f) {
        /* Hann 窗三点插值，delta 表示峰值偏离整数 bin 的比例。 */
        delta = (2.0f * (right - left)) / denominator;
        if (delta < -0.5f) {
            delta = -0.5f;
        } else if (delta > 0.5f) {
            delta = 0.5f;
        }
    }

    /* 用 Hann 主瓣响应补回非整数 bin 造成的幅值损失。 */
    response = FFT_HannResponse(delta);
    *frequency_hz = ((float)bin_index + delta) * sample_rate_hz /
                    (float)FFT_LENGTH;
    *amplitude_code = (response > 0.0f) ? (center / response) : center;
    return 1U;
}

void FFT_Process(const uint16_t adc_data[FFT_LENGTH], float sample_rate_hz)
{
    uint32_t sum = 0U;
    uint32_t first_bin;
    uint32_t last_bin;
    float mean;

    FFT_ClearResult();
    if ((adc_data == NULL) || (sample_rate_hz <= 0.0f)) {
        return;
    }

    /* 第 1 步：求整帧平均值（直流分量）。 */
    for (uint32_t index = 0U; index < FFT_LENGTH; ++index) {
        sum += adc_data[index] & 0x0FFFU;
    }
    mean = (float)sum / (float)FFT_LENGTH;

    /* 第 2 步：保留直流均值，仅给交流分量乘 Hann 窗。 */
    FFT_InitHannWindow();
    for (uint32_t index = 0U; index < FFT_LENGTH; ++index) {
        const float sample = (float)(adc_data[index] & 0x0FFFU);

        FFT_Buffer[2U * index] =
            mean + ((sample - mean) * FFT_HannWindow[index]);
        FFT_Buffer[(2U * index) + 1U] = 0.0f;
    }

    /* 第 3 步：执行 4096 点正向 CFFT，并转换为单边幅值谱。 */
    arm_cfft_f32(&arm_cfft_sR_f32_len4096, FFT_Buffer, 0U, 1U);
    arm_cmplx_mag_f32(FFT_Buffer, FFT_Magnitude, FFT_BIN_COUNT);

    /* 第 4 步：交流 bin 按 2/N 缩放，并补偿 Hann 相干增益。 */
    FFT_Magnitude[0] /= (float)FFT_LENGTH;
    for (uint32_t bin = 1U; bin < (FFT_BIN_COUNT - 1U); ++bin) {
        FFT_Magnitude[bin] =
            (FFT_Magnitude[bin] * 2.0f) /
            ((float)FFT_LENGTH * FFT_HannCoherentGain);
    }
    FFT_Magnitude[FFT_BIN_COUNT - 1U] /= (float)FFT_LENGTH;

    first_bin = (uint32_t)ceilf((FFT_MIN_FREQUENCY_HZ *
                                 (float)FFT_LENGTH) / sample_rate_hz);
    last_bin = (uint32_t)floorf((FFT_MAX_FREQUENCY_HZ *
                                (float)FFT_LENGTH) / sample_rate_hz);
    if (first_bin < 1U) {
        first_bin = 1U;
    }
    if (last_bin >= (FFT_BIN_COUNT - 1U)) {
        last_bin = FFT_BIN_COUNT - 2U;
    }

    /* 第 5 步：只在题目规定的 10 kHz～500 kHz 内找最大局部峰。 */
    for (uint32_t bin = first_bin; bin <= last_bin; ++bin) {
        const float magnitude = FFT_Magnitude[bin];

        if ((magnitude >= FFT_Magnitude[bin - 1U]) &&
            (magnitude > FFT_Magnitude[bin + 1U]) &&
            (magnitude > FFT_PeakAmplitude)) {
            FFT_PeakIndex = bin;
            FFT_PeakAmplitude = magnitude;
        }
    }

    /* 第 6 步：对主峰做三点频率和幅值插值。 */
    if ((FFT_PeakIndex != 0U) &&
        (FFT_PeakAmplitude >= FFT_MIN_PEAK_AMPLITUDE_CODE)) {
        (void)FFT_GetInterpolatedPeak(FFT_PeakIndex,
                                      sample_rate_hz,
                                      &FFT_PeakFrequency,
                                      &FFT_PeakAmplitude);
    } else {
        FFT_PeakIndex = 0U;
        FFT_PeakAmplitude = 0.0f;
    }
}
