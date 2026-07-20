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
    float dominant_frequency_hz;     /**< 插值后的主峰频率，单位 Hz。 */
    float dominant_amplitude_codes;  /**< 主峰单边幅度，单位 ADC 码值。 */
    float rms_codes;                 /**< 去直流后输入信号的有效值码数。 */
    float dc_offset_codes;           /**< 输入信号的平均直流偏置码数。 */
    float frequency_resolution_hz;   /**< 相邻 FFT 频点之间的频率间隔。 */
    uint32_t sample_rate_hz;         /**< 生成本次结果时使用的采样率。 */
    uint32_t generation;             /**< 结果更新代次，用于识别新帧。 */
    uint16_t dominant_bin;           /**< 未插值主峰所在的 FFT 频点编号。 */
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
