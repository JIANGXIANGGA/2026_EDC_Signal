#include "signal_hmi_app.h"

#include <stddef.h>

#include "signal_measurement_service.h"
#include "signal_reconstruction_service.h"
#include "tjc_screen_config.h"
#include "tjc_uart_driver.h"
#include "usart_hmi_service.h"

#define ONE_HMI_STARTUP_DELAY_MS 800U
#define ONE_HMI_LINK_TIMEOUT_MS 2000U
#define ONE_HMI_RX_TRANSFER_BUFFER_SIZE 64U
#define ONE_HMI_WAVEFORM_REFRESH_MS 250U
#define ONE_HMI_WAVEFORM_PAGE_TIMEOUT_MS 200U
#define ONE_HMI_WAVEFORM_TRANSFER_TIMEOUT_MS 1000U
#define ONE_HMI_TIME_DISPLAY_MIN 16U
#define ONE_HMI_TIME_DISPLAY_MAX 239U
#define ONE_HMI_SPECTRUM_DISPLAY_MIN 8U
#define ONE_HMI_SPECTRUM_DISPLAY_MAX 239U
#define ONE_HMI_SPECTRUM_LEFT_MARGIN_POINTS 6U
#define ONE_HMI_SPECTRUM_RIGHT_MARGIN_POINTS 6U
#define ONE_HMI_SPECTRUM_LINE_HALF_WIDTH_POINTS 2U
#define ONE_HMI_SPECTRUM_RIGHT_HEADROOM_RATIO 1.10f
#define ONE_HMI_MEASUREMENT_FIELD_COUNT 14U
#define ONE_HMI_MEASUREMENT_REFRESH_MS 200U
#define ONE_HMI_PLOT_SWITCH_DELAY_MS 20U

_Static_assert(TJC_SCREEN_PLOT_POINT_COUNT <=
                   USART_HMI_TRANSPARENT_MAX_SIZE,
               "TJC waveform exceeds transparent transfer limit");
_Static_assert(TJC_SCREEN_PLOT_POINT_COUNT > 1U,
               "TJC waveform requires at least two points");
_Static_assert((ONE_HMI_SPECTRUM_LEFT_MARGIN_POINTS +
                ONE_HMI_SPECTRUM_RIGHT_MARGIN_POINTS + 1U) <
                   TJC_SCREEN_PLOT_POINT_COUNT,
               "TJC spectrum horizontal margins are too large");

typedef struct {
    uint32_t observed_rx_restart_count;
    uint32_t observed_rx_dropped_count;
    uint32_t waveform_deadline_ms;
    uint32_t waveform_next_refresh_ms;
    uint32_t waveform_pending_sequence;
    uint8_t waveform_snapshot_requested;
    uint32_t plot_last_sequence[2];
    uint32_t measurement_next_refresh_ms;
    uint32_t measurement_pending_sequence;
    uint8_t measurement_publish_active;
    uint8_t measurement_field_index;
    signal_measurement_result_t measurement_snapshot;
    float waveform_reconstructed_mv[TJC_SCREEN_PLOT_POINT_COUNT];
    uint8_t waveform_points[TJC_SCREEN_PLOT_POINT_COUNT];
    signal_hmi_status_t status;
} one_hmi_context_t;

static one_hmi_context_t g_one_hmi;

/*
 * 页面 0 隐藏数值变量协议：电压值采用 mV * 10 的定点格式。
 * 屏端虚拟浮点数组件设置 vvs=1 后，以 mV 为单位显示一位小数。
 */
static const char *const
    g_one_hmi_measurement_names[ONE_HMI_MEASUREMENT_FIELD_COUNT] = {
        TJC_SCREEN_MEAS_SIGNAL_VALID,
        TJC_SCREEN_MEAS_PEAK_TO_PEAK,
        TJC_SCREEN_MEAS_TRUE_RMS,
        TJC_SCREEN_MEAS_FUNDAMENTAL_HZ,
        TJC_SCREEN_MEAS_COMPONENT_COUNT,
        TJC_SCREEN_MEAS_COMPONENT1_HZ,
        TJC_SCREEN_MEAS_COMPONENT1_AMPLITUDE,
        TJC_SCREEN_MEAS_COMPONENT2_HZ,
        TJC_SCREEN_MEAS_COMPONENT2_AMPLITUDE,
        TJC_SCREEN_MEAS_COMPONENT3_HZ,
        TJC_SCREEN_MEAS_COMPONENT3_AMPLITUDE,
        TJC_SCREEN_MEAS_WAVEFORM_CYCLES,
        TJC_SCREEN_MEAS_COMPONENT2_ORDER,
        TJC_SCREEN_MEAS_COMPONENT3_ORDER,
};

static uint8_t one_hmi_time_reached(uint32_t deadline_ms)
{
    return ((int32_t)(HAL_GetTick() - deadline_ms) >= 0) ? 1U : 0U;
}

