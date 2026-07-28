#include "one_hmi_ad9910_interface.h"

#include <stddef.h>

#include "ad9910_signal_generator_app.h"
#include "tjc_ad9910_interface.h"
#include "usart_hmi_service.h"

#define ONE_HMI_VAR_COUNT 7U
#define ONE_HMI_MAX_AMPLITUDE_PERCENT 100U

typedef enum {
    ONE_HMI_VAR_WAVEFORM = 0,
    ONE_HMI_VAR_AMPLITUDE,
    ONE_HMI_VAR_PHASE,
    ONE_HMI_VAR_FREQUENCY,
    ONE_HMI_VAR_MODE,
    ONE_HMI_VAR_PROFILE,
    ONE_HMI_VAR_RUN_FLAG
} one_hmi_var_index_t;

typedef struct {
    const char *object_name;
    int32_t value;
} one_hmi_variable_t;

typedef struct {
    one_hmi_variable_t variables[ONE_HMI_VAR_COUNT];
    uint8_t pending_sync;
    uint8_t active_query;
    uint8_t query_index;
    one_hmi_ad9910_status_t status;
} one_hmi_context_t;

static one_hmi_context_t g_one_hmi;

static const char *const g_one_hmi_variable_names[ONE_HMI_VAR_COUNT] = {
    "page1.va0",
    "page1.va1",
    "page1.va2",
    "page1.va4",
    "page1.va8",
    "page1.va9",
    "page1.va10",
};

static uint8_t one_hmi_u8_clamp(int32_t value, uint8_t max_value)
{
    if (value <= 0) {
        return 0U;
    }
    if (value > (int32_t)max_value) {
        return max_value;
    }

    return (uint8_t)value;
}

static uint16_t one_hmi_phase_clamp(int32_t value)
{
    if (value <= 0) {
        return 0U;
    }
    if (value > (int32_t)AD9910_MAX_PHASE_DEGREES) {
        return AD9910_MAX_PHASE_DEGREES;
    }

    return (uint16_t)value;
}

static uint32_t one_hmi_frequency_clamp(int32_t value)
{
    if (value <= 0) {
        return 1U;
    }
    if ((uint32_t)value > AD9910_MAX_FREQUENCY_HZ) {
        return AD9910_MAX_FREQUENCY_HZ;
    }

    return (uint32_t)value;
}

static ad9910_siggen_waveform_t one_hmi_build_waveform(int32_t value)
{
    switch (value) {
    case ONE_HMI_WAVE_TRIANGLE:
        return AD9910_SIGGEN_WAVEFORM_TRIANGLE;

    case ONE_HMI_WAVE_SQUARE:
        return AD9910_SIGGEN_WAVEFORM_SQUARE;

    case ONE_HMI_WAVE_SAW:
        return AD9910_SIGGEN_WAVEFORM_SAW_RISE;

    case ONE_HMI_WAVE_SINE:
    default:
        return AD9910_SIGGEN_WAVEFORM_SINE;
    }
}

static HAL_StatusTypeDef one_hmi_dispatch_command(
    const ad9910_siggen_command_t *command)
{
    HAL_StatusTypeDef status = TJC_AD9910_Interface_Dispatch(command);

    g_one_hmi.status.last_hal_status = status;
    if (status != HAL_OK) {
        g_one_hmi.status.error = ONE_HMI_ERROR_AD9910_COMMAND;
        g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_ERROR;
    }

    return status;
}

static HAL_StatusTypeDef one_hmi_apply_single_tone(
    const ad9910_siggen_tone_param_t *tone)
{
    ad9910_siggen_command_t command;
    HAL_StatusTypeDef status;

    command = (ad9910_siggen_command_t){0};
    command.type = AD9910_SIGGEN_COMMAND_SET_SINGLE_TONE;
    command.tone = *tone;
    status = one_hmi_dispatch_command(&command);
    if (status != HAL_OK) {
        return status;
    }

    command = (ad9910_siggen_command_t){0};
    command.type = AD9910_SIGGEN_COMMAND_SET_MODE;
    command.mode = AD9910_SIGGEN_MODE_SINGLE_TONE;
    return one_hmi_dispatch_command(&command);
}

