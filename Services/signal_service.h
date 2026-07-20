#ifndef SIGNAL_SERVICE_H
#define SIGNAL_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "dds.h"
#include "stm32g4xx_hal.h"

#define SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT 120U

#define SIGNAL_SERVICE_SAMPLE_RATE_20K_HZ  20000U
#define SIGNAL_SERVICE_SAMPLE_RATE_50K_HZ  50000U
#define SIGNAL_SERVICE_SAMPLE_RATE_100K_HZ 100000U
#define SIGNAL_SERVICE_SAMPLE_RATE_200K_HZ 200000U

#define SIGNAL_SERVICE_DDS_FREQ_MIN_HZ 10U

typedef enum {
    SIGNAL_SERVICE_PROCESS_NONE = 0U,
    SIGNAL_SERVICE_PROCESS_ADC_UPDATED = (1U << 0),
    SIGNAL_SERVICE_PROCESS_DDS_UPDATED = (1U << 1),
    SIGNAL_SERVICE_PROCESS_FFT_UPDATED = (1U << 2)
} SignalService_ProcessEvent;

typedef struct {
    uint32_t sample_rate_hz;                 /**< ADC 当前实际采样率，单位 Hz。 */
    uint32_t dds_frequency_hz;               /**< DDS 当前输出频率，单位 Hz。 */
    uint32_t adc_snapshot_generation;        /**< ADC 时域快照的更新代次。 */
    uint32_t dds_snapshot_generation;        /**< DDS 时域快照的更新代次。 */
    uint32_t spectrum_snapshot_generation;   /**< FFT 频谱快照的更新代次。 */
    uint32_t fft_peak_frequency_millihz;     /**< FFT 主峰频率，单位 mHz。 */
    uint32_t fft_resolution_millihz;         /**< FFT 频率分辨率，单位 mHz。 */
    uint32_t adc_dropped_block_count;        /**< ADC 未及时消费而丢弃的块数。 */
    uint32_t adc_error_count;                /**< ADC SPI/DMA 错误累计次数。 */
    uint32_t dac_underrun_count;             /**< DAC 输出半区欠载累计次数。 */
    dds_waveform_t dds_waveform;             /**< DDS 当前波形类型。 */
    uint16_t fft_peak_amplitude_codes;       /**< FFT 主峰幅度，单位 ADC 码值。 */
    uint16_t adc_rms_codes;                  /**< ADC 去直流信号的有效值码数。 */
    uint8_t dds_amplitude_percent;           /**< DDS 当前幅度百分比。 */
    uint8_t adc_snapshot_valid;              /**< ADC 时域快照是否有效。 */
    uint8_t dds_snapshot_valid;              /**< DDS 时域快照是否有效。 */
    uint8_t spectrum_snapshot_valid;         /**< FFT 频谱快照是否有效。 */
} SignalService_State;

/**
 * @brief 初始化信号服务、DDS 和 1024 点 FFT。
 * @note ADC121S101、DAC 输出及 TIM6/TIM7 硬件须先完成 Driver/CubeMX 初始化。
 */
HAL_StatusTypeDef SignalService_Init(TIM_HandleTypeDef *adc_sample_timer,
                                     TIM_HandleTypeDef *dds_sample_timer);

/**
 * @brief 在主循环中处理驱动恢复、ADC/FFT 数据块和 DDS 输出块。
 * @return SignalService_ProcessEvent 位掩码。
 * @note 禁止在中断中调用。
 */
uint32_t SignalService_Process(void);

/**
 * @brief 重启 ADC 采样流并设置 TIM7 采样率。
 * @param requested_hz 仅支持 20k、50k、100k、200kHz。
 * @param actual_hz 可选；返回由 PSC/ARR 得到的实际频率。
 */
HAL_StatusTypeDef SignalService_SetSampleRate(uint32_t requested_hz,
                                             uint32_t *actual_hz);

/** @brief 设置 DDS 频率，自动限制在 10Hz 至 DDS 更新率的十分之一。 */
HAL_StatusTypeDef SignalService_SetDdsFrequency(uint32_t requested_hz,
                                               uint32_t *applied_hz);

/** @brief 设置 DDS 幅度百分比，自动限制在 10% 至 100%。 */
HAL_StatusTypeDef SignalService_SetDdsAmplitude(uint8_t requested_percent,
                                               uint8_t *applied_percent);

/** @brief 设置 DDS 波形。 */
HAL_StatusTypeDef SignalService_SetDdsWaveform(dds_waveform_t waveform);

/** @brief 读取服务当前参数、FFT 测量值和驱动统计。 */
HAL_StatusTypeDef SignalService_GetState(SignalService_State *state);

/** @brief 将最新 ADC 时域显示快照复制到调用方缓冲区。 */
uint8_t SignalService_CopyAdcSnapshot(int32_t *points,
                                     uint16_t capacity,
                                     uint32_t *generation);

/** @brief 将最新 DDS 时域显示快照复制到调用方缓冲区。 */
uint8_t SignalService_CopyDdsSnapshot(int32_t *points,
                                     uint16_t capacity,
                                     uint32_t *generation);

/** @brief 将最新 FFT 频谱显示快照复制到调用方缓冲区。 */
uint8_t SignalService_CopySpectrumSnapshot(int32_t *points,
                                          uint16_t capacity,
                                          uint32_t *generation);

#ifdef __cplusplus
}
#endif

#endif /* SIGNAL_SERVICE_H */
