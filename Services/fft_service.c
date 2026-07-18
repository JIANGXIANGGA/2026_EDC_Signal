#include "fft_service.h"

#include <math.h>
#include <stddef.h>

#include "arm_math.h"

#define FFT_SERVICE_PI                    3.14159265358979323846f
#define FFT_SERVICE_MIN_AMPLITUDE_CODES   0.5f
#define FFT_SERVICE_INTERPOLATION_EPSILON 1.0e-12f

#if defined(__GNUC__)
#define FFT_SERVICE_CCMRAM \
    __attribute__((section(".ccmram"), aligned(4)))
#else
#define FFT_SERVICE_CCMRAM
#endif

static arm_rfft_fast_instance_f32 g_rfft;
static FftService_Result g_result;

/* FFT 工作区不参与 DMA，放入 CCM SRAM 为主 SRAM 留出显示与 DMA 余量。 */
static float g_window[FFT_SERVICE_SAMPLE_COUNT] FFT_SERVICE_CCMRAM;
static float g_fft_input[FFT_SERVICE_SAMPLE_COUNT] FFT_SERVICE_CCMRAM;
static float g_fft_output[FFT_SERVICE_SAMPLE_COUNT] FFT_SERVICE_CCMRAM;
static float g_magnitudes[FFT_SERVICE_BIN_COUNT] FFT_SERVICE_CCMRAM;

static float g_window_sum;
static uint8_t g_initialized;
static uint8_t g_result_valid;

static uint32_t fft_service_next_generation(uint32_t generation)
{
    generation++;
    return (generation != 0U) ? generation : 1U;
}

static void fft_service_clear_workspace(void)
{
    uint32_t index;

    for (index = 0U; index < FFT_SERVICE_SAMPLE_COUNT; index++) {
        g_fft_input[index] = 0.0f;
        g_fft_output[index] = 0.0f;
        if (index < FFT_SERVICE_BIN_COUNT) {
            g_magnitudes[index] = 0.0f;
        }
    }
}

static void fft_service_build_hann_window(void)
{
    uint32_t index;

    g_window_sum = 0.0f;
    for (index = 0U; index < FFT_SERVICE_SAMPLE_COUNT; index++) {
        float phase = (2.0f * FFT_SERVICE_PI * (float)index) /
                      (float)(FFT_SERVICE_SAMPLE_COUNT - 1U);

        g_window[index] = 0.5f - (0.5f * cosf(phase));
        g_window_sum += g_window[index];
    }
}

static void fft_service_find_dominant_bin(uint16_t *dominant_bin,
                                          float *interpolated_bin,
                                          float *interpolated_amplitude)
{
    uint32_t bin;
    uint16_t peak_bin = 0U;
    float peak_magnitude = 0.0f;
    float peak_position = 0.0f;

    for (bin = 1U; bin < FFT_SERVICE_BIN_COUNT; bin++) {
        if (g_magnitudes[bin] > peak_magnitude) {
            peak_magnitude = g_magnitudes[bin];
            peak_bin = (uint16_t)bin;
        }
    }

    if (peak_magnitude < FFT_SERVICE_MIN_AMPLITUDE_CODES) {
        *dominant_bin = 0U;
        *interpolated_bin = 0.0f;
        *interpolated_amplitude = 0.0f;
        return;
    }

    peak_position = (float)peak_bin;
    *interpolated_amplitude = peak_magnitude;
    if ((peak_bin > 1U) &&
        (peak_bin < (FFT_SERVICE_BIN_COUNT - 1U))) {
        float left = g_magnitudes[peak_bin - 1U];
        float center = g_magnitudes[peak_bin];
        float right = g_magnitudes[peak_bin + 1U];
        
        if ((left > FFT_SERVICE_INTERPOLATION_EPSILON) &&
            (center > FFT_SERVICE_INTERPOLATION_EPSILON) &&
            (right > FFT_SERVICE_INTERPOLATION_EPSILON)) {
            float log_left = logf(left);
            float log_center = logf(center);
            float log_right = logf(right);
            float denominator =
                log_left - (2.0f * log_center) + log_right;
            float offset = 0.0f;

            if (fabsf(denominator) >
                FFT_SERVICE_INTERPOLATION_EPSILON) {
                float peak_log;

                offset = 0.5f * (log_left - log_right) /
                         denominator;
                if (offset < -0.5f) {
                    offset = -0.5f;
                }
                if (offset > 0.5f) {
                    offset = 0.5f;
                }
                peak_log = log_center -
                    (0.25f * (log_left - log_right) * offset);
                *interpolated_amplitude = expf(peak_log);
                peak_position += offset;
            }
        }
    }

    *dominant_bin = peak_bin;
    *interpolated_bin = peak_position;
}