static void one_hmi_waveform_set_state(one_hmi_waveform_state_t state)
{
    g_one_hmi.status.waveform_state = state;
}

static void one_hmi_waveform_schedule_next(void)
{
    g_one_hmi.waveform_next_refresh_ms =
        HAL_GetTick() + ONE_HMI_WAVEFORM_REFRESH_MS;
}

static void one_hmi_measurement_schedule_next(void)
{
    g_one_hmi.measurement_next_refresh_ms =
        HAL_GetTick() + ONE_HMI_MEASUREMENT_REFRESH_MS;
}

static void one_hmi_waveform_reset(void)
{
    one_hmi_waveform_set_state(ONE_HMI_WAVEFORM_STATE_IDLE);
    g_one_hmi.waveform_deadline_ms = 0U;
    g_one_hmi.waveform_pending_sequence = 0U;
    g_one_hmi.waveform_snapshot_requested = 0U;
    g_one_hmi.plot_last_sequence[ONE_HMI_PLOT_TIME_DOMAIN] = 0U;
    g_one_hmi.plot_last_sequence[ONE_HMI_PLOT_SPECTRUM] = 0U;
    g_one_hmi.status.waveform_last_sequence = 0U;
    g_one_hmi.status.active_plot = ONE_HMI_PLOT_TIME_DOMAIN;
    g_one_hmi.status.current_page_known = 0U;
    g_one_hmi.status.waveform_snapshot_pending = 0U;
    one_hmi_waveform_schedule_next();
}

static HAL_StatusTypeDef one_hmi_waveform_fail(HAL_StatusTypeDef status)
{
    one_hmi_waveform_set_state(ONE_HMI_WAVEFORM_STATE_IDLE);
    g_one_hmi.status.waveform_last_hal_status = status;
    g_one_hmi.status.waveform_error_count++;
    if (status == HAL_TIMEOUT) {
        g_one_hmi.status.waveform_timeout_count++;
    }
    if (TJC_SCREEN_PLOT_SNAPSHOT_MODE != 0U) {
        g_one_hmi.waveform_snapshot_requested = 0U;
        g_one_hmi.status.waveform_snapshot_pending = 0U;
        g_one_hmi.status.active_plot = ONE_HMI_PLOT_TIME_DOMAIN;
    } else {
        g_one_hmi.status.active_plot =
            (g_one_hmi.status.active_plot == ONE_HMI_PLOT_TIME_DOMAIN) ?
                ONE_HMI_PLOT_SPECTRUM :
                ONE_HMI_PLOT_TIME_DOMAIN;
        one_hmi_waveform_schedule_next();
    }
    return status;
}

static uint8_t one_hmi_waveform_scale_sample(float sample_mv,
                                              float minimum_mv,
                                              float maximum_mv)
{
    const float display_span =
        (float)(ONE_HMI_TIME_DISPLAY_MAX - ONE_HMI_TIME_DISPLAY_MIN);
    float scaled;

    if (maximum_mv <= minimum_mv) {
        return (uint8_t)((ONE_HMI_TIME_DISPLAY_MIN +
                          ONE_HMI_TIME_DISPLAY_MAX) /
                         2U);
    }

    if (sample_mv < minimum_mv) {
        sample_mv = minimum_mv;
    } else if (sample_mv > maximum_mv) {
        sample_mv = maximum_mv;
    }

    scaled = (float)ONE_HMI_TIME_DISPLAY_MIN +
             (((sample_mv - minimum_mv) * display_span) /
              (maximum_mv - minimum_mv));
    return (uint8_t)(scaled + 0.5f);
}

/* 根据串口屏的实际绘制方向调整点序，保证时间轴从左向右。 */
static void one_hmi_waveform_apply_display_direction(void)
{
#if TJC_SCREEN_REVERSE_TIME_PLOT_POINTS
    uint16_t left = 0U;
    uint16_t right = TJC_SCREEN_PLOT_POINT_COUNT - 1U;

    while (left < right) {
        const uint8_t temporary = g_one_hmi.waveform_points[left];
        g_one_hmi.waveform_points[left] = g_one_hmi.waveform_points[right];
        g_one_hmi.waveform_points[right] = temporary;
        ++left;
        --right;
    }
#endif
}

/* 频谱横轴必须保持低频在左、高频在右。 */
static void one_hmi_spectrum_apply_display_direction(void)
{
#if TJC_SCREEN_REVERSE_SPECTRUM_PLOT_POINTS
    uint16_t left = 0U;
    uint16_t right = TJC_SCREEN_PLOT_POINT_COUNT - 1U;

    while (left < right) {
        const uint8_t temporary = g_one_hmi.waveform_points[left];
        g_one_hmi.waveform_points[left] = g_one_hmi.waveform_points[right];
        g_one_hmi.waveform_points[right] = temporary;
        ++left;
        --right;
    }
#endif
}

