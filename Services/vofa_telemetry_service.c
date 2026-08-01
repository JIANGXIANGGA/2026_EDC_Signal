#include "vofa_telemetry_service.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>

#include "signal_acquisition_service.h"
#include "signal_measurement_service.h"
#include "vofa_uart_driver.h"
#include "waveform_analyzer_service.h"

#define VOFA_TELEMETRY_PUBLISH_INTERVAL_MS 100U
#define VOFA_TELEMETRY_TX_BUFFER_SIZE 1024U

typedef struct {
    uint32_t next_publish_ms;
    char tx_buffer[VOFA_TELEMETRY_TX_BUFFER_SIZE];
    vofa_telemetry_status_t status;
} vofa_telemetry_context_t;

static vofa_telemetry_context_t g_vofa_telemetry;

static uint8_t vofa_telemetry_time_reached(uint32_t deadline_ms)
{
    return ((int32_t)(HAL_GetTick() - deadline_ms) >= 0) ? 1U : 0U;
}

static uint32_t vofa_telemetry_scale_nonnegative(float value, float scale)
{
    const float scaled = value * scale;

    if ((isfinite(scaled) == 0) || (scaled <= 0.0f)) {
        return 0U;
    }
    if (scaled >= (float)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)(scaled + 0.5f);
}

static int32_t vofa_telemetry_scale_signed(float value, float scale)
{
    const float scaled = value * scale;

    if (isfinite(scaled) == 0) {
        return 0;
    }
    if (scaled >= (float)INT32_MAX) {
        return INT32_MAX;
    }
    if (scaled <= (float)INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)((scaled >= 0.0f) ?
                         (scaled + 0.5f) :
                         (scaled - 0.5f));
}

static uint32_t vofa_telemetry_component_frequency(
    const signal_measurement_result_t *measurement,
    uint8_t index)
{
    if ((measurement == NULL) ||
        (index >= measurement->component_count) ||
        (measurement->components[index].valid == 0U)) {
        return 0U;
    }
    return vofa_telemetry_scale_nonnegative(
        measurement->components[index].frequency_hz, 1.0f);
}

static uint32_t vofa_telemetry_component_amplitude_mv_x10(
    const signal_measurement_result_t *measurement,
    uint8_t index)
{
    if ((measurement == NULL) ||
        (index >= measurement->component_count) ||
        (measurement->components[index].valid == 0U)) {
        return 0U;
    }
    return vofa_telemetry_scale_nonnegative(
        measurement->components[index].amplitude_mv, 10.0f);
}

static uint8_t vofa_telemetry_component_order(
    const signal_measurement_result_t *measurement,
    uint8_t index)
{
    if ((measurement == NULL) ||
        (index >= measurement->component_count) ||
        (measurement->components[index].valid == 0U)) {
        return 0U;
    }
    return measurement->components[index].harmonic_order;
}

static uint32_t vofa_telemetry_peak_frequency(
    const waveform_analyzer_result_t *analysis,
    uint8_t index)
{
    if ((analysis == NULL) || (index >= analysis->peak_count)) {
        return 0U;
    }
    return vofa_telemetry_scale_nonnegative(
        analysis->peak_frequencies_hz[index], 1.0f);
}

static uint32_t vofa_telemetry_peak_amplitude_code_x100(
    const waveform_analyzer_result_t *analysis,
    uint8_t index)
{
    if ((analysis == NULL) || (index >= analysis->peak_count)) {
        return 0U;
    }
    return vofa_telemetry_scale_nonnegative(
        analysis->peak_amplitudes_code[index], 100.0f);
}

static int32_t vofa_telemetry_peak_phase_mrad(
    const waveform_analyzer_result_t *analysis,
    uint8_t index)
{
    if ((analysis == NULL) || (index >= analysis->peak_count)) {
        return 0;
    }
    return vofa_telemetry_scale_signed(
        analysis->peak_phases_rad[index], 1000.0f);
}

HAL_StatusTypeDef VOFA_Telemetry_Service_Init(void)
{
    HAL_StatusTypeDef status;

    g_vofa_telemetry = (vofa_telemetry_context_t){0};
    status = VOFA_UART_Driver_Init();
    if (status != HAL_OK) {
        g_vofa_telemetry.status.last_hal_status = status;
        return status;
    }

    g_vofa_telemetry.next_publish_ms =
        HAL_GetTick() + VOFA_TELEMETRY_PUBLISH_INTERVAL_MS;
    g_vofa_telemetry.status.initialized = 1U;
    g_vofa_telemetry.status.last_hal_status = HAL_OK;
    return HAL_OK;
}

