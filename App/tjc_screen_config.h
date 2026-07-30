#ifndef TJC_SCREEN_CONFIG_H
#define TJC_SCREEN_CONFIG_H

#include "tjc_screen_component_ids.h"

/*
 * TJC 串口屏与 STM32 固件的页面契约。
 *
 * TJC 编辑器中页面 ID、组件 ID 或变量名发生变化时，只修改本文件。
 * 所有数值变量均应在 TJC 工程中创建为全局数值变量。
 */

/* 串口参数：115200 bit/s、8 数据位、无校验、1 停止位、无流控。 */
#define TJC_SCREEN_UART_BAUD_RATE 115200U

#define TJC_SCREEN_ENABLE_PLOT_TRANSFER 1U
#define TJC_SCREEN_PLOT_SNAPSHOT_MODE 0U

#if ((TJC_SCREEN_ENABLE_PLOT_TRANSFER != 0U) && \
     (TJC_SCREEN_ENABLE_PLOT_TRANSFER != 1U))
#error "TJC_SCREEN_ENABLE_PLOT_TRANSFER must be 0 or 1"
#endif

#if ((TJC_SCREEN_PLOT_SNAPSHOT_MODE != 0U) && \
     (TJC_SCREEN_PLOT_SNAPSHOT_MODE != 1U))
#error "TJC_SCREEN_PLOT_SNAPSHOT_MODE must be 0 or 1"
#endif

/* 页面 0：测量结果与波形。 */
#define TJC_SCREEN_MEASUREMENT_PAGE_ID TJC_PAGE0_ID
#define TJC_SCREEN_UNASSIGNED_COMPONENT_ID 0xFFU

/* 组件 ID 以 222.HMI 最新导出的 22.h 为准。 */
#define TJC_SCREEN_MEASUREMENT_TIMER_COMPONENT_ID TJC_PAGE0_TM0_ID
#define TJC_SCREEN_TIME_PLOT_COMPONENT_ID TJC_PAGE0_W0_ID
#define TJC_SCREEN_SPECTRUM_PLOT_COMPONENT_ID TJC_PAGE0_W1_ID
/* b0、b1 分别选择显示 1 个和 3 个完整周期。 */
#define TJC_SCREEN_ONE_CYCLE_BUTTON_ID TJC_PAGE0_B0_ID
#define TJC_SCREEN_THREE_CYCLE_BUTTON_ID TJC_PAGE0_B1_ID
/* 自动模式下 bSnap 可立即触发新一轮时域波形和频谱图更新。 */
#define TJC_SCREEN_SNAPSHOT_BUTTON_COMPONENT_ID TJC_PAGE0_BSNAP_ID
#define TJC_SCREEN_PLOT_CHANNEL 0U

/* w0 可视宽度为 512 像素；每个横向像素传一个采样点。 */
#define TJC_SCREEN_TIME_PLOT_WIDTH_PIXELS 512U
#define TJC_SCREEN_PLOT_POINT_COUNT 512U

/*
 * w0 实机显示方向修正：
 * 1U：反转发送点序，使波形时间轴按从左到右显示；
 * 0U：保持采样缓冲区的原始点序。
 */
#define TJC_SCREEN_REVERSE_TIME_PLOT_POINTS 1U
#define TJC_SCREEN_REVERSE_SPECTRUM_PLOT_POINTS 1U

#if ((TJC_SCREEN_REVERSE_TIME_PLOT_POINTS != 0U) && \
     (TJC_SCREEN_REVERSE_TIME_PLOT_POINTS != 1U))
#error "TJC_SCREEN_REVERSE_TIME_PLOT_POINTS must be 0 or 1"
#endif

#if ((TJC_SCREEN_REVERSE_SPECTRUM_PLOT_POINTS != 0U) && \
     (TJC_SCREEN_REVERSE_SPECTRUM_PLOT_POINTS != 1U))
#error "TJC_SCREEN_REVERSE_SPECTRUM_PLOT_POINTS must be 0 or 1"
#endif

#if (TJC_SCREEN_PLOT_POINT_COUNT != TJC_SCREEN_TIME_PLOT_WIDTH_PIXELS)
#error "TJC plot point count must match the w0 visible width"
#endif

#if TJC_SCREEN_ENABLE_PLOT_TRANSFER && \
    (TJC_SCREEN_TIME_PLOT_COMPONENT_ID == \
     TJC_SCREEN_UNASSIGNED_COMPONENT_ID)
#error "Set the TJC time waveform component ID before enabling plot transfer"
#endif

#if TJC_SCREEN_ENABLE_PLOT_TRANSFER && \
    TJC_SCREEN_PLOT_SNAPSHOT_MODE && \
    (TJC_SCREEN_SNAPSHOT_BUTTON_COMPONENT_ID == \
     TJC_SCREEN_UNASSIGNED_COMPONENT_ID)
#error "Set the TJC snapshot button ID before enabling snapshot mode"
#endif

#if TJC_SCREEN_ENABLE_PLOT_TRANSFER && \
    (TJC_SCREEN_SPECTRUM_PLOT_COMPONENT_ID == \
     TJC_SCREEN_UNASSIGNED_COMPONENT_ID)
#error "Set the TJC spectrum component ID before enabling plot transfer"
#endif

#if TJC_SCREEN_ENABLE_PLOT_TRANSFER && \
    ((TJC_SCREEN_ONE_CYCLE_BUTTON_ID == \
      TJC_SCREEN_UNASSIGNED_COMPONENT_ID) || \
     (TJC_SCREEN_THREE_CYCLE_BUTTON_ID == \
      TJC_SCREEN_UNASSIGNED_COMPONENT_ID))
#error "Set both TJC waveform-cycle touch component IDs"
#endif

/* 页面 0 数值变量，电压和幅值的单位为 0.1 mV。 */
#define TJC_SCREEN_MEAS_SIGNAL_VALID "page0.va0"
#define TJC_SCREEN_MEAS_PEAK_TO_PEAK "page0.va1"
#define TJC_SCREEN_MEAS_TRUE_RMS "page0.va2"
#define TJC_SCREEN_MEAS_FUNDAMENTAL_HZ "page0.va3"
#define TJC_SCREEN_MEAS_COMPONENT_COUNT "page0.va4"
#define TJC_SCREEN_MEAS_COMPONENT1_HZ "page0.va5"
#define TJC_SCREEN_MEAS_COMPONENT1_AMPLITUDE "page0.va6"
/* 第二路在当前页面中的显示顺序为：t7 幅值、t8 频率。 */
#define TJC_SCREEN_MEAS_COMPONENT2_HZ "page0.va8"
#define TJC_SCREEN_MEAS_COMPONENT2_AMPLITUDE "page0.va7"
#define TJC_SCREEN_MEAS_COMPONENT3_HZ "page0.va9"
#define TJC_SCREEN_MEAS_COMPONENT3_AMPLITUDE "page0.va10"
#define TJC_SCREEN_MEAS_WAVEFORM_CYCLES "page0.va11"
/* 基波阶次固定为 1，串口屏只显示后两条谱线的谐波阶次。 */
#define TJC_SCREEN_MEAS_COMPONENT2_ORDER "page0.va12"
#define TJC_SCREEN_MEAS_COMPONENT3_ORDER "page0.va13"

#endif