/* 波形控件无法绘制真正的竖线，用窄脉冲增强单像素谱线的可见度。 */
static void one_hmi_spectrum_draw_line(uint32_t center_x, uint8_t y)
{
    const uint32_t first =
        (center_x > ONE_HMI_SPECTRUM_LINE_HALF_WIDTH_POINTS) ?
            center_x - ONE_HMI_SPECTRUM_LINE_HALF_WIDTH_POINTS :
            0U;
    uint32_t last = center_x + ONE_HMI_SPECTRUM_LINE_HALF_WIDTH_POINTS;

    if (last >= TJC_SCREEN_PLOT_POINT_COUNT) {
        last = TJC_SCREEN_PLOT_POINT_COUNT - 1U;
    }

    for (uint32_t x = first; x <= last; ++x) {
        if (y > g_one_hmi.waveform_points[x]) {
            g_one_hmi.waveform_points[x] = y;
        }
    }
}

static uint8_t one_hmi_waveform_prepare(void)
{
    const signal_measurement_result_t *measurement =
        Signal_Measurement_Service_GetResult();
    float minimum_mv;
    float maximum_mv;

    if ((measurement == NULL) || (measurement->result_ready == 0U)) {
        return 0U;
    }
    if (measurement->measurement_count ==
        g_one_hmi.plot_last_sequence[ONE_HMI_PLOT_TIME_DOMAIN]) {
        return 0U;
    }

    /*
     * 第三问的时域曲线只由 10 kHz～500 kHz 有效谐波重构，
     * 因此不会把原始 ADC 中不低于 1 MHz 的干扰画到屏幕上。
     */
    if (Signal_Reconstruction_Service_Generate(
            measurement,
            g_one_hmi.status.waveform_cycles,
            g_one_hmi.waveform_reconstructed_mv,
            TJC_SCREEN_PLOT_POINT_COUNT,
            &minimum_mv,
            &maximum_mv) == 0U) {
        return 0U;
    }

    for (uint16_t index = 0U;
         index < TJC_SCREEN_PLOT_POINT_COUNT;
         ++index) {
        g_one_hmi.waveform_points[index] =
            one_hmi_waveform_scale_sample(
                g_one_hmi.waveform_reconstructed_mv[index],
                minimum_mv,
                maximum_mv);
    }

    one_hmi_waveform_apply_display_direction();

    g_one_hmi.waveform_pending_sequence = measurement->measurement_count;
    return 1U;
}

static uint8_t one_hmi_spectrum_prepare(void)
{
    const signal_measurement_result_t *measurement =
        Signal_Measurement_Service_GetResult();
    float max_amplitude = 0.0f;
    float max_frequency = 0.0f;

    if ((measurement == NULL) || (measurement->result_ready == 0U) ||
        (measurement->measurement_count ==
         g_one_hmi.plot_last_sequence[ONE_HMI_PLOT_SPECTRUM])) {
        return 0U;
    }

    for (uint16_t index = 0U;
         index < TJC_SCREEN_PLOT_POINT_COUNT;
         ++index) {
        g_one_hmi.waveform_points[index] =
            ONE_HMI_SPECTRUM_DISPLAY_MIN;
    }

    for (uint8_t index = 0U;
         index < measurement->component_count;
         ++index) {
        if ((measurement->components[index].valid != 0U) &&
            (measurement->components[index].amplitude_mv > max_amplitude)) {
            max_amplitude = measurement->components[index].amplitude_mv;
        }
        if ((measurement->components[index].valid != 0U) &&
            (measurement->components[index].frequency_hz > max_frequency)) {
            max_frequency = measurement->components[index].frequency_hz;
        }
    }

    if ((max_amplitude > 0.0f) && (max_frequency > 0.0f)) {
        const float frequency_span =
            max_frequency * ONE_HMI_SPECTRUM_RIGHT_HEADROOM_RATIO;
        const float display_span =
            (float)(ONE_HMI_SPECTRUM_DISPLAY_MAX -
                    ONE_HMI_SPECTRUM_DISPLAY_MIN);
        const uint32_t horizontal_span =
            TJC_SCREEN_PLOT_POINT_COUNT - 1U -
            ONE_HMI_SPECTRUM_LEFT_MARGIN_POINTS -
            ONE_HMI_SPECTRUM_RIGHT_MARGIN_POINTS;

        for (uint8_t index = 0U;
             index < measurement->component_count;
             ++index) {
            const float frequency =
                measurement->components[index].frequency_hz;
            uint32_t x;
            uint32_t y;

            if ((measurement->components[index].valid == 0U) ||
                (frequency <= 0.0f)) {
                continue;
            }

            x = ONE_HMI_SPECTRUM_LEFT_MARGIN_POINTS +
                (uint32_t)(((frequency * (float)horizontal_span) /
                            frequency_span) +
                           0.5f);
            if (x > (TJC_SCREEN_PLOT_POINT_COUNT - 1U -
                     ONE_HMI_SPECTRUM_RIGHT_MARGIN_POINTS)) {
                x = TJC_SCREEN_PLOT_POINT_COUNT - 1U -
                    ONE_HMI_SPECTRUM_RIGHT_MARGIN_POINTS;
            }
            y = ONE_HMI_SPECTRUM_DISPLAY_MIN +
                (uint32_t)(((measurement->components[index].amplitude_mv /
                             max_amplitude) *
                            display_span) +
                           0.5f);
            if (y > ONE_HMI_SPECTRUM_DISPLAY_MAX) {
                y = ONE_HMI_SPECTRUM_DISPLAY_MAX;
            }
            one_hmi_spectrum_draw_line(x, (uint8_t)y);
        }
    }

    one_hmi_spectrum_apply_display_direction();

    g_one_hmi.waveform_pending_sequence =
        measurement->measurement_count;
    return 1U;
}

