#include "waveform_analyzer_service.h"

#include <math.h>
#include <stddef.h>

#define WAVEFORM_ANALYZER_ADC_MAX_CODE 4095U
#define WAVEFORM_ANALYZER_CLIP_LOW_CODE 2U
#define WAVEFORM_ANALYZER_CLIP_HIGH_CODE 4093U
#define WAVEFORM_ANALYZER_MIN_PEAK_CODE 1.0f

typedef struct {
    uint8_t initialized;
    uint32_t sample_rate_hz;
} waveform_analyzer_context_t;

static waveform_analyzer_context_t g_waveform_analyzer;
static waveform_analyzer_result_t g_waveform_analyzer_result;

static void waveform_analyzer_clear_peaks(void)
{
    g_waveform_analyzer_result.peak_count = 0U;
    for (uint8_t index = 0U; index < WAVEFORM_ANALYZER_PEAK_COUNT; ++index) {
        g_waveform_analyzer_result.peak_bins[index] = 0U;
        g_waveform_analyzer_result.peak_frequencies_hz[index] = 0.0f;
        g_waveform_analyzer_result.peak_amplitudes_code[index] = 0.0f;
    }
}

static void waveform_analyzer_clear_result(uint32_t sample_rate_hz)
{
    g_waveform_analyzer_result = (waveform_analyzer_result_t){0};
    g_waveform_analyzer_result.sample_rate_hz = sample_rate_hz;
    g_waveform_analyzer_result.bin_resolution_hz =
        (sample_rate_hz == 0U) ? 0.0f :
            ((float)sample_rate_hz / (float)WAVEFORM_ANALYZER_FFT_SIZE);
}

static uint8_t waveform_analyzer_is_local_peak(uint16_t bin)
{
    return ((FFT_Magnitude[bin] >= FFT_Magnitude[bin - 1U]) &&
            (FFT_Magnitude[bin] > FFT_Magnitude[bin + 1U])) ?
               1U :
               0U;
}

static void waveform_analyzer_insert_peak(uint16_t bin,
                                           float frequency_hz,
                                           float amplitude_code)
{
    uint8_t position;

    if (g_waveform_analyzer_result.peak_count <
        WAVEFORM_ANALYZER_PEAK_COUNT) {
        position = g_waveform_analyzer_result.peak_count;
        g_waveform_analyzer_result.peak_count++;
    } else {
        if (amplitude_code <=
            g_waveform_analyzer_result
                .peak_amplitudes_code[WAVEFORM_ANALYZER_PEAK_COUNT - 1U]) {
            return;
        }
        position = WAVEFORM_ANALYZER_PEAK_COUNT - 1U;
    }

    while ((position > 0U) &&
           (amplitude_code >
            g_waveform_analyzer_result.peak_amplitudes_code[position - 1U])) {
        g_waveform_analyzer_result.peak_bins[position] =
            g_waveform_analyzer_result.peak_bins[position - 1U];
        g_waveform_analyzer_result.peak_frequencies_hz[position] =
            g_waveform_analyzer_result.peak_frequencies_hz[position - 1U];
        g_waveform_analyzer_result.peak_amplitudes_code[position] =
            g_waveform_analyzer_result.peak_amplitudes_code[position - 1U];
        --position;
    }

    g_waveform_analyzer_result.peak_bins[position] = bin;
    g_waveform_analyzer_result.peak_frequencies_hz[position] = frequency_hz;
    g_waveform_analyzer_result.peak_amplitudes_code[position] = amplitude_code;
}

static void waveform_analyzer_sort_peaks_by_frequency(void)
{
    for (uint8_t index = 1U;
         index < g_waveform_analyzer_result.peak_count;
         ++index) {
        uint8_t position = index;

        while ((position > 0U) &&
               (g_waveform_analyzer_result.peak_frequencies_hz[position] <
                g_waveform_analyzer_result
                    .peak_frequencies_hz[position - 1U])) {
            const uint16_t bin =
                g_waveform_analyzer_result.peak_bins[position - 1U];
            const float frequency =
                g_waveform_analyzer_result
                    .peak_frequencies_hz[position - 1U];
            const float amplitude =
                g_waveform_analyzer_result
                    .peak_amplitudes_code[position - 1U];

            g_waveform_analyzer_result.peak_bins[position - 1U] =
                g_waveform_analyzer_result.peak_bins[position];
            g_waveform_analyzer_result.peak_frequencies_hz[position - 1U] =
                g_waveform_analyzer_result.peak_frequencies_hz[position];
            g_waveform_analyzer_result.peak_amplitudes_code[position - 1U] =
                g_waveform_analyzer_result.peak_amplitudes_code[position];
            g_waveform_analyzer_result.peak_bins[position] = bin;
            g_waveform_analyzer_result.peak_frequencies_hz[position] =
                frequency;
            g_waveform_analyzer_result.peak_amplitudes_code[position] =
                amplitude;
            --position;
        }
    }
}

