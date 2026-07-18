#ifndef FFT_SERVICE_H
#define FFT_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define FFT_SERVICE_SAMPLE_COUNT 1024U
#define FFT_SERVICE_BIN_COUNT    (FFT_SERVICE_SAMPLE_COUNT / 2U)

typedef enum {
    FFT_SERVICE_STATUS_OK = 0,
    FFT_SERVICE_STATUS_INVALID_ARGUMENT,
    FFT_SERVICE_STATUS_NOT_INITIALIZED,
    FFT_SERVICE_STATUS_DSP_ERROR
} FftService_Status;

/** @brief 一帧 ADC 实数 FFT 的测量结果，幅度单位为 ADC 码值。 */
typedef struct {
    float dominant_frequency_hz;
    float dominant_amplitude_codes;
    float rms_codes;
    float dc_offset_codes;
    float frequency_resolution_hz;
    uint32_t sample_rate_hz;
    uint32_t generation;
    uint16_t dominant_bin;
} FftService_Result;

/** @brief 初始化 1024 点实数 FFT 和 Hann 窗。 */
FftService_Status FftService_Init(void);

/**
 * @brief 在主循环中对一帧 12 位 ADC 数据执行 FFT。
 * @note 本函数会去直流、加 Hann 窗，并生成单边幅度谱。
 */
FftService_Status FftService_Process(const uint16_t *samples,
                                     uint32_t sample_count,
                                     uint32_t sample_rate_hz);

/** @brief 复制最近一帧 FFT 测量结果。 */
FftService_Status FftService_GetResult(FftService_Result *result);

/**
 * @brief 获取最近一帧单边幅度谱的只读视图。
 * @note 指针在下一次 FftService_Process() 前保持有效。
 */
FftService_Status FftService_GetSpectrum(const float **magnitudes,
                                         uint16_t *bin_count);

#ifdef __cplusplus
}
#endif

#endif /* FFT_SERVICE_H */
