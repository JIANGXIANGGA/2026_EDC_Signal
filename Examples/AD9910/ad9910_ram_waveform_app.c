#include "ad9910_ram_waveform_app.h"

#include <stddef.h>

#include "ad9910.h"
#include "ad9910_ram_waveform_presets.h"

ad9910_ram_waveform_app_status_t g_ad9910_ram_waveform_status;

static uint32_t g_ad9910_ram_words[AD9910_RAM_WAVEFORM_APP_MAX_SAMPLES];
static ad9910_ram_playback_config_t g_ad9910_ram_config;
static const ad9910_ram_waveform_preset_t *g_ad9910_ram_preset;

static uint8_t ad9910_ram_waveform_build_amplitude_words(
    const ad9910_ram_waveform_preset_t *preset)
{
    if ((preset == NULL) || (preset->amplitude_samples == NULL) ||
        (preset->sample_count == 0U) ||
        (preset->sample_count > AD9910_RAM_WAVEFORM_APP_MAX_SAMPLES)) {
        return 0U;
    }

    for (uint16_t index = 0U; index < preset->sample_count; ++index) {
        g_ad9910_ram_words[index] =
            AD9910_BuildRamAmplitudeWord(preset->amplitude_samples[index]);
    }

    return 1U;
}

static void ad9910_ram_waveform_update_status(void)
{
    g_ad9910_ram_waveform_status.service_state = AD9910_Service_GetState();
    g_ad9910_ram_waveform_status.ram_active =
        AD9910_Service_IsRamPlaybackActive();
    g_ad9910_ram_waveform_status.destination =
        AD9910_Service_GetRamDestination();
    g_ad9910_ram_waveform_status.mode = AD9910_Service_GetRamMode();
    g_ad9910_ram_waveform_status.sample_count =
        AD9910_Service_GetRamSampleCount();
    g_ad9910_ram_waveform_status.address_step_rate =
        AD9910_Service_GetRamAddressStepRate();
    g_ad9910_ram_waveform_status.last_hal_error =
        AD9910_Service_GetLastHalError();
}

HAL_StatusTypeDef AD9910_Ram_Waveform_App_Init(SPI_HandleTypeDef *spi)
{
    HAL_StatusTypeDef status;

    g_ad9910_ram_preset = AD9910_Ram_Waveform_GetBootPreset();
    if ((g_ad9910_ram_preset == NULL) ||
        (ad9910_ram_waveform_build_amplitude_words(g_ad9910_ram_preset) == 0U)) {
        g_ad9910_ram_waveform_status.app_error =
            AD9910_RAM_WAVEFORM_APP_ERROR_INVALID_PRESET;
        g_ad9910_ram_waveform_status.app_state =
            AD9910_RAM_WAVEFORM_APP_STATE_ERROR;
        return HAL_ERROR;
    }

    status = AD9910_Service_Init(spi);
    if (status != HAL_OK) {
        g_ad9910_ram_waveform_status.app_error =
            AD9910_RAM_WAVEFORM_APP_ERROR_SERVICE_COMMAND;
        g_ad9910_ram_waveform_status.app_state =
            AD9910_RAM_WAVEFORM_APP_STATE_ERROR;
        return status;
    }

    g_ad9910_ram_config.profile_index = g_ad9910_ram_preset->profile_index;
    g_ad9910_ram_config.start_address = 0U;
    g_ad9910_ram_config.sample_count = g_ad9910_ram_preset->sample_count;
    g_ad9910_ram_config.address_step_rate = 0U;
    g_ad9910_ram_config.playback_sample_rate_hz =
        g_ad9910_ram_preset->playback_sample_rate_hz;
    g_ad9910_ram_config.destination = g_ad9910_ram_preset->destination;
    g_ad9910_ram_config.mode = g_ad9910_ram_preset->mode;
    g_ad9910_ram_config.no_dwell_high = g_ad9910_ram_preset->no_dwell_high;
    g_ad9910_ram_config.zero_crossing = g_ad9910_ram_preset->zero_crossing;
    g_ad9910_ram_config.base_tone.frequency_hz =
        g_ad9910_ram_preset->carrier_frequency_hz;
    g_ad9910_ram_config.base_tone.amplitude =
        AD9910_AmplitudePercentToASF(
            g_ad9910_ram_preset->carrier_amplitude_percent);
    g_ad9910_ram_config.base_tone.phase_offset =
        AD9910_PhaseDegreesToPOW(g_ad9910_ram_preset->carrier_phase_degrees);
    g_ad9910_ram_config.ram_words = g_ad9910_ram_words;
    g_ad9910_ram_config.ram_word_count = g_ad9910_ram_preset->sample_count;

    g_ad9910_ram_waveform_status.app_error =
        AD9910_RAM_WAVEFORM_APP_ERROR_NONE;
    g_ad9910_ram_waveform_status.app_state =
        AD9910_RAM_WAVEFORM_APP_STATE_WAIT_SERVICE;
    g_ad9910_ram_waveform_status.carrier_frequency_hz =
        g_ad9910_ram_preset->carrier_frequency_hz;
    g_ad9910_ram_waveform_status.carrier_amplitude_percent =
        g_ad9910_ram_preset->carrier_amplitude_percent;
    g_ad9910_ram_waveform_status.playback_sample_rate_hz =
        g_ad9910_ram_preset->playback_sample_rate_hz;
    ad9910_ram_waveform_update_status();

    return HAL_OK;
}

void AD9910_Ram_Waveform_App_Process(void)
{
    AD9910_Service_Process();

    if (AD9910_Service_GetState() == AD9910_SERVICE_STATE_ERROR) {
        g_ad9910_ram_waveform_status.app_error =
            AD9910_RAM_WAVEFORM_APP_ERROR_SERVICE_STATE;
        g_ad9910_ram_waveform_status.app_state =
            AD9910_RAM_WAVEFORM_APP_STATE_ERROR;
    }

    switch (g_ad9910_ram_waveform_status.app_state) {
    case AD9910_RAM_WAVEFORM_APP_STATE_WAIT_SERVICE:
        if (AD9910_Service_GetState() == AD9910_SERVICE_STATE_READY) {
            g_ad9910_ram_waveform_status.app_state =
                AD9910_RAM_WAVEFORM_APP_STATE_REQUEST_START;
        }
        break;

    case AD9910_RAM_WAVEFORM_APP_STATE_REQUEST_START:
        if (AD9910_Service_StartRamPlayback(&g_ad9910_ram_config) != HAL_OK) {
            g_ad9910_ram_waveform_status.app_error =
                AD9910_RAM_WAVEFORM_APP_ERROR_SERVICE_COMMAND;
            g_ad9910_ram_waveform_status.app_state =
                AD9910_RAM_WAVEFORM_APP_STATE_ERROR;
            break;
        }
        g_ad9910_ram_waveform_status.app_state =
            AD9910_RAM_WAVEFORM_APP_STATE_WAIT_PLAYBACK;
        break;

    case AD9910_RAM_WAVEFORM_APP_STATE_WAIT_PLAYBACK:
        if ((AD9910_Service_GetState() == AD9910_SERVICE_STATE_READY) &&
            (AD9910_Service_IsRamPlaybackActive() != 0U)) {
            g_ad9910_ram_waveform_status.app_state =
                AD9910_RAM_WAVEFORM_APP_STATE_RUNNING;
        }
        break;

    case AD9910_RAM_WAVEFORM_APP_STATE_RUNNING:
    case AD9910_RAM_WAVEFORM_APP_STATE_ERROR:
    default:
        break;
    }

    ad9910_ram_waveform_update_status();
}