static uint8_t one_hmi_plot_prepare(void)
{
    if (g_one_hmi.status.active_plot == ONE_HMI_PLOT_SPECTRUM) {
        return one_hmi_spectrum_prepare();
    }

    return one_hmi_waveform_prepare();
}

static uint8_t one_hmi_plot_component_id(void)
{
    return (g_one_hmi.status.active_plot == ONE_HMI_PLOT_SPECTRUM) ?
               TJC_SCREEN_SPECTRUM_PLOT_COMPONENT_ID :
               TJC_SCREEN_TIME_PLOT_COMPONENT_ID;
}

static int32_t one_hmi_round_nonnegative(float value)
{
    if (value <= 0.0f) {
        return 0;
    }
    if (value >= (float)INT32_MAX) {
        return INT32_MAX;
    }

    return (int32_t)(value + 0.5f);
}

static int32_t one_hmi_measurement_field_value(uint8_t field_index)
{
    const signal_measurement_result_t *measurement =
        &g_one_hmi.measurement_snapshot;
    uint8_t component_index;

    switch (field_index) {
    case 0U:
        return measurement->signal_valid;
    case 1U:
        return one_hmi_round_nonnegative(
            measurement->peak_to_peak_mv * 10.0f);
    case 2U:
        return one_hmi_round_nonnegative(
            measurement->true_rms_mv * 10.0f);
    case 3U:
        return one_hmi_round_nonnegative(
            measurement->fundamental_frequency_hz);
    case 4U:
        return measurement->component_count;
    case 11U:
        return g_one_hmi.status.waveform_cycles;
    case 12U:
    case 13U:
        component_index = (uint8_t)(field_index - 11U);
        return (measurement->components[component_index].valid != 0U) ?
                   measurement->components[component_index].harmonic_order :
                   0;
    default:
        break;
    }

    component_index = (uint8_t)((field_index - 5U) / 2U);
    if ((component_index >= SIGNAL_MEASUREMENT_COMPONENT_COUNT) ||
        (measurement->components[component_index].valid == 0U)) {
        return 0;
    }
    if (((field_index - 5U) & 1U) == 0U) {
        return one_hmi_round_nonnegative(
            measurement->components[component_index].frequency_hz);
    }

    return one_hmi_round_nonnegative(
        measurement->components[component_index].amplitude_mv * 10.0f);
}

static HAL_StatusTypeDef one_hmi_measurement_publish_process(void)
{
    const signal_measurement_result_t *measurement;
    HAL_StatusTypeDef status;

    if (g_one_hmi.measurement_publish_active != 0U) {
        if (g_one_hmi.measurement_field_index >=
            ONE_HMI_MEASUREMENT_FIELD_COUNT) {
            g_one_hmi.measurement_publish_active = 0U;
            g_one_hmi.status.measurement_last_sequence =
                g_one_hmi.measurement_pending_sequence;
            g_one_hmi.status.measurement_publish_count++;
            one_hmi_measurement_schedule_next();
            return HAL_OK;
        }

        status = Usart_HMI_Service_SetNumber(
            g_one_hmi_measurement_names[
                g_one_hmi.measurement_field_index],
            one_hmi_measurement_field_value(
                g_one_hmi.measurement_field_index));
        if (status == HAL_OK) {
            g_one_hmi.measurement_field_index++;
        } else if (status != HAL_BUSY) {
            g_one_hmi.measurement_publish_active = 0U;
            g_one_hmi.status.measurement_publish_error_count++;
            one_hmi_measurement_schedule_next();
        }
        return status;
    }

    if ((g_one_hmi.status.waveform_state !=
         ONE_HMI_WAVEFORM_STATE_IDLE) ||
        (one_hmi_time_reached(
             g_one_hmi.measurement_next_refresh_ms) == 0U)) {
        return HAL_OK;
    }

    measurement = Signal_Measurement_Service_GetResult();
    if ((measurement == NULL) || (measurement->result_ready == 0U) ||
        (measurement->measurement_count ==
         g_one_hmi.status.measurement_last_sequence)) {
        one_hmi_measurement_schedule_next();
        return HAL_OK;
    }

    g_one_hmi.measurement_snapshot = *measurement;
    g_one_hmi.measurement_pending_sequence =
        measurement->measurement_count;
    g_one_hmi.measurement_field_index = 0U;
    g_one_hmi.measurement_publish_active = 1U;
    return HAL_OK;
}