static void waveform_analyzer_find_spectrum_peaks(void)
{
    const float bin_resolution =
        g_waveform_analyzer_result.bin_resolution_hz;
    uint16_t first_bin;
    uint16_t last_bin;

    waveform_analyzer_clear_peaks();
    if (bin_resolution <= 0.0f) {
        return;
    }

    first_bin = (uint16_t)ceilf(FFT_MIN_FREQUENCY_HZ / bin_resolution);
    last_bin = (uint16_t)floorf(FFT_MAX_FREQUENCY_HZ / bin_resolution);
    if (first_bin < 1U) {
        first_bin = 1U;
    }
    if (last_bin >= (WAVEFORM_ANALYZER_BIN_COUNT - 1U)) {
        last_bin = WAVEFORM_ANALYZER_BIN_COUNT - 2U;
    }

    for (uint16_t bin = first_bin; bin <= last_bin; ++bin) {
        float frequency_hz;
        float amplitude_code;

        if ((FFT_Magnitude[bin] < WAVEFORM_ANALYZER_MIN_PEAK_CODE) ||
            (waveform_analyzer_is_local_peak(bin) == 0U) ||
            (FFT_GetInterpolatedPeak(bin,
                                     (float)g_waveform_analyzer.sample_rate_hz,
                                     &frequency_hz,
                                     &amplitude_code) == 0U)) {
            continue;
        }
        if ((frequency_hz < FFT_MIN_FREQUENCY_HZ) ||
            (frequency_hz > FFT_MAX_FREQUENCY_HZ)) {
            continue;
        }

        waveform_analyzer_insert_peak(bin, frequency_hz, amplitude_code);
    }

    waveform_analyzer_sort_peaks_by_frequency();
}

HAL_StatusTypeDef Waveform_Analyzer_Init(uint32_t sample_rate_hz)
{
    if ((sample_rate_hz == 0U) ||
        ((uint64_t)sample_rate_hz >
         ((uint64_t)WAVEFORM_ANALYZER_FFT_SIZE *
          WAVEFORM_ANALYZER_MAX_BIN_RESOLUTION_HZ))) {
        return HAL_ERROR;
    }

    g_waveform_analyzer.sample_rate_hz = sample_rate_hz;
    g_waveform_analyzer.initialized = 1U;
    waveform_analyzer_clear_result(sample_rate_hz);
    g_waveform_analyzer_result.initialized = 1U;
    return HAL_OK;
}

HAL_StatusTypeDef Waveform_Analyzer_ProcessBlock(const uint16_t *samples,
                                                  uint32_t length)
{
    uint16_t min_code = WAVEFORM_ANALYZER_ADC_MAX_CODE;
    uint16_t max_code = 0U;
    uint32_t sum = 0U;
    float square_sum = 0.0f;
    float dc_code;

    if ((g_waveform_analyzer.initialized == 0U) || (samples == NULL) ||
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
        const float centered_sample =
            (float)(samples[index] & WAVEFORM_ANALYZER_ADC_MAX_CODE) - dc_code;
        square_sum += centered_sample * centered_sample;
    }

    FFT_Process(samples, (float)g_waveform_analyzer.sample_rate_hz);
    waveform_analyzer_find_spectrum_peaks();

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
        (uint16_t)(dc_code + 0.5f);
    g_waveform_analyzer_result.peak_to_peak_code = max_code - min_code;
    g_waveform_analyzer_result.dc_code = dc_code;
    g_waveform_analyzer_result.rms_code =
        sqrtf(square_sum / (float)WAVEFORM_ANALYZER_FFT_SIZE);
    g_waveform_analyzer_result.fundamental_bin = (uint16_t)FFT_PeakIndex;
    g_waveform_analyzer_result.fundamental_frequency_hz = FFT_PeakFrequency;
    g_waveform_analyzer_result.fundamental_amplitude_code = FFT_PeakAmplitude;
    return HAL_OK;
}

const waveform_analyzer_result_t *Waveform_Analyzer_GetResult(void)
{
    return &g_waveform_analyzer_result;
}

const float *Waveform_Analyzer_GetMagnitudeBins(void)
{
    return FFT_Magnitude;
}

float Waveform_Analyzer_GetMagnitudeAtBin(uint16_t bin_index)
{
    return (bin_index < WAVEFORM_ANALYZER_BIN_COUNT) ?
               FFT_Magnitude[bin_index] :
               0.0f;
}
