#include "waveform_analyzer_service.h"

#include <math.h>
#include <stddef.h>

#include "arm_math.h"

#define WAVEFORM_ANALYZER_ADC_MAX_CODE 4095U
#define WAVEFORM_ANALYZER_MIN_SIGNAL_P2P_CODE 24U
#define WAVEFORM_ANALYZER_MIN_FUNDAMENTAL_CODE 8.0f
#define WAVEFORM_ANALYZER_CLIP_LOW_CODE 2U
#define WAVEFORM_ANALYZER_CLIP_HIGH_CODE 4093U
#define WAVEFORM_ANALYZER_PI 3.14159265358979323846f
#define WAVEFORM_ANALYZER_HANN_COHERENT_GAIN 0.5f
#define WAVEFORM_ANALYZER_HARMONIC_SEARCH_RADIUS 1U
#define WAVEFORM_ANALYZER_CCMRAM __attribute__((section(".ccmram"), aligned(4)))

typedef struct {
    arm_rfft_fast_instance_f32 fft;
    uint8_t initialized;
    uint32_t sample_rate_hz;
} waveform_analyzer_context_t;

static waveform_analyzer_context_t g_waveform_analyzer;
static waveform_analyzer_result_t g_waveform_analyzer_result;
static float g_waveform_analyzer_window[WAVEFORM_ANALYZER_FFT_SIZE];
static float g_waveform_analyzer_fft_input[WAVEFORM_ANALYZER_FFT_SIZE]
    WAVEFORM_ANALYZER_CCMRAM;
static float g_waveform_analyzer_fft_output[WAVEFORM_ANALYZER_FFT_SIZE]
    WAVEFORM_ANALYZER_CCMRAM;

static float waveform_analyzer_clamp_percent(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 100.0f) {
        return 100.0f;
    }

    return value;
}

static void waveform_analyzer_init_window(void)
{
    for (uint32_t index = 0U; index < WAVEFORM_ANALYZER_FFT_SIZE; ++index) {
        const float phase =
            (2.0f * WAVEFORM_ANALYZER_PI * (float)index) /
            (float)(WAVEFORM_ANALYZER_FFT_SIZE - 1U);
        g_waveform_analyzer_window[index] = 0.5f - (0.5f * cosf(phase));
    }
}

static void waveform_analyzer_clear_result(uint32_t sample_rate_hz)
{
    g_waveform_analyzer_result.initialized = 0U;
    g_waveform_analyzer_result.result_ready = 0U;
    g_waveform_analyzer_result.analysis_count = 0U;
    g_waveform_analyzer_result.sample_rate_hz = sample_rate_hz;
    g_waveform_analyzer_result.bin_resolution_hz =
        (sample_rate_hz == 0U) ? 0.0f :
            ((float)sample_rate_hz / (float)WAVEFORM_ANALYZER_FFT_SIZE);
    g_waveform_analyzer_result.waveform_type =
        WAVEFORM_ANALYZER_TYPE_UNKNOWN;
    g_waveform_analyzer_result.clipped_low = 0U;
    g_waveform_analyzer_result.clipped_high = 0U;
    g_waveform_analyzer_result.min_code = 0U;
    g_waveform_analyzer_result.max_code = 0U;
    g_waveform_analyzer_result.average_code = 0U;
    g_waveform_analyzer_result.peak_to_peak_code = 0U;
    g_waveform_analyzer_result.dc_code = 0.0f;
    g_waveform_analyzer_result.rms_code = 0.0f;
    g_waveform_analyzer_result.fundamental_bin = 0U;
    g_waveform_analyzer_result.fundamental_frequency_hz = 0.0f;
    g_waveform_analyzer_result.fundamental_amplitude_code = 0.0f;
    g_waveform_analyzer_result.harmonic2_percent = 0.0f;
    g_waveform_analyzer_result.harmonic3_percent = 0.0f;
    g_waveform_analyzer_result.harmonic4_percent = 0.0f;
    g_waveform_analyzer_result.harmonic5_percent = 0.0f;
    g_waveform_analyzer_result.thd_percent = 0.0f;
    g_waveform_analyzer_result.duty_percent = 0.0f;
    for (uint8_t index = 0U; index < WAVEFORM_ANALYZER_PEAK_COUNT; ++index) {
        g_waveform_analyzer_result.peak_bins[index] = 0U;
        g_waveform_analyzer_result.peak_frequencies_hz[index] = 0.0f;
        g_waveform_analyzer_result.peak_amplitudes_code[index] = 0.0f;
    }
}

static float waveform_analyzer_get_bin_magnitude(uint16_t bin_index)
{
    if (bin_index >= WAVEFORM_ANALYZER_BIN_COUNT) {
        return 0.0f;
    }

    return g_waveform_analyzer_fft_output[bin_index];
}