static HAL_StatusTypeDef one_hmi_apply_ram_waveform(
    const ad9910_siggen_tone_param_t *tone,
    ad9910_siggen_waveform_t waveform,
    uint8_t profile_index)
{
    ad9910_siggen_command_t command;
    HAL_StatusTypeDef status;

    command = (ad9910_siggen_command_t){0};
    command.type = AD9910_SIGGEN_COMMAND_SET_RAM_PRESET_TONE;
    command.ram_preset_index = profile_index;
    command.tone = *tone;
    status = one_hmi_dispatch_command(&command);
    if (status != HAL_OK) {
        return status;
    }

    command = (ad9910_siggen_command_t){0};
    command.type = AD9910_SIGGEN_COMMAND_SET_RAM_PRESET_WAVEFORM;
    command.ram_preset_index = profile_index;
    command.waveform = waveform;
    status = one_hmi_dispatch_command(&command);
    if (status != HAL_OK) {
        return status;
    }

    command = (ad9910_siggen_command_t){0};
    command.type = AD9910_SIGGEN_COMMAND_SELECT_RAM_PRESET;
    command.ram_preset_index = profile_index;
    status = one_hmi_dispatch_command(&command);
    if (status != HAL_OK) {
        return status;
    }

    command = (ad9910_siggen_command_t){0};
    command.type = AD9910_SIGGEN_COMMAND_SET_MODE;
    command.mode = AD9910_SIGGEN_MODE_RAM_WAVEFORM;
    return one_hmi_dispatch_command(&command);
}

static HAL_StatusTypeDef one_hmi_apply_variables(void)
{
    const int32_t waveform_value =
        g_one_hmi.variables[ONE_HMI_VAR_WAVEFORM].value;
    const int32_t mode_value = g_one_hmi.variables[ONE_HMI_VAR_MODE].value;
    const int32_t profile_value =
        g_one_hmi.variables[ONE_HMI_VAR_PROFILE].value;
    const ad9910_siggen_waveform_t waveform =
        one_hmi_build_waveform(waveform_value);
    const uint8_t profile_index =
        one_hmi_u8_clamp(profile_value,
                         (uint8_t)(AD9910_SIGGEN_RAM_PRESET_COUNT - 1U));
    ad9910_siggen_tone_param_t tone;
    HAL_StatusTypeDef status;

    tone.frequency_hz = one_hmi_frequency_clamp(
        g_one_hmi.variables[ONE_HMI_VAR_FREQUENCY].value);
    tone.amplitude_percent = one_hmi_u8_clamp(
        g_one_hmi.variables[ONE_HMI_VAR_AMPLITUDE].value,
        ONE_HMI_MAX_AMPLITUDE_PERCENT);
    tone.phase_degrees = one_hmi_phase_clamp(
        g_one_hmi.variables[ONE_HMI_VAR_PHASE].value);

    g_one_hmi.status.frequency_hz = tone.frequency_hz;
    g_one_hmi.status.amplitude_percent = tone.amplitude_percent;
    g_one_hmi.status.phase_degrees = tone.phase_degrees;
    g_one_hmi.status.waveform = (one_hmi_wave_t)one_hmi_u8_clamp(
        waveform_value,
        ONE_HMI_WAVE_SAW);
    g_one_hmi.status.mode = (one_hmi_mode_t)mode_value;
    g_one_hmi.status.profile_index = profile_index;
    g_one_hmi.status.run_flag =
        one_hmi_u8_clamp(g_one_hmi.variables[ONE_HMI_VAR_RUN_FLAG].value,
                         1U);

    if (mode_value == ONE_HMI_MODE_SWEEP) {
        g_one_hmi.status.error = ONE_HMI_ERROR_SWEEP_NOT_SUPPORTED;
        g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_ERROR;
        return HAL_ERROR;
    }

    if ((mode_value == ONE_HMI_MODE_RAM) ||
        (mode_value == ONE_HMI_MODE_PROFILE) ||
        (waveform != AD9910_SIGGEN_WAVEFORM_SINE)) {
        status = one_hmi_apply_ram_waveform(&tone, waveform, profile_index);
    } else {
        status = one_hmi_apply_single_tone(&tone);
    }

    if (status == HAL_OK) {
        g_one_hmi.status.error = ONE_HMI_ERROR_NONE;
        g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_READY;
        g_one_hmi.status.sync_count++;
    }

    return status;
}

