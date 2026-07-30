#include "fft.h"

#include <math.h>
#include <stddef.h>

#include "arm_const_structs.h"
#include "arm_math.h"

#define FFT_PI 3.14159265358979323846f
#define FFT_MIN_PEAK_AMPLITUDE_CODE 1.0f
#define FFT_CCMRAM __attribute__((section(".ccmram"), aligned(4)))
#define FFT_COMPLEX_LENGTH (FFT_LENGTH / 2U)
#define FFT_HANN_COHERENT_GAIN \
    ((float)(FFT_LENGTH - 1U) / (2.0f * (float)FFT_LENGTH))

/* 8192 点实数序列按偶/奇样本打包为 4096 点复数序列，保持 CCMRAM 为 32 KB。 */
static float FFT_Buffer[FFT_LENGTH] FFT_CCMRAM;

_Static_assert(FFT_COMPLEX_LENGTH == 4096U,
               "当前实数 FFT 打包实现要求内部 CFFT 长度为 4096");

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

static void FFT_NormalizeOscillator(float *cosine, float *sine)
{
    const float norm = sqrtf((*cosine * *cosine) + (*sine * *sine));

    if (norm > 0.0f) {
        *cosine /= norm;
        *sine /= norm;
    }
}

static void FFT_StepOscillator(float step_cos,
                               float step_sin,
                               float *cosine,
                               float *sine)
{
    const float next_cos = (*cosine * step_cos) - (*sine * step_sin);
    const float next_sin = (*sine * step_cos) + (*cosine * step_sin);

    *cosine = next_cos;
    *sine = next_sin;
}