static float waveform_analyzer_get_near_harmonic_magnitude(uint16_t center_bin)
{
    float max_magnitude = 0.0f;
    uint16_t start_bin;
    uint16_t end_bin;

    if ((center_bin == 0U) || (center_bin >= WAVEFORM_ANALYZER_BIN_COUNT)) {
        return 0.0f;
    }

    start_bin = (center_bin > WAVEFORM_ANALYZER_HARMONIC_SEARCH_RADIUS) ?
                    (uint16_t)(center_bin -
                               WAVEFORM_ANALYZER_HARMONIC_SEARCH_RADIUS) :
                    1U;
    end_bin = (uint16_t)(center_bin +
                         WAVEFORM_ANALYZER_HARMONIC_SEARCH_RADIUS);
    if (end_bin >= WAVEFORM_ANALYZER_BIN_COUNT) {
        end_bin = WAVEFORM_ANALYZER_BIN_COUNT - 1U;
    }

    for (uint16_t bin = start_bin; bin <= end_bin; ++bin) {
        const float magnitude = waveform_analyzer_get_bin_magnitude(bin);
        if (magnitude > max_magnitude) {
            max_magnitude = magnitude;
        }
    }

    return max_magnitude;
}

static void waveform_analyzer_build_magnitude(void)
{
    g_waveform_analyzer_fft_output[0] = 0.0f;

    /* 从低频到高频原地压缩，写入位置始终落后于尚未读取的复数数据。 */
    for (uint16_t bin = 1U; bin < WAVEFORM_ANALYZER_BIN_COUNT; ++bin) {
        const uint32_t offset = (uint32_t)bin * 2U;
        const float real = g_waveform_analyzer_fft_output[offset];
        const float imag = g_waveform_analyzer_fft_output[offset + 1U];
        const float raw_magnitude = sqrtf((real * real) + (imag * imag));

        g_waveform_analyzer_fft_output[bin] =
            (2.0f * raw_magnitude) /
            ((float)WAVEFORM_ANALYZER_FFT_SIZE *
             WAVEFORM_ANALYZER_HANN_COHERENT_GAIN);
    }
}

static uint16_t waveform_analyzer_find_fundamental_bin(void)
{
    float max_magnitude = 0.0f;
    uint16_t max_bin = 0U;

    for (uint16_t bin = 1U; bin < WAVEFORM_ANALYZER_BIN_COUNT; ++bin) {
        const float magnitude = waveform_analyzer_get_bin_magnitude(bin);
        if (magnitude > max_magnitude) {
            max_magnitude = magnitude;
            max_bin = bin;
        }
    }

    return max_bin;
}

static void waveform_analyzer_insert_peak(uint16_t bin, float magnitude)
{
    for (uint8_t index = 0U; index < WAVEFORM_ANALYZER_PEAK_COUNT; ++index) {
        if (magnitude >
            g_waveform_analyzer_result.peak_amplitudes_code[index]) {
            for (uint8_t move = (WAVEFORM_ANALYZER_PEAK_COUNT - 1U);
                 move > index;
                 --move) {
                g_waveform_analyzer_result.peak_bins[move] =
                    g_waveform_analyzer_result.peak_bins[move - 1U];
                g_waveform_analyzer_result.peak_frequencies_hz[move] =
                    g_waveform_analyzer_result.peak_frequencies_hz[move - 1U];
                g_waveform_analyzer_result.peak_amplitudes_code[move] =
                    g_waveform_analyzer_result
                        .peak_amplitudes_code[move - 1U];
            }

            g_waveform_analyzer_result.peak_bins[index] = bin;
            g_waveform_analyzer_result.peak_frequencies_hz[index] =
                (float)bin * g_waveform_analyzer_result.bin_resolution_hz;
            g_waveform_analyzer_result.peak_amplitudes_code[index] =
                magnitude;
            break;
        }
    }
}

static void waveform_analyzer_find_spectrum_peaks(void)
{
    const float noise_floor =
        g_waveform_analyzer_result.fundamental_amplitude_code * 0.02f;

    for (uint8_t index = 0U; index < WAVEFORM_ANALYZER_PEAK_COUNT; ++index) {
        g_waveform_analyzer_result.peak_bins[index] = 0U;
        g_waveform_analyzer_result.peak_frequencies_hz[index] = 0.0f;
        g_waveform_analyzer_result.peak_amplitudes_code[index] = 0.0f;
    }

    for (uint16_t bin = 2U; bin < (WAVEFORM_ANALYZER_BIN_COUNT - 1U); ++bin) {
        const float previous = waveform_analyzer_get_bin_magnitude(
            (uint16_t)(bin - 1U));
        const float current = waveform_analyzer_get_bin_magnitude(bin);
        const float next = waveform_analyzer_get_bin_magnitude(
            (uint16_t)(bin + 1U));

        if ((current > previous) &&
            (current >= next) &&
            (current >= noise_floor)) {
            waveform_analyzer_insert_peak(bin, current);
        }
    }
}

