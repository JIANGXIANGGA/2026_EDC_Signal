/**
 * @file ui.h
 * @brief 信号控制界面接口
 */

#ifndef UI_H
#define UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "lvgl.h"

/** @brief 界面显示所需的信号状态快照。 */
typedef struct {
    uint32_t sample_rate_hz;                /**< 界面显示的 ADC 采样率，单位 Hz。 */
    uint32_t dds_freq_hz;                   /**< 界面显示的 DDS 频率，单位 Hz。 */
    uint32_t fft_peak_frequency_millihz;    /**< 界面显示的 FFT 主峰频率，单位 mHz。 */
    uint32_t fft_resolution_millihz;        /**< FFT 频率分辨率，单位 mHz。 */
    uint32_t adc_dropped_block_count;       /**< ADC 丢块累计数。 */
    uint32_t adc_error_count;               /**< ADC 传输错误累计数。 */
    uint32_t dac_underrun_count;            /**< DAC 欠载累计数。 */
    uint16_t fft_peak_amplitude_codes;      /**< FFT 主峰的 ADC 码值幅度。 */
    uint16_t adc_rms_codes;                 /**< ADC 输入有效值码数。 */
    uint8_t dds_amplitude_percent;          /**< DDS 幅度百分比。 */
    const char * dds_waveform_text;         /**< DDS 波形名称文本。 */
    int32_t * adc_points;                   /**< ADC 时域图的数据点数组。 */
    int32_t * dds_points;                   /**< DDS 时域图的数据点数组。 */
    int32_t * spectrum_points;              /**< FFT 频谱图的数据点数组。 */
    uint16_t wave_point_count;              /**< 每路时域图的有效点数。 */
    uint16_t spectrum_point_count;          /**< 频谱图的有效点数。 */
} ui_signal_state_t;

/** @brief 界面操作回调，由 Application 层提供具体实现。 */
typedef struct {
    void (*on_sample_rate_down)(void);    /**< 请求降低 ADC 采样率的回调。 */
    void (*on_sample_rate_up)(void);      /**< 请求提高 ADC 采样率的回调。 */
    void (*on_dds_freq_down)(void);       /**< 请求降低 DDS 频率的回调。 */
    void (*on_dds_freq_up)(void);         /**< 请求提高 DDS 频率的回调。 */
    void (*on_dds_amplitude_down)(void);  /**< 请求降低 DDS 幅度的回调。 */
    void (*on_dds_amplitude_up)(void);    /**< 请求提高 DDS 幅度的回调。 */
    void (*on_dds_waveform_next)(void);   /**< 请求切换到下一种 DDS 波形的回调。 */
} ui_signal_callbacks_t;

/**
 * @brief 创建信号显示与控制界面。
 * @param callbacks Application 层回调集合，允许传入 NULL。
 */
void ui_init(const ui_signal_callbacks_t * callbacks);

/**
 * @brief 使用最新状态刷新标签和两路波形。
 * @note adc_points 与 dds_points 必须在下一次刷新前保持有效。
 */
void ui_update_signal_state(const ui_signal_state_t * state);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
