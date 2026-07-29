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
#define WAVEFORM_ANALYZER_HARMONIC_SEARCH_RADIUS 1U
#define WAVEFORM_ANALYZER_MIN_FREQUENCY_HZ 10000.0f
#define WAVEFORM_ANALYZER_MAX_FREQUENCY_HZ 500000.0f
#define WAVEFORM_ANALYZER_MIN_PEAK_CODE 1.0f
#define WAVEFORM_ANALYZER_RELATIVE_PEAK_THRESHOLD 0.005f
#define WAVEFORM_ANALYZER_PEAK_GUARD_BINS 8U
#define WAVEFORM_ANALYZER_EXTREME_AVERAGE_COUNT 8U
#define WAVEFORM_ANALYZER_CCMRAM __attribute__((section(".ccmram"), aligned(4)))

typedef struct {
    arm_rfft_fast_instance_f32 fft;
    uint8_t initialized;
    uint32_t sample_rate_hz;
    float window_sum;
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
    g_waveform_analyzer.window_sum = 0.0f;
    for (uint32_t index = 0U; index < WAVEFORM_ANALYZER_FFT_SIZE; ++index) {
        const float phase =
            (2.0f * WAVEFORM_ANALYZER_PI * (float)index) /
            (float)(WAVEFORM_ANALYZER_FFT_SIZE - 1U);
        g_waveform_analyzer_window[index] = 0.5f - (0.5f * cosf(phase));
        g_waveform_analyzer.window_sum +=
            g_waveform_analyzer_window[index];
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
    g_waveform_analyzer_result.peak_count = 0U;
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
            (g_waveform_analyzer.window_sum > 0.0f) ?
                ((2.0f * raw_magnitude) /
                 g_waveform_analyzer.window_sum) :
                0.0f;
    }
}

static uint8_t waveform_analyzer_is_local_peak(uint16_t bin)
{
    const float previous = waveform_analyzer_get_bin_magnitude(
        (uint16_t)(bin - 1U));
    const float current = waveform_analyzer_get_bin_magnitude(bin);
    const float next = waveform_analyzer_get_bin_magnitude(
        (uint16_t)(bin + 1U));

    return ((current > previous) && (current >= next)) ? 1U : 0U;
}

static uint8_t waveform_analyzer_peak_is_guarded(uint16_t bin,
                                                 uint8_t selected_count)
{
    for (uint8_t index = 0U; index < selected_count; ++index) {
        const uint16_t selected_bin =
            g_waveform_analyzer_result.peak_bins[index];
        const uint16_t distance = (bin >= selected_bin) ?
                                      (uint16_t)(bin - selected_bin) :
                                      (uint16_t)(selected_bin - bin);
        if (distance <= WAVEFORM_ANALYZER_PEAK_GUARD_BINS) {
            return 1U;
        }
    }

    return 0U;
}

static float waveform_analyzer_hann_response(float delta)
{
    float sinc;
    float denominator;

    if (fabsf(delta) < 0.000001f) {
        return 1.0f;
    }

    sinc = sinf(WAVEFORM_ANALYZER_PI * delta) /
           (WAVEFORM_ANALYZER_PI * delta);
    denominator = 1.0f - (delta * delta);
    if (fabsf(denominator) < 0.000001f) {
        return 1.0f;
    }

    return fabsf(sinc / denominator);
}

static void waveform_analyzer_store_interpolated_peak(uint8_t index,
                                                       uint16_t bin)
{
    const float left = waveform_analyzer_get_bin_magnitude(
        (uint16_t)(bin - 1U));
    const float center = waveform_analyzer_get_bin_magnitude(bin);
    const float right = waveform_analyzer_get_bin_magnitude(
        (uint16_t)(bin + 1U));
    const float denominator = left + (2.0f * center) + right;
    float delta = 0.0f;
    float response;

    if (denominator > 0.0f) {
        /* Hann 窗三点插值，补偿非整周期采样的栅栏与幅值损失。 */
        delta = (2.0f * (right - left)) / denominator;
        if (delta < -0.5f) {
            delta = -0.5f;
        } else if (delta > 0.5f) {
            delta = 0.5f;
        }
    }

    response = waveform_analyzer_hann_response(delta);
    g_waveform_analyzer_result.peak_bins[index] = bin;
    g_waveform_analyzer_result.peak_frequencies_hz[index] =
        ((float)bin + delta) *
        g_waveform_analyzer_result.bin_resolution_hz;
    g_waveform_analyzer_result.peak_amplitudes_code[index] =
        (response > 0.0f) ? (center / response) : center;
}