static void FFT_ConvertPackedRealSpectrum(void)
{
    const float ac_scale = 2.0f /
                           ((float)FFT_LENGTH * FFT_HANN_COHERENT_GAIN);
    const float angle_step = (2.0f * FFT_PI) / (float)FFT_LENGTH;
    const float step_cos = cosf(angle_step);
    const float step_sin = sinf(angle_step);
    float angle_cos = step_cos;
    float angle_sin = step_sin;
    const float dc_real = FFT_Buffer[0] + FFT_Buffer[1];
    const float nyquist_real = FFT_Buffer[0] - FFT_Buffer[1];

    FFT_Magnitude[0] = fabsf(dc_real) / (float)FFT_LENGTH;
    FFT_Magnitude[FFT_BIN_COUNT - 1U] =
        fabsf(nyquist_real) / (float)FFT_LENGTH;

    for (uint32_t bin = 1U; bin < FFT_COMPLEX_LENGTH; ++bin) {
        const uint32_t mirror = FFT_COMPLEX_LENGTH - bin;
        const float real_a = FFT_Buffer[2U * bin];
        const float imag_a = FFT_Buffer[(2U * bin) + 1U];
        const float real_mirror = FFT_Buffer[2U * mirror];
        const float imag_mirror = FFT_Buffer[(2U * mirror) + 1U];
        const float real_difference = real_a - real_mirror;
        const float imag_sum = imag_a + imag_mirror;
        const float real = 0.5f *
            ((real_a + real_mirror) -
             (angle_sin * real_difference) +
             (angle_cos * imag_sum));
        const float imag = 0.5f *
            ((imag_a - imag_mirror) -
             (angle_sin * imag_sum) -
             (angle_cos * real_difference));

        FFT_Magnitude[bin] = sqrtf((real * real) + (imag * imag)) *
                             ac_scale;
        FFT_StepOscillator(step_cos,
                           step_sin,
                           &angle_cos,
                           &angle_sin);
        if ((bin & 0xFFU) == 0U) {
            FFT_NormalizeOscillator(&angle_cos, &angle_sin);
        }
    }
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

uint8_t FFT_EstimateTone(const uint16_t adc_data[FFT_LENGTH],
                         float sample_rate_hz,
                         float frequency_hz,
                         float *amplitude_code,
                         float *phase_rad)
{
    float sum_weight = 0.0f;
    float sum_sample = 0.0f;
    float sum_cos = 0.0f;
    float sum_sin = 0.0f;
    float sum_cos_cos = 0.0f;
    float sum_sin_sin = 0.0f;
    float sum_cos_sin = 0.0f;
    float sum_sample_cos = 0.0f;
    float sum_sample_sin = 0.0f;
    float oscillator_cos = 1.0f;
    float oscillator_sin = 0.0f;
    float window_cos = 1.0f;
    float window_sin = 0.0f;
    float step_cos;
    float step_sin;
    float window_step_cos;
    float window_step_sin;
    float centered_cos_cos;
    float centered_sin_sin;
    float centered_cos_sin;
    float centered_sample_cos;
    float centered_sample_sin;
    float determinant;
    float cosine_coefficient;
    float sine_coefficient;

    if ((adc_data == NULL) || (amplitude_code == NULL) ||
        (phase_rad == NULL) || (sample_rate_hz <= 0.0f) ||
        (frequency_hz <= 0.0f) ||
        (frequency_hz >= (0.5f * sample_rate_hz))) {
        return 0U;
    }

    step_cos = cosf((2.0f * FFT_PI * frequency_hz) / sample_rate_hz);
    step_sin = sinf((2.0f * FFT_PI * frequency_hz) / sample_rate_hz);
    window_step_cos = cosf((2.0f * FFT_PI) /
                           (float)(FFT_LENGTH - 1U));
    window_step_sin = sinf((2.0f * FFT_PI) /
                           (float)(FFT_LENGTH - 1U));

    for (uint32_t index = 0U; index < FFT_LENGTH; ++index) {
        const float weight = 0.5f - (0.5f * window_cos);
        const float sample = (float)(adc_data[index] & 0x0FFFU);

        sum_weight += weight;
        sum_sample += weight * sample;
        sum_cos += weight * oscillator_cos;
        sum_sin += weight * oscillator_sin;
        sum_cos_cos += weight * oscillator_cos * oscillator_cos;
        sum_sin_sin += weight * oscillator_sin * oscillator_sin;
        sum_cos_sin += weight * oscillator_cos * oscillator_sin;
        sum_sample_cos += weight * sample * oscillator_cos;
        sum_sample_sin += weight * sample * oscillator_sin;

        FFT_StepOscillator(step_cos,
                           step_sin,
                           &oscillator_cos,
                           &oscillator_sin);
        FFT_StepOscillator(window_step_cos,
                           window_step_sin,
                           &window_cos,
                           &window_sin);

        /* 限制递推振荡器的浮点模长漂移。 */
        if ((index & 0xFFU) == 0xFFU) {
            FFT_NormalizeOscillator(&oscillator_cos, &oscillator_sin);
            FFT_NormalizeOscillator(&window_cos, &window_sin);
        }
    }

    if (sum_weight <= 0.0f) {
        return 0U;
    }

    /* 消去直流列后求解 2x2 加权正规方程。 */
    centered_cos_cos = sum_cos_cos - ((sum_cos * sum_cos) / sum_weight);
    centered_sin_sin = sum_sin_sin - ((sum_sin * sum_sin) / sum_weight);
    centered_cos_sin = sum_cos_sin - ((sum_cos * sum_sin) / sum_weight);
    centered_sample_cos =
        sum_sample_cos - ((sum_sample * sum_cos) / sum_weight);
    centered_sample_sin =
        sum_sample_sin - ((sum_sample * sum_sin) / sum_weight);
    determinant = (centered_cos_cos * centered_sin_sin) -
                  (centered_cos_sin * centered_cos_sin);
    if (fabsf(determinant) < 0.001f) {
        return 0U;
    }

    cosine_coefficient =
        ((centered_sample_cos * centered_sin_sin) -
         (centered_sample_sin * centered_cos_sin)) /
        determinant;
    sine_coefficient =
        ((centered_sample_sin * centered_cos_cos) -
         (centered_sample_cos * centered_cos_sin)) /
        determinant;

    *amplitude_code = sqrtf((cosine_coefficient * cosine_coefficient) +
                            (sine_coefficient * sine_coefficient));
    *phase_rad = atan2f(-sine_coefficient, cosine_coefficient);
    return (isfinite(*amplitude_code) != 0) ? 1U : 0U;
}

void FFT_Process(const uint16_t adc_data[FFT_LENGTH],
                 float sample_rate_hz,
                 float mean_code)
{
    uint32_t first_bin;
    uint32_t last_bin;
    const float window_step_angle =
        (2.0f * FFT_PI) / (float)(FFT_LENGTH - 1U);
    const float window_step_cos = cosf(window_step_angle);
    const float window_step_sin = sinf(window_step_angle);
    float window_cos = 1.0f;
    float window_sin = 0.0f;

    FFT_ClearResult();
    if ((adc_data == NULL) || (sample_rate_hz <= 0.0f) ||
        (isfinite(mean_code) == 0)) {
        return;
    }

    /* 第 1 步：保留调用方给出的直流均值，仅给交流分量乘 Hann 窗。 */
    for (uint32_t index = 0U; index < FFT_LENGTH; ++index) {
        const float sample = (float)(adc_data[index] & 0x0FFFU);
        const float weight = 0.5f - (0.5f * window_cos);

        FFT_Buffer[index] = mean_code + ((sample - mean_code) * weight);
        FFT_StepOscillator(window_step_cos,
                           window_step_sin,
                           &window_cos,
                           &window_sin);
        if ((index & 0xFFU) == 0xFFU) {
            FFT_NormalizeOscillator(&window_cos, &window_sin);
        }
    }

    /* 第 2 步：偶/奇样本打包后执行 4096 点 CFFT，解包为 8192 点实数频谱。 */
    arm_cfft_f32(&arm_cfft_sR_f32_len4096, FFT_Buffer, 0U, 1U);
    FFT_ConvertPackedRealSpectrum();

    /* 第 3 步：确定量程对应的候选 bin。 */
    first_bin = (uint32_t)((FFT_MIN_FREQUENCY_HZ *
                            (float)FFT_LENGTH) / sample_rate_hz);
    /* 上限向上取整，避免 500 kHz 位于半个 bin 以上时漏掉主峰。 */
    last_bin = (uint32_t)ceilf((FFT_MAX_FREQUENCY_HZ *
                               (float)FFT_LENGTH) / sample_rate_hz);
    if (first_bin < 1U) {
        first_bin = 1U;
    }
    if (last_bin >= (FFT_BIN_COUNT - 1U)) {
        last_bin = FFT_BIN_COUNT - 2U;
    }

    /* 第 4 步：只在题目规定的 10 kHz～500 kHz 内找最大局部峰。 */
    for (uint32_t bin = first_bin; bin <= last_bin; ++bin) {
        const float magnitude = FFT_Magnitude[bin];

        if ((magnitude >= FFT_Magnitude[bin - 1U]) &&
            (magnitude > FFT_Magnitude[bin + 1U]) &&
            (magnitude > FFT_PeakAmplitude)) {
            FFT_PeakIndex = bin;
            FFT_PeakAmplitude = magnitude;
        }
    }

    /* 第 5 步：对主峰做三点频率和幅值插值。 */
    if ((FFT_PeakIndex != 0U) &&
        (FFT_PeakAmplitude >= FFT_MIN_PEAK_AMPLITUDE_CODE)) {
        (void)FFT_GetInterpolatedPeak(FFT_PeakIndex,
                                      sample_rate_hz,
                                      &FFT_PeakFrequency,
                                      &FFT_PeakAmplitude);
        if ((FFT_PeakFrequency <
             (FFT_MIN_FREQUENCY_HZ - FFT_FREQUENCY_RANGE_TOLERANCE_HZ)) ||
            (FFT_PeakFrequency >
             (FFT_MAX_FREQUENCY_HZ + FFT_FREQUENCY_RANGE_TOLERANCE_HZ))) {
            FFT_PeakIndex = 0U;
            FFT_PeakFrequency = 0.0f;
            FFT_PeakAmplitude = 0.0f;
        } else if (FFT_PeakFrequency < FFT_MIN_FREQUENCY_HZ) {
            FFT_PeakFrequency = FFT_MIN_FREQUENCY_HZ;
        } else if (FFT_PeakFrequency > FFT_MAX_FREQUENCY_HZ) {
            FFT_PeakFrequency = FFT_MAX_FREQUENCY_HZ;
        }
    } else {
        FFT_PeakIndex = 0U;
        FFT_PeakFrequency = 0.0f;
        FFT_PeakAmplitude = 0.0f;
    }
}