static HAL_StatusTypeDef one_hmi_send_bytes(const uint8_t *data,
                                             uint16_t length,
                                             void *user_context)
{
    (void)user_context;
    return TJC_UART_Driver_Transmit(data, length);
}

static void one_hmi_waveform_handle_current_page(uint8_t page_id)
{
    g_one_hmi.status.current_page_known = 1U;
    g_one_hmi.status.current_page_id = page_id;

    if (g_one_hmi.status.waveform_state !=
        ONE_HMI_WAVEFORM_STATE_WAIT_PAGE) {
        return;
    }

    if (page_id == TJC_SCREEN_MEASUREMENT_PAGE_ID) {
        one_hmi_waveform_set_state(ONE_HMI_WAVEFORM_STATE_CLEAR);
        g_one_hmi.waveform_deadline_ms =
            HAL_GetTick() + ONE_HMI_WAVEFORM_TRANSFER_TIMEOUT_MS;
    } else {
        one_hmi_waveform_set_state(ONE_HMI_WAVEFORM_STATE_IDLE);
        g_one_hmi.status.waveform_page_skip_count++;
        if (TJC_SCREEN_PLOT_SNAPSHOT_MODE != 0U) {
            g_one_hmi.waveform_snapshot_requested = 0U;
            g_one_hmi.status.waveform_snapshot_pending = 0U;
        } else {
            one_hmi_waveform_schedule_next();
        }
    }
}

static void one_hmi_waveform_handle_ready(void)
{
    if (g_one_hmi.status.waveform_state ==
        ONE_HMI_WAVEFORM_STATE_WAIT_READY) {
        one_hmi_waveform_set_state(ONE_HMI_WAVEFORM_STATE_SEND_DATA);
        g_one_hmi.waveform_deadline_ms =
            HAL_GetTick() + ONE_HMI_WAVEFORM_TRANSFER_TIMEOUT_MS;
    } else {
        g_one_hmi.status.waveform_unexpected_event_count++;
    }
}

static void one_hmi_waveform_handle_finished(void)
{
    if (g_one_hmi.status.waveform_state ==
        ONE_HMI_WAVEFORM_STATE_WAIT_FINISHED) {
        one_hmi_waveform_set_state(ONE_HMI_WAVEFORM_STATE_IDLE);
        g_one_hmi.status.waveform_last_hal_status = HAL_OK;
        g_one_hmi.status.waveform_last_sequence =
            g_one_hmi.waveform_pending_sequence;
        g_one_hmi.plot_last_sequence[g_one_hmi.status.active_plot] =
            g_one_hmi.waveform_pending_sequence;
        g_one_hmi.status.waveform_transfer_count++;
        if ((TJC_SCREEN_PLOT_SNAPSHOT_MODE != 0U) &&
            (g_one_hmi.status.active_plot ==
             ONE_HMI_PLOT_TIME_DOMAIN)) {
            g_one_hmi.status.active_plot = ONE_HMI_PLOT_SPECTRUM;
            g_one_hmi.waveform_next_refresh_ms =
                HAL_GetTick() + ONE_HMI_PLOT_SWITCH_DELAY_MS;
        } else if (TJC_SCREEN_PLOT_SNAPSHOT_MODE != 0U) {
            /* 手动刷新一次包含时域和频谱两次透明传输。 */
            g_one_hmi.waveform_snapshot_requested = 0U;
            g_one_hmi.status.waveform_snapshot_pending = 0U;
            g_one_hmi.status.active_plot = ONE_HMI_PLOT_TIME_DOMAIN;
        } else if (g_one_hmi.status.active_plot ==
                   ONE_HMI_PLOT_TIME_DOMAIN) {
            g_one_hmi.status.active_plot = ONE_HMI_PLOT_SPECTRUM;
            g_one_hmi.waveform_next_refresh_ms =
                HAL_GetTick() + ONE_HMI_PLOT_SWITCH_DELAY_MS;
        } else {
            g_one_hmi.status.active_plot = ONE_HMI_PLOT_TIME_DOMAIN;
            one_hmi_waveform_schedule_next();
        }
    } else {
        g_one_hmi.status.waveform_unexpected_event_count++;
    }
}

