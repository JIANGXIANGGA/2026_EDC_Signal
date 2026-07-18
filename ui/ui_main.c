/**
 * @file ui_main.c
 * @brief LVGL 信号显示与控制界面实现
 */

#include "ui.h"

#define UI_TAB_BAR_HEIGHT          32
#define UI_PAGE_PADDING             4
#define UI_PAGE_GAP                 3
#define UI_WAVE_HEADER_HEIGHT      18
#define UI_CONTROL_ROW_HEIGHT      48
#define UI_CONTROL_BUTTON_WIDTH    38
#define UI_CONTROL_BUTTON_HEIGHT   34

#define UI_COLOR_SCREEN        0x0B1014U
#define UI_COLOR_PANEL         0x151C22U
#define UI_COLOR_CHART         0x090D10U
#define UI_COLOR_BORDER        0x33404AU
#define UI_COLOR_TEXT          0xF2F5F7U
#define UI_COLOR_TEXT_MUTED    0x9EABB4U
#define UI_COLOR_ADC           0x34D399U
#define UI_COLOR_DDS           0xF4B942U
#define UI_COLOR_FFT           0x38BDF8U
#define UI_COLOR_BUTTON        0x26343DU
#define UI_COLOR_BUTTON_PRESS  0x3A4C57U

typedef enum {
    UI_ACTION_SAMPLE_RATE_DOWN = 0,
    UI_ACTION_SAMPLE_RATE_UP,
    UI_ACTION_DDS_FREQ_DOWN,
    UI_ACTION_DDS_FREQ_UP,
    UI_ACTION_DDS_AMPLITUDE_DOWN,
    UI_ACTION_DDS_AMPLITUDE_UP,
    UI_ACTION_DDS_WAVEFORM_NEXT,
    UI_ACTION_COUNT
} ui_action_t;

typedef struct {
    lv_obj_t * chart;
    lv_chart_series_t * series;
} ui_wave_view_t;

static const ui_action_t g_actions[UI_ACTION_COUNT] = {
    UI_ACTION_SAMPLE_RATE_DOWN,
    UI_ACTION_SAMPLE_RATE_UP,
    UI_ACTION_DDS_FREQ_DOWN,
    UI_ACTION_DDS_FREQ_UP,
    UI_ACTION_DDS_AMPLITUDE_DOWN,
    UI_ACTION_DDS_AMPLITUDE_UP,
    UI_ACTION_DDS_WAVEFORM_NEXT,
};

static ui_signal_callbacks_t g_callbacks;
static ui_wave_view_t g_adc_view;
static ui_wave_view_t g_dds_view;
static ui_wave_view_t g_spectrum_view;

static lv_obj_t * g_wave_summary_label;
static lv_obj_t * g_fft_summary_label;
static lv_obj_t * g_fft_status_label;
static lv_obj_t * g_sample_rate_value_label;
static lv_obj_t * g_dds_freq_value_label;
static lv_obj_t * g_dds_amplitude_value_label;
static lv_obj_t * g_dds_waveform_value_label;

static void ui_set_container_style(lv_obj_t * obj, lv_color_t background)
{
    lv_obj_set_style_bg_color(obj, background, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
}

static void ui_set_panel_style(lv_obj_t * obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 4, LV_PART_MAIN);
}

static void ui_control_event_cb(lv_event_t * event)
{
    const ui_action_t * action = lv_event_get_user_data(event);

    if(action == NULL) {
        return;
    }

    switch(*action) {
        case UI_ACTION_SAMPLE_RATE_DOWN:
            if(g_callbacks.on_sample_rate_down != NULL) {
                g_callbacks.on_sample_rate_down();
            }
            break;

        case UI_ACTION_SAMPLE_RATE_UP:
            if(g_callbacks.on_sample_rate_up != NULL) {
                g_callbacks.on_sample_rate_up();
            }
            break;

        case UI_ACTION_DDS_FREQ_DOWN:
            if(g_callbacks.on_dds_freq_down != NULL) {
                g_callbacks.on_dds_freq_down();
            }
            break;

        case UI_ACTION_DDS_FREQ_UP:
            if(g_callbacks.on_dds_freq_up != NULL) {
                g_callbacks.on_dds_freq_up();
            }
            break;

        case UI_ACTION_DDS_AMPLITUDE_DOWN:
            if(g_callbacks.on_dds_amplitude_down != NULL) {
                g_callbacks.on_dds_amplitude_down();
            }
            break;

        case UI_ACTION_DDS_AMPLITUDE_UP:
            if(g_callbacks.on_dds_amplitude_up != NULL) {
                g_callbacks.on_dds_amplitude_up();
            }
            break;

        case UI_ACTION_DDS_WAVEFORM_NEXT:
            if(g_callbacks.on_dds_waveform_next != NULL) {
                g_callbacks.on_dds_waveform_next();
            }
            break;

        case UI_ACTION_COUNT:
        default:
            break;
    }
}

