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
    uint32_t sample_rate_hz;
    uint32_t dds_freq_hz;
    uint32_t fft_peak_frequency_millihz;
    uint32_t fft_resolution_millihz;
    uint32_t adc_dropped_block_count;
    uint32_t adc_error_count;
    uint32_t dac_underrun_count;
    uint16_t fft_peak_amplitude_codes;
    uint16_t adc_rms_codes;
    uint8_t dds_amplitude_percent;
    const char * dds_waveform_text;
    int32_t * adc_points;
    int32_t * dds_points;
    int32_t * spectrum_points;
    uint16_t wave_point_count;
    uint16_t spectrum_point_count;
} ui_signal_state_t;

/** @brief 界面操作回调，由 Application 层提供具体实现。 */
typedef struct {
    void (*on_sample_rate_down)(void);
    void (*on_sample_rate_up)(void);
    void (*on_dds_freq_down)(void);
    void (*on_dds_freq_up)(void);
    void (*on_dds_amplitude_down)(void);
    void (*on_dds_amplitude_up)(void);
    void (*on_dds_waveform_next)(void);
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