static void waveform_analyzer_sort_peaks_by_frequency(void)
{
    for (uint8_t index = 1U;
         index < g_waveform_analyzer_result.peak_count;
         ++index) {
        uint8_t move = index;
        while ((move > 0U) &&
               (g_waveform_analyzer_result.peak_frequencies_hz[move] <
                g_waveform_analyzer_result
                    .peak_frequencies_hz[move - 1U])) {
            const uint16_t bin =
                g_waveform_analyzer_result.peak_bins[move - 1U];
            const float frequency =
                g_waveform_analyzer_result
                    .peak_frequencies_hz[move - 1U];
            const float amplitude =
                g_waveform_analyzer_result
                    .peak_amplitudes_code[move - 1U];

            g_waveform_analyzer_result.peak_bins[move - 1U] =
                g_waveform_analyzer_result.peak_bins[move];
            g_waveform_analyzer_result.peak_frequencies_hz[move - 1U] =
                g_waveform_analyzer_result.peak_frequencies_hz[move];
            g_waveform_analyzer_result.peak_amplitudes_code[move - 1U] =
                g_waveform_analyzer_result.peak_amplitudes_code[move];
            g_waveform_analyzer_result.peak_bins[move] = bin;
            g_waveform_analyzer_result.peak_frequencies_hz[move] =
                frequency;
            g_waveform_analyzer_result.peak_amplitudes_code[move] =
                amplitude;
            --move;
        }
    }
}