static HAL_StatusTypeDef one_hmi_waveform_process(void)
{
    HAL_StatusTypeDef status;

    if (TJC_SCREEN_ENABLE_PLOT_TRANSFER == 0U) {
        return HAL_OK;
    }

    switch (g_one_hmi.status.waveform_state) {
    case ONE_HMI_WAVEFORM_STATE_IDLE:
        if ((TJC_SCREEN_PLOT_SNAPSHOT_MODE != 0U) &&
            (g_one_hmi.waveform_snapshot_requested == 0U)) {
            return HAL_OK;
        }
        if ((g_one_hmi.measurement_publish_active != 0U) ||
            (one_hmi_time_reached(
                 g_one_hmi.waveform_next_refresh_ms) == 0U)) {
            return HAL_OK;
        }
        if (one_hmi_plot_prepare() == 0U) {
            one_hmi_waveform_schedule_next();
            return HAL_OK;
        }
        one_hmi_waveform_set_state(ONE_HMI_WAVEFORM_STATE_REQUEST_PAGE);
        g_one_hmi.waveform_deadline_ms =
            HAL_GetTick() + ONE_HMI_WAVEFORM_TRANSFER_TIMEOUT_MS;
        return HAL_OK;

    case ONE_HMI_WAVEFORM_STATE_REQUEST_PAGE:
        if (one_hmi_time_reached(g_one_hmi.waveform_deadline_ms) != 0U) {
            return one_hmi_waveform_fail(HAL_TIMEOUT);
        }
        status = Usart_HMI_Service_RequestCurrentPage();
        if (status == HAL_OK) {
            one_hmi_waveform_set_state(ONE_HMI_WAVEFORM_STATE_WAIT_PAGE);
            g_one_hmi.waveform_deadline_ms =
                HAL_GetTick() + ONE_HMI_WAVEFORM_PAGE_TIMEOUT_MS;
        } else if (status != HAL_BUSY) {
            return one_hmi_waveform_fail(status);
        }
        return HAL_OK;

    case ONE_HMI_WAVEFORM_STATE_WAIT_PAGE:
        if (one_hmi_time_reached(g_one_hmi.waveform_deadline_ms) != 0U) {
            return one_hmi_waveform_fail(HAL_TIMEOUT);
        }
        return HAL_OK;

    case ONE_HMI_WAVEFORM_STATE_CLEAR:
        if (one_hmi_time_reached(g_one_hmi.waveform_deadline_ms) != 0U) {
            return one_hmi_waveform_fail(HAL_TIMEOUT);
        }
        status = Usart_HMI_Service_ClearWaveform(
            one_hmi_plot_component_id(),
            TJC_SCREEN_PLOT_CHANNEL);
        if (status == HAL_OK) {
            one_hmi_waveform_set_state(
                ONE_HMI_WAVEFORM_STATE_BEGIN_TRANSFER);
        } else if (status != HAL_BUSY) {
            return one_hmi_waveform_fail(status);
        }
        return HAL_OK;

    case ONE_HMI_WAVEFORM_STATE_BEGIN_TRANSFER:
        if (one_hmi_time_reached(g_one_hmi.waveform_deadline_ms) != 0U) {
            return one_hmi_waveform_fail(HAL_TIMEOUT);
        }
        status = Usart_HMI_Service_BeginWaveformTransfer(
            one_hmi_plot_component_id(),
            TJC_SCREEN_PLOT_CHANNEL,
            TJC_SCREEN_PLOT_POINT_COUNT);
        if (status == HAL_OK) {
            one_hmi_waveform_set_state(ONE_HMI_WAVEFORM_STATE_WAIT_READY);
            g_one_hmi.waveform_deadline_ms =
                HAL_GetTick() + ONE_HMI_WAVEFORM_TRANSFER_TIMEOUT_MS;
        } else if (status != HAL_BUSY) {
            return one_hmi_waveform_fail(status);
        }
        return HAL_OK;

    case ONE_HMI_WAVEFORM_STATE_WAIT_READY:
        if (one_hmi_time_reached(g_one_hmi.waveform_deadline_ms) != 0U) {
            return one_hmi_waveform_fail(HAL_TIMEOUT);
        }
        return HAL_OK;

    case ONE_HMI_WAVEFORM_STATE_SEND_DATA:
        if (one_hmi_time_reached(g_one_hmi.waveform_deadline_ms) != 0U) {
            return one_hmi_waveform_fail(HAL_TIMEOUT);
        }
        status = Usart_HMI_Service_SendTransparentData(
            g_one_hmi.waveform_points,
            TJC_SCREEN_PLOT_POINT_COUNT);
        if (status == HAL_OK) {
            one_hmi_waveform_set_state(
                ONE_HMI_WAVEFORM_STATE_WAIT_FINISHED);
            g_one_hmi.waveform_deadline_ms =
                HAL_GetTick() + ONE_HMI_WAVEFORM_TRANSFER_TIMEOUT_MS;
        } else if (status != HAL_BUSY) {
            return one_hmi_waveform_fail(status);
        }
        return HAL_OK;

    case ONE_HMI_WAVEFORM_STATE_WAIT_FINISHED:
        if (one_hmi_time_reached(g_one_hmi.waveform_deadline_ms) != 0U) {
            return one_hmi_waveform_fail(HAL_TIMEOUT);
        }
        return HAL_OK;

    default:
        return one_hmi_waveform_fail(HAL_ERROR);
    }
}