static lv_obj_t * ui_create_control_button(lv_obj_t * parent,
                                           const char * text,
                                           lv_coord_t width,
                                           ui_action_t action)
{
    lv_obj_t * button = lv_button_create(parent);
    lv_obj_t * label;

    lv_obj_set_size(button, width, UI_CONTROL_BUTTON_HEIGHT);
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_BUTTON), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_BUTTON_PRESS), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(button, ui_control_event_cb, LV_EVENT_CLICKED, (void *)&g_actions[action]);

    label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(label);

    return button;
}

static lv_obj_t * ui_create_control_text(lv_obj_t * parent, const char * title)
{
    lv_obj_t * text_container = lv_obj_create(parent);
    lv_obj_t * title_label;
    lv_obj_t * value_label;

    lv_obj_set_size(text_container, 0, lv_pct(100));
    lv_obj_set_flex_grow(text_container, 1);
    ui_set_container_style(text_container, lv_color_hex(UI_COLOR_PANEL));
    lv_obj_set_style_pad_all(text_container, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(text_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(text_container,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(text_container, LV_OBJ_FLAG_SCROLLABLE);

    title_label = lv_label_create(text_container);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_14, LV_PART_MAIN);

    value_label = lv_label_create(text_container);
    lv_label_set_text(value_label, "--");
    lv_obj_set_width(value_label, lv_pct(100));
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(value_label, lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_16, LV_PART_MAIN);

    return value_label;
}

static lv_obj_t * ui_create_adjust_row(lv_obj_t * parent,
                                       const char * title,
                                       ui_action_t down_action,
                                       ui_action_t up_action)
{
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_t * value_label;

    lv_obj_set_size(row, lv_pct(100), UI_CONTROL_ROW_HEIGHT);
    ui_set_panel_style(row);
    lv_obj_set_style_pad_all(row, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    value_label = ui_create_control_text(row, title);
    (void)ui_create_control_button(row, LV_SYMBOL_MINUS, UI_CONTROL_BUTTON_WIDTH, down_action);
    (void)ui_create_control_button(row, LV_SYMBOL_PLUS, UI_CONTROL_BUTTON_WIDTH, up_action);

    return value_label;
}

static lv_obj_t * ui_create_waveform_row(lv_obj_t * parent)
{
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_t * value_label;

    lv_obj_set_size(row, lv_pct(100), UI_CONTROL_ROW_HEIGHT);
    ui_set_panel_style(row);
    lv_obj_set_style_pad_all(row, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    value_label = ui_create_control_text(row, "DDS waveform");
    (void)ui_create_control_button(row, "Next", 66, UI_ACTION_DDS_WAVEFORM_NEXT);

    return value_label;
}

static void ui_create_wave_panel(lv_obj_t * parent,
                                 const char * title,
                                 lv_color_t color,
                                 int32_t range_min,
                                 int32_t range_max,
                                 ui_wave_view_t * view)
{
    lv_obj_t * panel = lv_obj_create(parent);
    lv_obj_t * title_label;

    lv_obj_set_size(panel, lv_pct(100), 0);
    lv_obj_set_flex_grow(panel, 1);
    ui_set_panel_style(panel);
    lv_obj_set_style_pad_row(panel, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    title_label = lv_label_create(panel);
    lv_obj_set_size(title_label, lv_pct(100), UI_WAVE_HEADER_HEIGHT);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, color, LV_PART_MAIN);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_14, LV_PART_MAIN);

    view->chart = lv_chart_create(panel);
    lv_obj_set_size(view->chart, lv_pct(100), 0);
    lv_obj_set_flex_grow(view->chart, 1);
    lv_chart_set_type(view->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_axis_range(view->chart,
                            LV_CHART_AXIS_PRIMARY_Y,
                            range_min,
                            range_max);
    lv_chart_set_div_line_count(view->chart, 3, 5);
    lv_obj_set_style_bg_color(view->chart, lv_color_hex(UI_COLOR_CHART), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(view->chart, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(view->chart, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(view->chart, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(view->chart, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(view->chart, 2, LV_PART_MAIN);
    lv_obj_set_style_line_color(view->chart, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_line_opa(view->chart, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_line_width(view->chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(view->chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_remove_flag(view->chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(view->chart, LV_OBJ_FLAG_SCROLLABLE);

    view->series = lv_chart_add_series(view->chart, color, LV_CHART_AXIS_PRIMARY_Y);
}

static void ui_create_wave_tab(lv_obj_t * tab)
{
    ui_set_container_style(tab, lv_color_hex(UI_COLOR_SCREEN));
    lv_obj_set_style_pad_all(tab, UI_PAGE_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_row(tab, UI_PAGE_GAP, LV_PART_MAIN);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    g_wave_summary_label = lv_label_create(tab);
    lv_obj_set_size(g_wave_summary_label, lv_pct(100), UI_WAVE_HEADER_HEIGHT);
    lv_label_set_text(g_wave_summary_label, "Fs -- Hz   DDS -- Hz");
    lv_label_set_long_mode(g_wave_summary_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(g_wave_summary_label, lv_color_hex(UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_wave_summary_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(g_wave_summary_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    ui_create_wave_panel(tab,
                         "ADC121 input",
                         lv_color_hex(UI_COLOR_ADC),
                         0,
                         4095,
                         &g_adc_view);
    ui_create_wave_panel(tab,
                         "DDS output",
                         lv_color_hex(UI_COLOR_DDS),
                         0,
                         4095,
                         &g_dds_view);
}

static void ui_create_spectrum_tab(lv_obj_t * tab)
{
    ui_set_container_style(tab, lv_color_hex(UI_COLOR_SCREEN));
    lv_obj_set_style_pad_all(tab, UI_PAGE_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_row(tab, UI_PAGE_GAP, LV_PART_MAIN);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    g_fft_summary_label = lv_label_create(tab);
    lv_obj_set_size(g_fft_summary_label,
                    lv_pct(100),
                    UI_WAVE_HEADER_HEIGHT);
    lv_label_set_text(g_fft_summary_label, "Peak -- Hz   A --");
    lv_label_set_long_mode(g_fft_summary_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(g_fft_summary_label,
                                lv_color_hex(UI_COLOR_TEXT),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(g_fft_summary_label,
                               &lv_font_montserrat_14,
                               LV_PART_MAIN);
    lv_obj_set_style_text_align(g_fft_summary_label,
                                LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);

    g_fft_status_label = lv_label_create(tab);
    lv_obj_set_size(g_fft_status_label,
                    lv_pct(100),
                    UI_WAVE_HEADER_HEIGHT);
    lv_label_set_text(g_fft_status_label, "RMS --   Drop --");
    lv_label_set_long_mode(g_fft_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(g_fft_status_label,
                                lv_color_hex(UI_COLOR_TEXT_MUTED),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(g_fft_status_label,
                               &lv_font_montserrat_14,
                               LV_PART_MAIN);
    lv_obj_set_style_text_align(g_fft_status_label,
                                LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);

    ui_create_wave_panel(tab,
                         "ADC spectrum (-80..0 dBFS)",
                         lv_color_hex(UI_COLOR_FFT),
                         0,
                         80,
                         &g_spectrum_view);
}

static void ui_create_control_tab(lv_obj_t * tab)
{
    ui_set_container_style(tab, lv_color_hex(UI_COLOR_SCREEN));
    lv_obj_set_style_pad_all(tab, UI_PAGE_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_row(tab, 5, LV_PART_MAIN);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    g_sample_rate_value_label = ui_create_adjust_row(tab,
                                                     "Sample rate",
                                                     UI_ACTION_SAMPLE_RATE_DOWN,
                                                     UI_ACTION_SAMPLE_RATE_UP);
    g_dds_freq_value_label = ui_create_adjust_row(tab,
                                                  "DDS frequency",
                                                  UI_ACTION_DDS_FREQ_DOWN,
                                                  UI_ACTION_DDS_FREQ_UP);
    g_dds_amplitude_value_label = ui_create_adjust_row(tab,
                                                       "DDS amplitude",
                                                       UI_ACTION_DDS_AMPLITUDE_DOWN,
                                                       UI_ACTION_DDS_AMPLITUDE_UP);
    g_dds_waveform_value_label = ui_create_waveform_row(tab);
}

static void ui_update_wave_view(ui_wave_view_t * view,
                                int32_t * points,
                                uint16_t point_count)
{
    if(view->chart == NULL || view->series == NULL || points == NULL || point_count < 2U) {
        return;
    }

    if(lv_chart_get_point_count(view->chart) != point_count) {
        lv_chart_set_point_count(view->chart, point_count);
    }

    lv_chart_set_series_ext_y_array(view->chart, view->series, points);
    lv_chart_refresh(view->chart);
}

void ui_init(const ui_signal_callbacks_t * callbacks)
{
    lv_obj_t * screen = lv_screen_active();
    lv_obj_t * tabview;
    lv_obj_t * wave_tab;
    lv_obj_t * spectrum_tab;
    lv_obj_t * control_tab;
    lv_obj_t * tab_bar;

    g_callbacks = (ui_signal_callbacks_t){0};
    if(callbacks != NULL) {
        g_callbacks = *callbacks;
    }

    g_adc_view = (ui_wave_view_t){0};
    g_dds_view = (ui_wave_view_t){0};
    g_spectrum_view = (ui_wave_view_t){0};
    g_wave_summary_label = NULL;
    g_fft_summary_label = NULL;
    g_fft_status_label = NULL;
    g_sample_rate_value_label = NULL;
    g_dds_freq_value_label = NULL;
    g_dds_amplitude_value_label = NULL;
    g_dds_waveform_value_label = NULL;

    lv_obj_clean(screen);
    ui_set_container_style(screen, lv_color_hex(UI_COLOR_SCREEN));

    tabview = lv_tabview_create(screen);
    lv_obj_set_size(tabview, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(tabview, lv_color_hex(UI_COLOR_SCREEN), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tabview, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tabview, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(tabview, 0, LV_PART_MAIN);
    lv_tabview_set_tab_bar_size(tabview, UI_TAB_BAR_HEIGHT);

    tab_bar = lv_tabview_get_tab_bar(tabview);
    lv_obj_set_style_bg_color(tab_bar, lv_color_hex(UI_COLOR_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(tab_bar, lv_color_hex(UI_COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(tab_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(tab_bar, 1, LV_PART_ITEMS);
    lv_obj_set_style_text_color(tab_bar, lv_color_hex(UI_COLOR_TEXT_MUTED), LV_PART_ITEMS);
    lv_obj_set_style_text_color(tab_bar, lv_color_hex(UI_COLOR_TEXT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(tab_bar, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(tab_bar, lv_color_hex(UI_COLOR_BUTTON), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_radius(tab_bar, 0, LV_PART_ITEMS);

    wave_tab = lv_tabview_add_tab(tabview, "Waveform");
    spectrum_tab = lv_tabview_add_tab(tabview, "Spectrum");
    control_tab = lv_tabview_add_tab(tabview, "Control");

    ui_create_wave_tab(wave_tab);
    ui_create_spectrum_tab(spectrum_tab);
    ui_create_control_tab(control_tab);
}

void ui_update_signal_state(const ui_signal_state_t * state)
{
    const char * waveform_text;

    if(state == NULL) {
        return;
    }

    waveform_text = (state->dds_waveform_text != NULL) ? state->dds_waveform_text : "--";

    if(g_wave_summary_label != NULL) {
        lv_label_set_text_fmt(g_wave_summary_label,
                              "Fs %lu Hz   DDS %lu Hz",
                              (unsigned long)state->sample_rate_hz,
                              (unsigned long)state->dds_freq_hz);
    }

    if(g_fft_summary_label != NULL) {
        uint32_t peak_hz = state->fft_peak_frequency_millihz / 1000U;
        uint32_t peak_tenth_hz =
            (state->fft_peak_frequency_millihz % 1000U) / 100U;

        lv_label_set_text_fmt(g_fft_summary_label,
                              "Peak %lu.%lu Hz   A %u",
                              (unsigned long)peak_hz,
                              (unsigned long)peak_tenth_hz,
                              (unsigned int)state->fft_peak_amplitude_codes);
    }

    if(g_fft_status_label != NULL) {
        lv_label_set_text_fmt(g_fft_status_label,
                              "RMS %u   Drop %lu   Err %lu/%lu",
                              (unsigned int)state->adc_rms_codes,
                              (unsigned long)state->adc_dropped_block_count,
                              (unsigned long)state->adc_error_count,
                              (unsigned long)state->dac_underrun_count);
    }

    if(g_sample_rate_value_label != NULL) {
        lv_label_set_text_fmt(g_sample_rate_value_label,
                              "%lu Hz",
                              (unsigned long)state->sample_rate_hz);
    }

    if(g_dds_freq_value_label != NULL) {
        lv_label_set_text_fmt(g_dds_freq_value_label,
                              "%lu Hz",
                              (unsigned long)state->dds_freq_hz);
    }

    if(g_dds_amplitude_value_label != NULL) {
        lv_label_set_text_fmt(g_dds_amplitude_value_label,
                              "%u%%",
                              (unsigned int)state->dds_amplitude_percent);
    }

    if(g_dds_waveform_value_label != NULL) {
        lv_label_set_text(g_dds_waveform_value_label, waveform_text);
    }

    ui_update_wave_view(&g_adc_view, state->adc_points, state->wave_point_count);
    ui_update_wave_view(&g_dds_view, state->dds_points, state->wave_point_count);
    ui_update_wave_view(&g_spectrum_view,
                        state->spectrum_points,
                        state->spectrum_point_count);
}
