/**
 * @file dds_app.h
 * @brief 信号服务与 LVGL 界面的 Application 层接口
 */

#ifndef DDS_APP_H
#define DDS_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "stm32g4xx_hal.h"
#include "ui.h"

/** @brief 初始化 Application 层并绑定 ADC、DDS 采样定时器。 */
HAL_StatusTypeDef dds_app_init(TIM_HandleTypeDef *adc_sample_timer,
                               TIM_HandleTypeDef *dds_sample_timer);

/** @brief 请求将 ADC 采样率切换到较低一档。 */
void dds_app_request_sample_rate_down(void);
/** @brief 请求将 ADC 采样率切换到较高一档。 */
void dds_app_request_sample_rate_up(void);
/** @brief 请求降低 DDS 输出频率。 */
void dds_app_request_freq_down(void);
/** @brief 请求提高 DDS 输出频率。 */
void dds_app_request_freq_up(void);
/** @brief 请求降低 DDS 输出幅度。 */
void dds_app_request_amplitude_down(void);
/** @brief 请求提高 DDS 输出幅度。 */
void dds_app_request_amplitude_up(void);
/** @brief 请求切换到下一种 DDS 波形。 */
void dds_app_request_next_waveform(void);

/**
 * @brief 处理控制请求和两路信号块。
 * @retval true 到达 UI 刷新周期且状态有更新；false 暂不刷新 UI。
 */
bool dds_app_process(void);
/** @brief 将最新信号状态和绘图数据填入 UI 状态结构体。 */
void dds_app_fill_ui_state(ui_signal_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* DDS_APP_H */