static void one_hmi_handle_event(const usart_hmi_event_t *event)
{
    HAL_StatusTypeDef touch_status;

    g_one_hmi.status.link_alive = 1U;
    g_one_hmi.status.last_rx_tick_ms = HAL_GetTick();

    switch (event->type) {
    case USART_HMI_EVENT_TOUCH:
        g_one_hmi.status.current_page_known = 1U;
        g_one_hmi.status.current_page_id = event->data.touch.page_id;
        if (event->data.touch.state == USART_HMI_TOUCH_RELEASE) {
            g_one_hmi.status.touch_count++;
            if (event->data.touch.page_id ==
                TJC_SCREEN_MEASUREMENT_PAGE_ID) {
                if (event->data.touch.component_id ==
                    TJC_SCREEN_ONE_CYCLE_BUTTON_ID) {
                    (void)Signal_HMI_App_SetWaveformCycles(1U);
                } else if (event->data.touch.component_id ==
                           TJC_SCREEN_THREE_CYCLE_BUTTON_ID) {
                    (void)Signal_HMI_App_SetWaveformCycles(3U);
                } else if (event->data.touch.component_id ==
                           TJC_SCREEN_SNAPSHOT_BUTTON_COMPONENT_ID) {
                    touch_status =
                        Signal_HMI_App_RequestWaveformSnapshot();
                    if (touch_status == HAL_BUSY) {
                        g_one_hmi.status.waveform_snapshot_busy_count++;
                    } else if (touch_status != HAL_OK) {
                        g_one_hmi.status.waveform_snapshot_rejected_count++;
                    }
                }
            }
        }
        break;

    case USART_HMI_EVENT_CURRENT_PAGE:
        one_hmi_waveform_handle_current_page(
            event->data.current_page.page_id);
        break;

    case USART_HMI_EVENT_STARTUP:
        g_one_hmi.status.startup_count++;
        one_hmi_waveform_reset();
        break;

    case USART_HMI_EVENT_NUMBER:
        g_one_hmi.status.ignored_number_count++;
        break;

    case USART_HMI_EVENT_RETURN_STATUS:
        g_one_hmi.status.last_return_status =
            event->data.return_status.code;
        if (event->data.return_status.code != 0x01U) {
            g_one_hmi.status.return_status_error_count++;
        }
        break;

    case USART_HMI_EVENT_TRANSPARENT_READY:
        one_hmi_waveform_handle_ready();
        break;

    case USART_HMI_EVENT_TRANSPARENT_FINISHED:
        one_hmi_waveform_handle_finished();
        break;

    default:
        break;
    }
}

HAL_StatusTypeDef Signal_HMI_App_Init(UART_HandleTypeDef *uart)
{
    const usart_hmi_service_config_t service_config = {
        .send_bytes = one_hmi_send_bytes,
        .user_context = NULL,
        .async_tx = 1U,
    };
    HAL_StatusTypeDef status;

    if ((uart == NULL) ||
        (uart->Init.BaudRate != TJC_SCREEN_UART_BAUD_RATE)) {
        return HAL_ERROR;
    }

    g_one_hmi = (one_hmi_context_t){0};
    g_one_hmi.observed_rx_restart_count = 0U;
    g_one_hmi.observed_rx_dropped_count = 0U;
    g_one_hmi.status.error = ONE_HMI_ERROR_NONE;
    g_one_hmi.status.last_hal_status = HAL_OK;
    g_one_hmi.status.waveform_last_hal_status = HAL_OK;
    g_one_hmi.status.waveform_cycles = 3U;
    g_one_hmi.measurement_publish_active = 0U;
    g_one_hmi.measurement_field_index = 0U;
    g_one_hmi.measurement_pending_sequence = 0U;
    g_one_hmi.measurement_snapshot =
        (signal_measurement_result_t){0};
    g_one_hmi.measurement_next_refresh_ms =
        HAL_GetTick() + ONE_HMI_STARTUP_DELAY_MS;
    one_hmi_waveform_reset();

    status = Usart_HMI_Service_Init(&service_config);
    if (status == HAL_OK) {
        status = TJC_UART_Driver_Init(uart);
    }
    if (status != HAL_OK) {
        g_one_hmi.status.error = ONE_HMI_ERROR_TRANSPORT;
        g_one_hmi.status.last_hal_status = status;
        return status;
    }

    return HAL_OK;
}