static float waveform_analyzer_ratio_percent(float value, float base)
{
    if (base <= 0.0f) {
        return 0.0f;
    }

    return waveform_analyzer_clamp_percent((value * 100.0f) / base);
}

static waveform_analyzer_type_t waveform_analyzer_classify(
    uint16_t peak_to_peak_code,
    float fundamental,
    float harmonic2_percent,
    float harmonic3_percent,
    float harmonic4_percent,
    float harmonic5_percent)
{
    if ((peak_to_peak_code < WAVEFORM_ANALYZER_MIN_SIGNAL_P2P_CODE) ||
        (fundamental < WAVEFORM_ANALYZER_MIN_FUNDAMENTAL_CODE)) {
        return WAVEFORM_ANALYZER_TYPE_DC;
    }

    if ((harmonic2_percent < 8.0f) &&
        (harmonic3_percent < 8.0f) &&
        (harmonic4_percent < 8.0f) &&
        (harmonic5_percent < 8.0f)) {
        return WAVEFORM_ANALYZER_TYPE_SINE;
    }

    if ((harmonic2_percent < 18.0f) &&
        (harmonic3_percent > 18.0f) &&
        (harmonic5_percent > 8.0f)) {
        return WAVEFORM_ANALYZER_TYPE_SQUARE;
    }

    if ((harmonic2_percent < 12.0f) &&
        (harmonic3_percent > 5.0f) &&
        (harmonic3_percent < 18.0f) &&
        (harmonic5_percent < 8.0f)) {
        return WAVEFORM_ANALYZER_TYPE_TRIANGLE;
    }

    if ((harmonic2_percent > 18.0f) &&
        (harmonic3_percent > 10.0f) &&
        (harmonic4_percent > 6.0f)) {
        return WAVEFORM_ANALYZER_TYPE_SAWTOOTH;
    }

    return WAVEFORM_ANALYZER_TYPE_UNKNOWN;
}

HAL_StatusTypeDef Waveform_Analyzer_Init(uint32_t sample_rate_hz)
{
    arm_status status;

    if (sample_rate_hz == 0U) {
        return HAL_ERROR;
    }

    status = arm_rfft_fast_init_f32(&g_waveform_analyzer.fft,
                                    WAVEFORM_ANALYZER_FFT_SIZE);
    if (status != ARM_MATH_SUCCESS) {
        g_waveform_analyzer.initialized = 0U;
        waveform_analyzer_clear_result(sample_rate_hz);
        return HAL_ERROR;
    }

    g_waveform_analyzer.sample_rate_hz = sample_rate_hz;
    waveform_analyzer_init_window();
    waveform_analyzer_clear_result(sample_rate_hz);
    g_waveform_analyzer.initialized = 1U;
    g_waveform_analyzer_result.initialized = 1U;

    return HAL_OK;
}