HAL_StatusTypeDef VOFA_Telemetry_Service_Process(void)
{
    const waveform_analyzer_result_t *analysis;
    const signal_measurement_result_t *measurement;
    const signal_acquisition_status_t *acquisition;
    uint32_t upp_x10;
    uint32_t u_x10;
    uint32_t u1_x10;
    uint32_t u2_x10;
    uint32_t u3_x10;
    uint32_t now_ms;
    HAL_StatusTypeDef status;
    int written;

    if (g_vofa_telemetry.status.initialized == 0U) {
        return HAL_ERROR;
    }

    status = VOFA_UART_Driver_Process();
    if (status != HAL_OK) {
        g_vofa_telemetry.status.last_hal_status = status;
        g_vofa_telemetry.status.transmit_error_count++;
        return status;
    }

    now_ms = HAL_GetTick();
    if (vofa_telemetry_time_reached(
            g_vofa_telemetry.next_publish_ms) == 0U) {
        return HAL_OK;
    }
    if (VOFA_UART_Driver_IsBusy() != 0U) {
        return HAL_BUSY;
    }

    analysis = Waveform_Analyzer_GetResult();
    measurement = Signal_Measurement_Service_GetResult();
    acquisition = Signal_Acquisition_Service_GetStatus();
    if ((analysis == NULL) || (measurement == NULL) ||
        (acquisition == NULL) ||
        (analysis->result_ready == 0U) ||
        (measurement->result_ready == 0U) ||
        (measurement->measurement_count ==
         g_vofa_telemetry.status.last_measurement_count)) {
        g_vofa_telemetry.next_publish_ms =
            now_ms + VOFA_TELEMETRY_PUBLISH_INTERVAL_MS;
        return HAL_OK;
    }

    upp_x10 = vofa_telemetry_scale_nonnegative(
        measurement->peak_to_peak_mv, 10.0f);
    u_x10 = vofa_telemetry_scale_nonnegative(
        measurement->true_rms_mv, 10.0f);
    u1_x10 = vofa_telemetry_component_amplitude_mv_x10(
        measurement, 0U);
    u2_x10 = vofa_telemetry_component_amplitude_mv_x10(
        measurement, 1U);
    u3_x10 = vofa_telemetry_component_amplitude_mv_x10(
        measurement, 2U);

    written = snprintf(
        g_vofa_telemetry.tx_buffer,
        sizeof(g_vofa_telemetry.tx_buffer),
        "Upp_mV:%lu.%01lu,U_mV:%lu.%01lu,f_base_Hz:%lu,"
        "component_count:%u,signal_valid:%u,"
        "f1_Hz:%lu,U1_peak_mV:%lu.%01lu,n1:%u,"
        "f2_Hz:%lu,U2_peak_mV:%lu.%01lu,n2:%u,"
        "f3_Hz:%lu,U3_peak_mV:%lu.%01lu,n3:%u\r\n"
        "seq:%lu,adc_min:%u,adc_max:%u,p2p_code:%u,"
        "rms_code_x100:%lu,analysis_ready:%u,measurement_ready:%u,"
        "peak_count:%u,raw_f1_hz:%lu,raw_a1_code_x100:%lu,"
        "raw_f2_hz:%lu,raw_a2_code_x100:%lu,"
        "raw_f3_hz:%lu,raw_a3_code_x100:%lu,"
        "raw_f4_hz:%lu,raw_a4_code_x100:%lu,"
        "raw_f5_hz:%lu,raw_a5_code_x100:%lu,"
        "raw_f6_hz:%lu,raw_a6_code_x100:%lu,"
        "raw_p1_mrad:%ld,raw_p2_mrad:%ld,raw_p3_mrad:%ld,"
        "raw_p4_mrad:%ld,raw_p5_mrad:%ld,raw_p6_mrad:%ld,"
        "upp_mv_x10:%lu,urms_mv_x10:%lu,comp_count:%u,avg_count:%u,"
        "f1_hz:%lu,a1_mv_x10:%lu,f2_hz:%lu,a2_mv_x10:%lu,"
        "f3_hz:%lu,a3_mv_x10:%lu,p2p_spread_mv_x10:%lu,"
        "amp_spread_mv_x10:%lu,analysis_us:%lu,analysis_max_us:%lu,"
        "adc_overrun:%lu\r\n",
        (unsigned long)(upp_x10 / 10U),
        (unsigned long)(upp_x10 % 10U),
        (unsigned long)(u_x10 / 10U),
        (unsigned long)(u_x10 % 10U),
        (unsigned long)vofa_telemetry_scale_nonnegative(
            measurement->fundamental_frequency_hz, 1.0f),
        (unsigned int)measurement->component_count,
        (unsigned int)measurement->signal_valid,
        (unsigned long)vofa_telemetry_component_frequency(measurement, 0U),
        (unsigned long)(u1_x10 / 10U),
        (unsigned long)(u1_x10 % 10U),
        (unsigned int)vofa_telemetry_component_order(measurement, 0U),
        (unsigned long)vofa_telemetry_component_frequency(measurement, 1U),
        (unsigned long)(u2_x10 / 10U),
        (unsigned long)(u2_x10 % 10U),
        (unsigned int)vofa_telemetry_component_order(measurement, 1U),
        (unsigned long)vofa_telemetry_component_frequency(measurement, 2U),
        (unsigned long)(u3_x10 / 10U),
        (unsigned long)(u3_x10 % 10U),
        (unsigned int)vofa_telemetry_component_order(measurement, 2U),
        (unsigned long)measurement->measurement_count,
        (unsigned int)analysis->min_code,
        (unsigned int)analysis->max_code,
        (unsigned int)analysis->peak_to_peak_code,
        (unsigned long)vofa_telemetry_scale_nonnegative(
            analysis->rms_code, 100.0f),
        (unsigned int)analysis->result_ready,
        (unsigned int)measurement->result_ready,
        (unsigned int)analysis->peak_count,
        (unsigned long)vofa_telemetry_peak_frequency(analysis, 0U),
        (unsigned long)vofa_telemetry_peak_amplitude_code_x100(
            analysis, 0U),
        (unsigned long)vofa_telemetry_peak_frequency(analysis, 1U),
        (unsigned long)vofa_telemetry_peak_amplitude_code_x100(
            analysis, 1U),
        (unsigned long)vofa_telemetry_peak_frequency(analysis, 2U),
        (unsigned long)vofa_telemetry_peak_amplitude_code_x100(
            analysis, 2U),
        (unsigned long)vofa_telemetry_peak_frequency(analysis, 3U),
        (unsigned long)vofa_telemetry_peak_amplitude_code_x100(
            analysis, 3U),
        (unsigned long)vofa_telemetry_peak_frequency(analysis, 4U),
        (unsigned long)vofa_telemetry_peak_amplitude_code_x100(
            analysis, 4U),
        (unsigned long)vofa_telemetry_peak_frequency(analysis, 5U),
        (unsigned long)vofa_telemetry_peak_amplitude_code_x100(
            analysis, 5U),
        (long)vofa_telemetry_peak_phase_mrad(analysis, 0U),
        (long)vofa_telemetry_peak_phase_mrad(analysis, 1U),
        (long)vofa_telemetry_peak_phase_mrad(analysis, 2U),
        (long)vofa_telemetry_peak_phase_mrad(analysis, 3U),
        (long)vofa_telemetry_peak_phase_mrad(analysis, 4U),
        (long)vofa_telemetry_peak_phase_mrad(analysis, 5U),
        (unsigned long)upp_x10,
        (unsigned long)u_x10,
        (unsigned int)measurement->component_count,
        (unsigned int)measurement->averaging_count,
        (unsigned long)vofa_telemetry_component_frequency(measurement, 0U),
        (unsigned long)u1_x10,
        (unsigned long)vofa_telemetry_component_frequency(measurement, 1U),
        (unsigned long)u2_x10,
        (unsigned long)vofa_telemetry_component_frequency(measurement, 2U),
        (unsigned long)u3_x10,
        (unsigned long)vofa_telemetry_scale_nonnegative(
            measurement->peak_to_peak_spread_mv, 10.0f),
        (unsigned long)vofa_telemetry_scale_nonnegative(
            measurement->max_component_spread_mv, 10.0f),
        (unsigned long)acquisition->last_analysis_time_us,
        (unsigned long)acquisition->max_analysis_time_us,
        (unsigned long)acquisition->adc_overrun_count);
    if ((written <= 0) ||
        ((size_t)written >= sizeof(g_vofa_telemetry.tx_buffer))) {
        g_vofa_telemetry.status.format_error_count++;
        g_vofa_telemetry.status.last_hal_status = HAL_ERROR;
        g_vofa_telemetry.next_publish_ms =
            now_ms + VOFA_TELEMETRY_PUBLISH_INTERVAL_MS;
        return HAL_ERROR;
    }

    status = VOFA_UART_Driver_Transmit(
        (const uint8_t *)g_vofa_telemetry.tx_buffer,
        (uint16_t)written);
    if (status != HAL_OK) {
        if (status != HAL_BUSY) {
            g_vofa_telemetry.status.transmit_error_count++;
        }
        g_vofa_telemetry.status.last_hal_status = status;
        return status;
    }

    g_vofa_telemetry.status.publish_count++;
    g_vofa_telemetry.status.last_measurement_count =
        measurement->measurement_count;
    g_vofa_telemetry.status.last_hal_status = HAL_OK;
    g_vofa_telemetry.next_publish_ms =
        now_ms + VOFA_TELEMETRY_PUBLISH_INTERVAL_MS;
    return HAL_OK;
}

const vofa_telemetry_status_t *VOFA_Telemetry_Service_GetStatus(void)
{
    return &g_vofa_telemetry.status;
}