HAL_StatusTypeDef Signal_HMI_App_Process(void)
{
    const tjc_uart_driver_status_t *driver_status;
    uint8_t rx_buffer[ONE_HMI_RX_TRANSFER_BUFFER_SIZE];
    usart_hmi_event_t event;
    HAL_StatusTypeDef result;
    HAL_StatusTypeDef status;
    uint16_t rx_length;

    result = TJC_UART_Driver_Process();
    if (result != HAL_OK) {
        g_one_hmi.status.error = ONE_HMI_ERROR_TRANSPORT;
        g_one_hmi.status.last_hal_status = result;
        g_one_hmi.status.transport_error_count++;
    }

    if (TJC_UART_Driver_TakeTxComplete() != 0U) {
        Usart_HMI_Service_NotifyTxComplete();
    }

    do {
        rx_length = TJC_UART_Driver_Read(rx_buffer, sizeof(rx_buffer));
        if (rx_length != 0U) {
            status = Usart_HMI_Service_PushRxBytes(rx_buffer, rx_length);
            if (status != HAL_OK) {
                g_one_hmi.status.error = ONE_HMI_ERROR_TRANSPORT;
                g_one_hmi.status.last_hal_status = status;
                g_one_hmi.status.rx_push_error_count++;
                result = status;
            }
            Usart_HMI_Service_Process();
        }
    } while (rx_length != 0U);

    driver_status = TJC_UART_Driver_GetStatus();
    if ((driver_status->rx_restart_count !=
         g_one_hmi.observed_rx_restart_count) ||
        (driver_status->rx_bytes_dropped !=
         g_one_hmi.observed_rx_dropped_count)) {
        g_one_hmi.observed_rx_restart_count =
            driver_status->rx_restart_count;
        g_one_hmi.observed_rx_dropped_count =
            driver_status->rx_bytes_dropped;
        Usart_HMI_Service_ResetRx();
        g_one_hmi.measurement_publish_active = 0U;
        one_hmi_measurement_schedule_next();
        one_hmi_waveform_reset();
        g_one_hmi.status.transport_error_count++;
    }

    Usart_HMI_Service_Process();
    while (Usart_HMI_Service_ReadEvent(&event) != 0U) {
        one_hmi_handle_event(&event);
    }

    if ((g_one_hmi.status.link_alive != 0U) &&
        (one_hmi_time_reached(g_one_hmi.status.last_rx_tick_ms +
                              ONE_HMI_LINK_TIMEOUT_MS) != 0U)) {
        g_one_hmi.status.link_alive = 0U;
    }

    status = one_hmi_measurement_publish_process();
    if ((status != HAL_OK) && (status != HAL_BUSY)) {
        result = status;
    }

    status = one_hmi_waveform_process();
    if ((status != HAL_OK) && (status != HAL_BUSY)) {
        result = status;
    }

    if (result == HAL_OK) {
        g_one_hmi.status.error = ONE_HMI_ERROR_NONE;
        g_one_hmi.status.last_hal_status = HAL_OK;
    }

    return result;
}

HAL_StatusTypeDef Signal_HMI_App_SetWaveformCycles(
    uint8_t cycle_count)
{
    if ((cycle_count != 1U) && (cycle_count != 3U)) {
        return HAL_ERROR;
    }

    g_one_hmi.status.waveform_cycles = cycle_count;
    g_one_hmi.plot_last_sequence[ONE_HMI_PLOT_TIME_DOMAIN] = 0U;
    if (g_one_hmi.status.waveform_state ==
        ONE_HMI_WAVEFORM_STATE_IDLE) {
        g_one_hmi.status.active_plot = ONE_HMI_PLOT_TIME_DOMAIN;
    }
    g_one_hmi.waveform_next_refresh_ms = HAL_GetTick();
    g_one_hmi.status.measurement_last_sequence = 0U;
    g_one_hmi.measurement_next_refresh_ms = HAL_GetTick();
    return HAL_OK;
}

HAL_StatusTypeDef Signal_HMI_App_RequestWaveformSnapshot(void)
{
    if ((TJC_SCREEN_ENABLE_PLOT_TRANSFER == 0U) ||
        (TJC_SCREEN_TIME_PLOT_COMPONENT_ID ==
         TJC_SCREEN_UNASSIGNED_COMPONENT_ID) ||
        (TJC_SCREEN_SNAPSHOT_BUTTON_COMPONENT_ID ==
         TJC_SCREEN_UNASSIGNED_COMPONENT_ID)) {
        return HAL_ERROR;
    }

    if (g_one_hmi.status.waveform_state !=
        ONE_HMI_WAVEFORM_STATE_IDLE) {
        return HAL_BUSY;
    }

    g_one_hmi.status.waveform_snapshot_request_count++;
    g_one_hmi.status.active_plot = ONE_HMI_PLOT_TIME_DOMAIN;
    g_one_hmi.plot_last_sequence[ONE_HMI_PLOT_TIME_DOMAIN] = 0U;
    g_one_hmi.plot_last_sequence[ONE_HMI_PLOT_SPECTRUM] = 0U;
    g_one_hmi.waveform_next_refresh_ms = HAL_GetTick();

    if (TJC_SCREEN_PLOT_SNAPSHOT_MODE != 0U) {
        g_one_hmi.waveform_snapshot_requested = 1U;
        g_one_hmi.status.waveform_snapshot_pending = 1U;
    }
    return HAL_OK;
}

const signal_hmi_status_t *Signal_HMI_App_GetStatus(void)
{
    return &g_one_hmi.status;
}
