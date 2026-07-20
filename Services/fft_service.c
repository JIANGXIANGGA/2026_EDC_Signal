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

static arm_rfft_fast_instance_f32 g_rfft; /* CMSIS-DSP 实数快速 FFT 运算实例。 */
static FftService_Result g_result;         /* 最近一帧有效的 FFT 测量结果。 */

/* FFT 工作区不参与 DMA，放入 CCM SRAM 为主 SRAM 留出显示与 DMA 余量。 */
static float g_window[FFT_SERVICE_SAMPLE_COUNT] FFT_SERVICE_CCMRAM; /* Hann 窗系数表。 */
static float g_fft_input[FFT_SERVICE_SAMPLE_COUNT] FFT_SERVICE_CCMRAM; /* 去直流并加窗后的 FFT 输入。 */
static float g_fft_output[FFT_SERVICE_SAMPLE_COUNT] FFT_SERVICE_CCMRAM; /* CMSIS-DSP 打包格式的实数 FFT 输出。 */
static float g_magnitudes[FFT_SERVICE_BIN_COUNT] FFT_SERVICE_CCMRAM; /* 单边幅度谱缓存。 */

static float g_window_sum;       /* Hann 窗系数总和，用于幅度校正。 */
static uint8_t g_initialized;    /* FFT 服务是否已完成初始化。 */
static uint8_t g_result_valid;   /* 当前结果和频谱缓存是否有效。 */

/** @brief 生成非零且可回卷的结果代次编号。 */
static uint32_t fft_service_next_generation(uint32_t generation)
{
    generation++;
    return (generation != 0U) ? generation : 1U;
}

/** @brief 清零 FFT 输入、输出和幅度谱工作区。 */
static void fft_service_clear_workspace(void)
{
    uint32_t index; /* 清零 FFT 工作区时的数组索引。 */

    for (index = 0U; index < FFT_SERVICE_SAMPLE_COUNT; index++) {
        g_fft_input[index] = 0.0f;
        g_fft_output[index] = 0.0f;
        if (index < FFT_SERVICE_BIN_COUNT) {
            g_magnitudes[index] = 0.0f;
        }
    }
}

/** @brief 生成 Hann 窗系数并计算幅度校正所需的窗和。 */
static void fft_service_build_hann_window(void)
{
    uint32_t index; /* 生成 Hann 窗系数时的数组索引。 */

    g_window_sum = 0.0f;
    for (index = 0U; index < FFT_SERVICE_SAMPLE_COUNT; index++) {
        float phase = (2.0f * FFT_SERVICE_PI * (float)index) /
                      (float)(FFT_SERVICE_SAMPLE_COUNT - 1U); /* 当前窗系数对应的弧度相位。 */

        g_window[index] = 0.5f - (0.5f * cosf(phase));
        g_window_sum += g_window[index];
    }
}

/** @brief 搜索频谱主峰并使用对数抛物线完成频点插值。 */
static void fft_service_find_dominant_bin(uint16_t *dominant_bin,
                                          float *interpolated_bin,
                                          float *interpolated_amplitude)
{
    uint32_t bin;                 /* 扫描单边幅度谱的频点索引。 */
    uint16_t peak_bin = 0U;       /* 幅度最大的整数频点编号。 */
    float peak_magnitude = 0.0f;  /* 当前找到的最大频点幅度。 */
    float peak_position = 0.0f;   /* 抛物线插值后的主峰频点位置。 */

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
        float left = g_magnitudes[peak_bin - 1U];   /* 主峰左邻频点的幅度。 */
        float center = g_magnitudes[peak_bin];      /* 主峰中心频点的幅度。 */
        float right = g_magnitudes[peak_bin + 1U]; /* 主峰右邻频点的幅度。 */
        
        if ((left > FFT_SERVICE_INTERPOLATION_EPSILON) &&
            (center > FFT_SERVICE_INTERPOLATION_EPSILON) &&
            (right > FFT_SERVICE_INTERPOLATION_EPSILON)) {
            float log_left = logf(left);     /* 左邻频点幅度的自然对数。 */
            float log_center = logf(center); /* 中心频点幅度的自然对数。 */
            float log_right = logf(right);   /* 右邻频点幅度的自然对数。 */
            float denominator =
                log_left - (2.0f * log_center) + log_right; /* 对数抛物线插值的分母。 */
            float offset = 0.0f; /* 主峰相对整数频点的插值偏移量。 */

            if (fabsf(denominator) >
                FFT_SERVICE_INTERPOLATION_EPSILON) {
                float peak_log; /* 插值后主峰幅度的自然对数。 */

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

/** @brief 初始化 CMSIS-DSP 实数 FFT 实例及 Hann 窗工作区。 */
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

/** @brief 对一帧 ADC 数据去直流、加窗并计算单边幅度谱。 */
FftService_Status FftService_Process(const uint16_t *samples,
                                     uint32_t sample_count,
                                     uint32_t sample_rate_hz)
{
    uint32_t index;                     /* 遍历输入采样或频谱的数组索引。 */
    uint16_t dominant_bin;              /* 未插值的主峰频点编号。 */
    float interpolated_bin;             /* 插值后的主峰频点位置。 */
    float interpolated_amplitude;       /* 插值后的主峰幅度码值。 */
    float mean = 0.0f;                  /* 当前采样块的直流平均码值。 */
    float square_sum = 0.0f;            /* 去直流采样的平方和，用于计算 RMS。 */
    float magnitude_scale;              /* Hann 窗单边幅度谱的校正系数。 */

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
        float centered = (float)(samples[index] & 0x0FFFU) - mean; /* 当前采样去除直流后的有符号码值。 */

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

/** @brief 复制最近一帧有效的 FFT 测量结果。 */
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

/** @brief 返回最近一帧单边幅度谱的只读视图及频点数。 */
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
