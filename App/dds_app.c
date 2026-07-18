/**
 * @file dds_app.c
 * @brief 信号服务与 LVGL 界面的 Application 层实现
 */

#include "dds_app.h"

#include <stddef.h>

#include "signal_service.h"

#define DDS_APP_UI_REFRESH_PERIOD_MS 50U
#define DDS_APP_FREQ_STEP_HZ         10
#define DDS_APP_AMPLITUDE_STEP       10

static const uint32_t g_sample_rate_options[] = {
    SIGNAL_SERVICE_SAMPLE_RATE_20K_HZ,
    SIGNAL_SERVICE_SAMPLE_RATE_50K_HZ,
    SIGNAL_SERVICE_SAMPLE_RATE_100K_HZ,
    SIGNAL_SERVICE_SAMPLE_RATE_200K_HZ,
};

static int32_t g_adc_points[SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT];
static int32_t g_dds_points[SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT];
static int32_t g_spectrum_points[SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT];
static uint32_t g_adc_generation;
static uint32_t g_dds_generation;
static uint32_t g_spectrum_generation;
static uint32_t g_last_ui_refresh_tick;
static uint8_t g_sample_rate_index;
static bool g_ui_dirty;

static int8_t g_pending_sample_rate_step;
static int32_t g_pending_frequency_delta;
static int16_t g_pending_amplitude_delta;
static uint8_t g_pending_waveform_next;

static const char *dds_app_waveform_text(dds_waveform_t waveform)
{
    switch(waveform) {
        case DDS_WAVE_SINE:
            return "Sine";
        case DDS_WAVE_COMPOSITE:
            return "Composite";
        case DDS_WAVE_COUNT:
        default:
            return "Unknown";
    }
}

static uint8_t dds_app_find_sample_rate_index(uint32_t sample_rate_hz)
{
    uint8_t index;
    uint8_t nearest_index = 0U;
    uint32_t nearest_difference = UINT32_MAX;

    for(index = 0U;
        index < (uint8_t)(sizeof(g_sample_rate_options) /
                          sizeof(g_sample_rate_options[0]));
        index++) {
        uint32_t option = g_sample_rate_options[index];
        uint32_t difference = (option > sample_rate_hz) ?
                              (option - sample_rate_hz) :
                              (sample_rate_hz - option);

        if(difference < nearest_difference) {
            nearest_difference = difference;
            nearest_index = index;
        }
    }
    return nearest_index;
}

static void dds_app_queue_sample_rate_step(int8_t step)
{
    if(step < 0) {
        g_pending_sample_rate_step = -1;
    }
    else if(step > 0) {
        g_pending_sample_rate_step = 1;
    }
}

static void dds_app_apply_requests(void)
{
    SignalService_State state;
    int8_t sample_rate_step = g_pending_sample_rate_step;
    int32_t frequency_delta = g_pending_frequency_delta;
    int16_t amplitude_delta = g_pending_amplitude_delta;
    uint8_t waveform_next = g_pending_waveform_next;

    g_pending_sample_rate_step = 0;
    g_pending_frequency_delta = 0;
    g_pending_amplitude_delta = 0;
    g_pending_waveform_next = 0U;

    if(SignalService_GetState(&state) != HAL_OK) {
        return;
    }

    if(sample_rate_step != 0) {
        int32_t next_index = (int32_t)g_sample_rate_index +
                             (int32_t)sample_rate_step;
        int32_t max_index =
            (int32_t)(sizeof(g_sample_rate_options) /
                      sizeof(g_sample_rate_options[0])) - 1;

        if(next_index < 0) {
            next_index = 0;
        }
        if(next_index > max_index) {
            next_index = max_index;
        }
        if((uint8_t)next_index != g_sample_rate_index) {
            uint32_t actual_rate;

            if(SignalService_SetSampleRate(
                   g_sample_rate_options[next_index],
                   &actual_rate) == HAL_OK) {
                g_sample_rate_index = (uint8_t)next_index;
                state.sample_rate_hz = actual_rate;
                g_ui_dirty = true;
            }
        }
    }

    if(frequency_delta != 0) {
        int64_t requested = (int64_t)state.dds_frequency_hz +
                            frequency_delta;

        if(requested < 0) {
            requested = 0;
        }
        if(SignalService_SetDdsFrequency((uint32_t)requested,
                                         NULL) == HAL_OK) {
            g_ui_dirty = true;
        }
    }

    if(amplitude_delta != 0) {
        int32_t requested = (int32_t)state.dds_amplitude_percent +
                            amplitude_delta;

        if(requested < 0) {
            requested = 0;
        }
        if(requested > UINT8_MAX) {
            requested = UINT8_MAX;
        }
        if(SignalService_SetDdsAmplitude((uint8_t)requested,
                                         NULL) == HAL_OK) {
            g_ui_dirty = true;
        }
    }

    if(waveform_next != 0U) {
        dds_waveform_t waveform =
            (dds_waveform_t)(((uint32_t)state.dds_waveform + 1U) %
                             (uint32_t)DDS_WAVE_COUNT);

        if(SignalService_SetDdsWaveform(waveform) == HAL_OK) {
            g_ui_dirty = true;
        }
    }
}