static void waveform_analyzer_find_spectrum_peaks(void)
{
    const float bin_resolution =
        g_waveform_analyzer_result.bin_resolution_hz;
    uint16_t first_bin;
    uint16_t last_bin;
    float strongest_magnitude = 0.0f;

    g_waveform_analyzer_result.peak_count = 0U;
    for (uint8_t index = 0U; index < WAVEFORM_ANALYZER_PEAK_COUNT; ++index) {
        g_waveform_analyzer_result.peak_bins[index] = 0U;
        g_waveform_analyzer_result.peak_frequencies_hz[index] = 0.0f;
        g_waveform_analyzer_result.peak_amplitudes_code[index] = 0.0f;
    }

    if (bin_resolution <= 0.0f) {
        return;
    }

    first_bin = (uint16_t)(WAVEFORM_ANALYZER_MIN_FREQUENCY_HZ /
                           bin_resolution);
    if (((float)first_bin * bin_resolution) <
        WAVEFORM_ANALYZER_MIN_FREQUENCY_HZ) {
        first_bin++;
    }
    if (first_bin < 2U) {
        first_bin = 2U;
    }

    last_bin = (uint16_t)(WAVEFORM_ANALYZER_MAX_FREQUENCY_HZ /
                          bin_resolution);
    if (last_bin >= (WAVEFORM_ANALYZER_BIN_COUNT - 1U)) {
        last_bin = WAVEFORM_ANALYZER_BIN_COUNT - 2U;
    }
    if (last_bin < first_bin) {
        return;
    }

    for (uint8_t selected = 0U;
         selected < WAVEFORM_ANALYZER_PEAK_COUNT;
         ++selected) {
        float best_magnitude = 0.0f;
        uint16_t best_bin = 0U;

        for (uint16_t bin = first_bin; bin <= last_bin; ++bin) {
            const float magnitude =
                waveform_analyzer_get_bin_magnitude(bin);
            if ((magnitude > best_magnitude) &&
                (waveform_analyzer_is_local_peak(bin) != 0U) &&
                (waveform_analyzer_peak_is_guarded(bin, selected) == 0U)) {
                best_magnitude = magnitude;
                best_bin = bin;
            }
        }

        if (selected == 0U) {
            strongest_magnitude = best_magnitude;
        }
        if ((best_bin == 0U) ||
            (best_magnitude < WAVEFORM_ANALYZER_MIN_PEAK_CODE) ||
            ((selected > 0U) &&
             (best_magnitude <
              (strongest_magnitude *
               WAVEFORM_ANALYZER_RELATIVE_PEAK_THRESHOLD)))) {
            break;
        }

        waveform_analyzer_store_interpolated_peak(selected, best_bin);
        g_waveform_analyzer_result.peak_count++;
    }

    waveform_analyzer_sort_peaks_by_frequency();
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

static void waveform_analyzer_update_extremes(
    uint16_t sample,
    uint16_t *lowest,
    uint16_t *highest)
{
    if (sample < lowest[WAVEFORM_ANALYZER_EXTREME_AVERAGE_COUNT - 1U]) {
        uint8_t index = WAVEFORM_ANALYZER_EXTREME_AVERAGE_COUNT - 1U;
        while ((index > 0U) && (sample < lowest[index - 1U])) {
            lowest[index] = lowest[index - 1U];
            --index;
        }
        lowest[index] = sample;
    }

    if (sample > highest[WAVEFORM_ANALYZER_EXTREME_AVERAGE_COUNT - 1U]) {
        uint8_t index = WAVEFORM_ANALYZER_EXTREME_AVERAGE_COUNT - 1U;
        while ((index > 0U) && (sample > highest[index - 1U])) {
            highest[index] = highest[index - 1U];
            --index;
        }
        highest[index] = sample;
    }
}

HAL_StatusTypeDef Waveform_Analyzer_Init(uint32_t sample_rate_hz)
{
    arm_status status;

    /* 参数调整后仍必须保证 Fs / N 不超过题目要求的 500 Hz。 */
    if ((sample_rate_hz == 0U) ||
        ((uint64_t)sample_rate_hz >
         ((uint64_t)WAVEFORM_ANALYZER_FFT_SIZE *
          WAVEFORM_ANALYZER_MAX_BIN_RESOLUTION_HZ))) {
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
    uint16_t lowest[WAVEFORM_ANALYZER_EXTREME_AVERAGE_COUNT];
    uint16_t highest[WAVEFORM_ANALYZER_EXTREME_AVERAGE_COUNT];
    uint32_t sum = 0U;
    uint32_t lowest_sum = 0U;
    uint32_t highest_sum = 0U;
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

    for (uint8_t index = 0U;
         index < WAVEFORM_ANALYZER_EXTREME_AVERAGE_COUNT;
         ++index) {
        lowest[index] = WAVEFORM_ANALYZER_ADC_MAX_CODE;
        highest[index] = 0U;
    }

    for (uint32_t index = 0U; index < WAVEFORM_ANALYZER_FFT_SIZE; ++index) {
        const uint16_t sample = samples[index] & WAVEFORM_ANALYZER_ADC_MAX_CODE;
        if (sample < min_code) {
            min_code = sample;
        }
        if (sample > max_code) {
            max_code = sample;
        }
        waveform_analyzer_update_extremes(sample, lowest, highest);
        sum += sample;
    }

    for (uint8_t index = 0U;
         index < WAVEFORM_ANALYZER_EXTREME_AVERAGE_COUNT;
         ++index) {
        lowest_sum += lowest[index];
        highest_sum += highest[index];
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

    g_waveform_analyzer_result.sample_rate_hz =
        g_waveform_analyzer.sample_rate_hz;
    g_waveform_analyzer_result.bin_resolution_hz =
        (float)g_waveform_analyzer.sample_rate_hz /
        (float)WAVEFORM_ANALYZER_FFT_SIZE;
    waveform_analyzer_find_spectrum_peaks();

    if (g_waveform_analyzer_result.peak_count > 0U) {
        fundamental_bin = g_waveform_analyzer_result.peak_bins[0];
        fundamental_magnitude =
            g_waveform_analyzer_result.peak_amplitudes_code[0];
    } else {
        fundamental_bin = 0U;
        fundamental_magnitude = 0.0f;
    }
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
        (uint16_t)(((highest_sum - lowest_sum) +
                    (WAVEFORM_ANALYZER_EXTREME_AVERAGE_COUNT / 2U)) /
                   WAVEFORM_ANALYZER_EXTREME_AVERAGE_COUNT);
    g_waveform_analyzer_result.dc_code = dc_code;
    g_waveform_analyzer_result.rms_code =
        sqrtf(square_sum / (float)WAVEFORM_ANALYZER_FFT_SIZE);
    g_waveform_analyzer_result.fundamental_bin = fundamental_bin;
    g_waveform_analyzer_result.fundamental_frequency_hz =
        (g_waveform_analyzer_result.peak_count > 0U) ?
            g_waveform_analyzer_result.peak_frequencies_hz[0] :
            0.0f;
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