HAL_StatusTypeDef Waveform_Analyzer_ProcessBlock(const uint16_t *samples,
                                                 uint32_t length)
{
    uint16_t min_code = WAVEFORM_ANALYZER_ADC_MAX_CODE;
    uint16_t max_code = 0U;
    uint32_t sum = 0U;
    uint32_t positive_count = 0U;
    float dc_code;
    float square_sum = 0.0f;
    uint16_t fundamental_bin;
    float fundamental_magnitude;
    float harmonic2;
    float harmonic3;
    float harmonic4;
    float harmonic5;

    if ((g_waveform_analyzer.initialized == 0U) ||
        (samples == NULL) ||
        (length < WAVEFORM_ANALYZER_FFT_SIZE)) {
        return HAL_ERROR;
    }

    for (uint32_t index = 0U; index < WAVEFORM_ANALYZER_FFT_SIZE; ++index) {
        const uint16_t sample = samples[index] & WAVEFORM_ANALYZER_ADC_MAX_CODE;
        if (sample < min_code) {
            min_code = sample;
        }
        if (sample > max_code) {
            max_code = sample;
        }
        sum += sample;
    }

    dc_code = (float)sum / (float)WAVEFORM_ANALYZER_FFT_SIZE;

    for (uint32_t index = 0U; index < WAVEFORM_ANALYZER_FFT_SIZE; ++index) {
        const float centered_sample = (float)(samples[index] &
                                             WAVEFORM_ANALYZER_ADC_MAX_CODE) -
                                      dc_code;
        if (centered_sample >= 0.0f) {
            positive_count++;
        }
        square_sum += centered_sample * centered_sample;
        g_waveform_analyzer_fft_input[index] =
            centered_sample * g_waveform_analyzer_window[index];
    }

    arm_rfft_fast_f32(&g_waveform_analyzer.fft,
                      g_waveform_analyzer_fft_input,
                      g_waveform_analyzer_fft_output,
                      0U);
    waveform_analyzer_build_magnitude();

    fundamental_bin = waveform_analyzer_find_fundamental_bin();
    fundamental_magnitude =
        waveform_analyzer_get_bin_magnitude(fundamental_bin);
    harmonic2 = waveform_analyzer_get_near_harmonic_magnitude(
        (uint16_t)(fundamental_bin * 2U));
    harmonic3 = waveform_analyzer_get_near_harmonic_magnitude(
        (uint16_t)(fundamental_bin * 3U));
    harmonic4 = waveform_analyzer_get_near_harmonic_magnitude(
        (uint16_t)(fundamental_bin * 4U));
    harmonic5 = waveform_analyzer_get_near_harmonic_magnitude(
        (uint16_t)(fundamental_bin * 5U));

    g_waveform_analyzer_result.initialized = 1U;
    g_waveform_analyzer_result.result_ready = 1U;
    g_waveform_analyzer_result.analysis_count++;
    g_waveform_analyzer_result.sample_rate_hz =
        g_waveform_analyzer.sample_rate_hz;
    g_waveform_analyzer_result.bin_resolution_hz =
        (float)g_waveform_analyzer.sample_rate_hz /
        (float)WAVEFORM_ANALYZER_FFT_SIZE;
    g_waveform_analyzer_result.min_code = min_code;
    g_waveform_analyzer_result.max_code = max_code;
    g_waveform_analyzer_result.clipped_low =
        (min_code <= WAVEFORM_ANALYZER_CLIP_LOW_CODE) ? 1U : 0U;
    g_waveform_analyzer_result.clipped_high =
        (max_code >= WAVEFORM_ANALYZER_CLIP_HIGH_CODE) ? 1U : 0U;
    g_waveform_analyzer_result.average_code =
        (uint16_t)((sum + (WAVEFORM_ANALYZER_FFT_SIZE / 2U)) /
                   WAVEFORM_ANALYZER_FFT_SIZE);
    g_waveform_analyzer_result.peak_to_peak_code =
        (uint16_t)(max_code - min_code);
    g_waveform_analyzer_result.dc_code = dc_code;
    g_waveform_analyzer_result.rms_code =
        sqrtf(square_sum / (float)WAVEFORM_ANALYZER_FFT_SIZE);
    g_waveform_analyzer_result.fundamental_bin = fundamental_bin;
    g_waveform_analyzer_result.fundamental_frequency_hz =
        (float)fundamental_bin *
        g_waveform_analyzer_result.bin_resolution_hz;
    g_waveform_analyzer_result.fundamental_amplitude_code =
        fundamental_magnitude;
    g_waveform_analyzer_result.harmonic2_percent =
        waveform_analyzer_ratio_percent(harmonic2, fundamental_magnitude);
    g_waveform_analyzer_result.harmonic3_percent =
        waveform_analyzer_ratio_percent(harmonic3, fundamental_magnitude);
    g_waveform_analyzer_result.harmonic4_percent =
        waveform_analyzer_ratio_percent(harmonic4, fundamental_magnitude);
    g_waveform_analyzer_result.harmonic5_percent =
        waveform_analyzer_ratio_percent(harmonic5, fundamental_magnitude);
    g_waveform_analyzer_result.thd_percent =
        waveform_analyzer_ratio_percent(
            sqrtf((harmonic2 * harmonic2) +
                  (harmonic3 * harmonic3) +
                  (harmonic4 * harmonic4) +
                  (harmonic5 * harmonic5)),
            fundamental_magnitude);
    g_waveform_analyzer_result.duty_percent =
        waveform_analyzer_clamp_percent(
            ((float)positive_count * 100.0f) /
            (float)WAVEFORM_ANALYZER_FFT_SIZE);
    waveform_analyzer_find_spectrum_peaks();
    g_waveform_analyzer_result.waveform_type =
        waveform_analyzer_classify(
            g_waveform_analyzer_result.peak_to_peak_code,
            fundamental_magnitude,
            g_waveform_analyzer_result.harmonic2_percent,
            g_waveform_analyzer_result.harmonic3_percent,
            g_waveform_analyzer_result.harmonic4_percent,
            g_waveform_analyzer_result.harmonic5_percent);

    return HAL_OK;
}

const waveform_analyzer_result_t *Waveform_Analyzer_GetResult(void)
{
    return &g_waveform_analyzer_result;
}

const float *Waveform_Analyzer_GetMagnitudeBins(void)
{
    return g_waveform_analyzer_fft_output;
}

float Waveform_Analyzer_GetMagnitudeAtBin(uint16_t bin_index)
{
    return waveform_analyzer_get_bin_magnitude(bin_index);
}