FftService_Status FftService_Init(void)
{
    if (arm_rfft_fast_init_f32(&g_rfft,
                               FFT_SERVICE_SAMPLE_COUNT) != ARM_MATH_SUCCESS) {
        g_initialized = 0U;
        return FFT_SERVICE_STATUS_DSP_ERROR;
    }

    fft_service_clear_workspace();
    fft_service_build_hann_window();
    g_result = (FftService_Result){0};
    g_result_valid = 0U;
    g_initialized = 1U;
    return FFT_SERVICE_STATUS_OK;
}

FftService_Status FftService_Process(const uint16_t *samples,
                                     uint32_t sample_count,
                                     uint32_t sample_rate_hz)
{
    uint32_t index;
    uint16_t dominant_bin;
    float interpolated_bin;
    float interpolated_amplitude;
    float mean = 0.0f;
    float square_sum = 0.0f;
    float magnitude_scale;

    if (g_initialized == 0U) {
        return FFT_SERVICE_STATUS_NOT_INITIALIZED;
    }
    if ((samples == NULL) ||
        (sample_count != FFT_SERVICE_SAMPLE_COUNT) ||
        (sample_rate_hz == 0U) ||
        (g_window_sum <= 0.0f)) {
        return FFT_SERVICE_STATUS_INVALID_ARGUMENT;
    }

    for (index = 0U; index < FFT_SERVICE_SAMPLE_COUNT; index++) {
        mean += (float)(samples[index] & 0x0FFFU);
    }
    mean /= (float)FFT_SERVICE_SAMPLE_COUNT;

    for (index = 0U; index < FFT_SERVICE_SAMPLE_COUNT; index++) {
        float centered = (float)(samples[index] & 0x0FFFU) - mean;

        square_sum += centered * centered;
        g_fft_input[index] = centered * g_window[index];
    }

    arm_rfft_fast_f32(&g_rfft, g_fft_input, g_fft_output, 0U);

    g_magnitudes[0] = fabsf(g_fft_output[0]) / g_window_sum;
    arm_cmplx_mag_f32(&g_fft_output[2],
                      &g_magnitudes[1],
                      FFT_SERVICE_BIN_COUNT - 1U);

    magnitude_scale = 2.0f / g_window_sum;
    for (index = 1U; index < FFT_SERVICE_BIN_COUNT; index++) {
        g_magnitudes[index] *= magnitude_scale;
    }

    fft_service_find_dominant_bin(&dominant_bin,
                                  &interpolated_bin,
                                  &interpolated_amplitude);
    g_result.sample_rate_hz = sample_rate_hz;
    g_result.frequency_resolution_hz =
        (float)sample_rate_hz / (float)FFT_SERVICE_SAMPLE_COUNT;
    g_result.dominant_bin = dominant_bin;
    g_result.dominant_frequency_hz =
        interpolated_bin * g_result.frequency_resolution_hz;
    g_result.dominant_amplitude_codes = interpolated_amplitude;
    g_result.rms_codes =
        sqrtf(square_sum / (float)FFT_SERVICE_SAMPLE_COUNT);
    g_result.dc_offset_codes = mean;
    g_result.generation =
        fft_service_next_generation(g_result.generation);
    g_result_valid = 1U;
    return FFT_SERVICE_STATUS_OK;
}

FftService_Status FftService_GetResult(FftService_Result *result)
{
    if (g_initialized == 0U) {
        return FFT_SERVICE_STATUS_NOT_INITIALIZED;
    }
    if ((result == NULL) || (g_result_valid == 0U)) {
        return FFT_SERVICE_STATUS_INVALID_ARGUMENT;
    }

    *result = g_result;
    return FFT_SERVICE_STATUS_OK;
}

FftService_Status FftService_GetSpectrum(const float **magnitudes,
                                         uint16_t *bin_count)
{
    if (g_initialized == 0U) {
        return FFT_SERVICE_STATUS_NOT_INITIALIZED;
    }
    if ((magnitudes == NULL) || (bin_count == NULL) ||
        (g_result_valid == 0U)) {
        return FFT_SERVICE_STATUS_INVALID_ARGUMENT;
    }

    *magnitudes = g_magnitudes;
    *bin_count = FFT_SERVICE_BIN_COUNT;
    return FFT_SERVICE_STATUS_OK;
}