static HAL_StatusTypeDef one_hmi_start_next_query(void)
{
    const char *object_name;
    HAL_StatusTypeDef status;

    if (g_one_hmi.query_index >= ONE_HMI_VAR_COUNT) {
        g_one_hmi.pending_sync = 0U;
        g_one_hmi.active_query = 0U;
        return one_hmi_apply_variables();
    }

    object_name = g_one_hmi.variables[g_one_hmi.query_index].object_name;
    status = Usart_HMI_Service_GetProperty(object_name, "val");
    g_one_hmi.status.last_hal_status = status;
    if (status == HAL_OK) {
        g_one_hmi.active_query = 1U;
        g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_QUERYING;
    } else if (status != HAL_BUSY) {
        g_one_hmi.status.error = ONE_HMI_ERROR_HMI_BUSY;
        g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_ERROR;
    }

    return status;
}

static void one_hmi_handle_number(uint32_t value)
{
    if ((g_one_hmi.active_query == 0U) ||
        (g_one_hmi.query_index >= ONE_HMI_VAR_COUNT)) {
        g_one_hmi.status.ignored_number_count++;
        return;
    }

    g_one_hmi.variables[g_one_hmi.query_index].value = (int32_t)value;
    g_one_hmi.query_index++;
    g_one_hmi.active_query = 0U;
}

static void one_hmi_handle_event(const usart_hmi_event_t *event)
{
    switch (event->type) {
    case USART_HMI_EVENT_TOUCH:
        if (event->data.touch.state == USART_HMI_TOUCH_RELEASE) {
            g_one_hmi.status.touch_count++;
            (void)One_HMI_AD9910_Interface_RequestSync();
        }
        break;

    case USART_HMI_EVENT_STARTUP:
        g_one_hmi.status.startup_count++;
        (void)One_HMI_AD9910_Interface_RequestSync();
        break;

    case USART_HMI_EVENT_NUMBER:
        one_hmi_handle_number(event->data.number.value);
        break;

    default:
        break;
    }
}

void One_HMI_AD9910_Interface_Init(void)
{
    for (uint8_t index = 0U; index < ONE_HMI_VAR_COUNT; ++index) {
        g_one_hmi.variables[index].object_name =
            g_one_hmi_variable_names[index];
        g_one_hmi.variables[index].value = 0;
    }

    g_one_hmi.pending_sync = 0U;
    g_one_hmi.active_query = 0U;
    g_one_hmi.query_index = 0U;
    g_one_hmi.status = (one_hmi_ad9910_status_t){0};
    g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_IDLE;
    g_one_hmi.status.error = ONE_HMI_ERROR_NONE;
    g_one_hmi.status.last_hal_status = HAL_OK;
}

void One_HMI_AD9910_Interface_Process(void)
{
    usart_hmi_event_t event;

    Usart_HMI_Service_Process();
    while (Usart_HMI_Service_ReadEvent(&event) != 0U) {
        one_hmi_handle_event(&event);
    }

    if ((g_one_hmi.pending_sync != 0U) &&
        (g_one_hmi.active_query == 0U)) {
        (void)one_hmi_start_next_query();
    }
}

HAL_StatusTypeDef One_HMI_AD9910_Interface_RequestSync(void)
{
    if (g_one_hmi.active_query != 0U) {
        g_one_hmi.pending_sync = 1U;
        return HAL_BUSY;
    }

    g_one_hmi.pending_sync = 1U;
    g_one_hmi.query_index = 0U;
    g_one_hmi.status.error = ONE_HMI_ERROR_NONE;
    g_one_hmi.status.sync_state = ONE_HMI_SYNC_STATE_QUERYING;
    return HAL_OK;
}

const one_hmi_ad9910_status_t *One_HMI_AD9910_Interface_GetStatus(void)
{
    return &g_one_hmi.status;
}