static void dds_app_copy_new_snapshots(void)
{
    SignalService_State state;

    if(SignalService_GetState(&state) != HAL_OK) {
        return;
    }

    if(state.adc_snapshot_valid &&
       (state.adc_snapshot_generation != g_adc_generation) &&
       SignalService_CopyAdcSnapshot(g_adc_points,
                                     SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT,
                                     &g_adc_generation)) {
        g_ui_dirty = true;
    }

    if(state.dds_snapshot_valid &&
       (state.dds_snapshot_generation != g_dds_generation) &&
       SignalService_CopyDdsSnapshot(g_dds_points,
                                     SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT,
                                     &g_dds_generation)) {
        g_ui_dirty = true;
    }

    if(state.spectrum_snapshot_valid &&
       (state.spectrum_snapshot_generation != g_spectrum_generation) &&
       SignalService_CopySpectrumSnapshot(
           g_spectrum_points,
           SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT,
           &g_spectrum_generation)) {
        g_ui_dirty = true;
    }
}

HAL_StatusTypeDef dds_app_init(TIM_HandleTypeDef *adc_sample_timer,
                               TIM_HandleTypeDef *dds_sample_timer)
{
    SignalService_State state;
    uint32_t index;

    for(index = 0U; index < SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT; index++) {
        g_adc_points[index] = 0;
        g_dds_points[index] = 0;
        g_spectrum_points[index] = 0;
    }

    g_adc_generation = 0U;
    g_dds_generation = 0U;
    g_spectrum_generation = 0U;
    g_last_ui_refresh_tick = HAL_GetTick();
    g_pending_sample_rate_step = 0;
    g_pending_frequency_delta = 0;
    g_pending_amplitude_delta = 0;
    g_pending_waveform_next = 0U;
    g_ui_dirty = true;

    if(SignalService_Init(adc_sample_timer, dds_sample_timer) != HAL_OK) {
        return HAL_ERROR;
    }
    if(SignalService_GetState(&state) != HAL_OK) {
        return HAL_ERROR;
    }

    g_sample_rate_index =
        dds_app_find_sample_rate_index(state.sample_rate_hz);
    return HAL_OK;
}

void dds_app_request_sample_rate_down(void)
{
    dds_app_queue_sample_rate_step(-1);
}

void dds_app_request_sample_rate_up(void)
{
    dds_app_queue_sample_rate_step(1);
}

void dds_app_request_freq_down(void)
{
    g_pending_frequency_delta -= DDS_APP_FREQ_STEP_HZ;
}

void dds_app_request_freq_up(void)
{
    g_pending_frequency_delta += DDS_APP_FREQ_STEP_HZ;
}

void dds_app_request_amplitude_down(void)
{
    g_pending_amplitude_delta -= DDS_APP_AMPLITUDE_STEP;
}

void dds_app_request_amplitude_up(void)
{
    g_pending_amplitude_delta += DDS_APP_AMPLITUDE_STEP;
}

void dds_app_request_next_waveform(void)
{
    g_pending_waveform_next = 1U;
}

bool dds_app_process(void)
{
    uint32_t now;

    dds_app_apply_requests();
    (void)SignalService_Process();
    dds_app_copy_new_snapshots();

    now = HAL_GetTick();
    if(g_ui_dirty &&
       ((now - g_last_ui_refresh_tick) >= DDS_APP_UI_REFRESH_PERIOD_MS)) {
        g_last_ui_refresh_tick = now;
        g_ui_dirty = false;
        return true;
    }
    return false;
}

void dds_app_fill_ui_state(ui_signal_state_t *state)
{
    SignalService_State service_state;

    if((state == NULL) ||
       (SignalService_GetState(&service_state) != HAL_OK)) {
        return;
    }

    state->sample_rate_hz = service_state.sample_rate_hz;
    state->dds_freq_hz = service_state.dds_frequency_hz;
    state->dds_amplitude_percent =
        service_state.dds_amplitude_percent;
    state->dds_waveform_text =
        dds_app_waveform_text(service_state.dds_waveform);
    state->fft_peak_frequency_millihz =
        service_state.fft_peak_frequency_millihz;
    state->fft_resolution_millihz =
        service_state.fft_resolution_millihz;
    state->fft_peak_amplitude_codes =
        service_state.fft_peak_amplitude_codes;
    state->adc_rms_codes = service_state.adc_rms_codes;
    state->adc_dropped_block_count =
        service_state.adc_dropped_block_count;
    state->adc_error_count = service_state.adc_error_count;
    state->dac_underrun_count = service_state.dac_underrun_count;
    state->adc_points = g_adc_points;
    state->dds_points = g_dds_points;
    state->spectrum_points = g_spectrum_points;
    state->wave_point_count = SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT;
    state->spectrum_point_count = SIGNAL_SERVICE_SNAPSHOT_POINT_COUNT;
}
