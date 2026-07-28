#include "ad9910_signal_generator_app.h"

#include <stddef.h>

#include "ad9910.h"

#define AD9910_SIGGEN_DEFAULT_DUTY_PERCENT 50U
#define AD9910_SIGGEN_DEFAULT_HARMONIC2_PERCENT 35U
#define AD9910_SIGGEN_DEFAULT_HARMONIC3_PERCENT 20U
#define AD9910_SIGGEN_POLAR_POSITIVE_PHASE_DEGREES 90U
#define AD9910_SIGGEN_POLAR_NEGATIVE_PHASE_DEGREES 270U
#define AD9910_SIGGEN_SIGNED_SAMPLE_MAX ((int32_t)AD9910_MAX_AMPLITUDE)

typedef struct {
    /* 单音模式只维护一组实时参数，不做 8 组上电预编程。 */
    ad9910_siggen_tone_param_t single_tone;
    ad9910_siggen_tone_param_t active_single_tone;
    ad9910_siggen_tone_param_t service_cycle_single_tone;
    /* RAM 模式面向 TJC 页面保留 8 个可编辑波形预设槽。 */
    ad9910_siggen_ram_preset_t ram_presets[AD9910_SIGGEN_RAM_PRESET_COUNT];
    ad9910_siggen_ram_preset_t active_ram_preset_config;
    ad9910_siggen_ram_preset_t service_cycle_ram_preset_config;
    uint32_t ram_words[AD9910_SIGGEN_RAM_SAMPLE_COUNT];
    ad9910_ram_playback_config_t ram_config;
    ad9910_siggen_state_t state;
    ad9910_siggen_error_t error;
    ad9910_siggen_mode_t active_mode;
    ad9910_siggen_mode_t requested_mode;
    uint8_t active_ram_preset;
    uint8_t requested_ram_preset;
    uint8_t service_cycle_ram_preset;
    uint8_t pending_apply;
    uint8_t pending_single_tone_update;
    uint8_t service_cycle_started;
    uint32_t ram_playback_sample_rate_hz;
    uint32_t active_ram_playback_sample_rate_hz;
    uint32_t service_cycle_ram_playback_sample_rate_hz;
    uint32_t apply_generation;
    uint32_t service_cycle_apply_generation;
    uint32_t single_tone_generation;
    uint32_t service_cycle_single_tone_generation;
} ad9910_siggen_context_t;

static ad9910_siggen_context_t g_ad9910_siggen;
static ad9910_siggen_status_t g_ad9910_siggen_status;

static const int16_t g_ad9910_siggen_sine64[AD9910_SIGGEN_RAM_SAMPLE_COUNT] = {
    0, 1606, 3196, 4756, 6270, 7723, 9102, 10394,
    11585, 12665, 13623, 14449, 15136, 15679, 16069, 16305,
    16383, 16305, 16069, 15679, 15136, 14449, 13623, 12665,
    11585, 10394, 9102, 7723, 6270, 4756, 3196, 1606,
    0, -1606, -3196, -4756, -6270, -7723, -9102, -10394,
    -11585, -12665, -13623, -14449, -15136, -15679, -16069, -16305,
    -16383, -16305, -16069, -15679, -15136, -14449, -13623, -12665,
    -11585, -10394, -9102, -7723, -6270, -4756, -3196, -1606
};

static uint8_t ad9910_siggen_ram_preset_index_is_valid(uint8_t preset_index)
{
    return (preset_index < AD9910_SIGGEN_RAM_PRESET_COUNT) ? 1U : 0U;
}

static uint8_t ad9910_siggen_mode_is_valid(ad9910_siggen_mode_t mode)
{
    switch (mode) {
    case AD9910_SIGGEN_MODE_SINGLE_TONE:
        return (AD9910_SIGGEN_ENABLE_SINGLE_TONE != 0U) ? 1U : 0U;

    case AD9910_SIGGEN_MODE_RAM_WAVEFORM:
        return (AD9910_SIGGEN_ENABLE_RAM_PLAYBACK != 0U) ? 1U : 0U;

    default:
        return 0U;
    }
}

static uint8_t ad9910_siggen_waveform_is_valid(ad9910_siggen_waveform_t waveform)
{
    return (waveform < AD9910_SIGGEN_WAVEFORM_COUNT) ? 1U : 0U;
}

static uint8_t ad9910_siggen_tone_is_valid(
    const ad9910_siggen_tone_param_t *tone)
{
    return (tone != NULL) &&
           (tone->frequency_hz <= AD9910_MAX_FREQUENCY_HZ) &&
           (tone->amplitude_percent <= 100U) &&
           (tone->phase_degrees <= AD9910_MAX_PHASE_DEGREES);
}

static uint8_t ad9910_siggen_ram_tone_is_valid(
    const ad9910_siggen_tone_param_t *tone)
{
    return (ad9910_siggen_tone_is_valid(tone) != 0U) &&
           (tone->frequency_hz != 0U) &&
           (tone->frequency_hz <= AD9910_SIGGEN_RAM_MAX_WAVE_FREQUENCY_HZ);
}

static void ad9910_siggen_set_error(ad9910_siggen_error_t error)
{
    g_ad9910_siggen.error = error;
    g_ad9910_siggen.state = AD9910_SIGGEN_STATE_ERROR;
}

static void ad9910_siggen_request_apply(void)
{
    g_ad9910_siggen.apply_generation++;
    g_ad9910_siggen.pending_apply = 1U;
}

static void ad9910_siggen_complete_apply_request(void)
{
    g_ad9910_siggen.pending_apply =
        (g_ad9910_siggen.service_cycle_apply_generation ==
         g_ad9910_siggen.apply_generation) ? 0U : 1U;
}

static uint8_t ad9910_siggen_service_cycle_completed(void)
{
    if (AD9910_Service_GetState() != AD9910_SERVICE_STATE_READY) {
        g_ad9910_siggen.service_cycle_started = 1U;
        return 0U;
    }

    return g_ad9910_siggen.service_cycle_started;
}

static void ad9910_siggen_begin_service_cycle(ad9910_siggen_state_t wait_state)
{
    g_ad9910_siggen.service_cycle_started = 0U;
    g_ad9910_siggen.service_cycle_apply_generation =
        g_ad9910_siggen.apply_generation;
    g_ad9910_siggen.service_cycle_single_tone_generation =
        g_ad9910_siggen.single_tone_generation;
    g_ad9910_siggen.service_cycle_ram_preset =
        g_ad9910_siggen.requested_ram_preset;
    g_ad9910_siggen.state = wait_state;
}

static ad9910_tone_config_t ad9910_siggen_build_tone_config(
    const ad9910_siggen_tone_param_t *tone_param)
{
    ad9910_tone_config_t tone;

    tone.frequency_hz = tone_param->frequency_hz;
    tone.amplitude =
        AD9910_AmplitudePercentToASF(tone_param->amplitude_percent);
    tone.phase_offset =
        AD9910_PhaseDegreesToPOW(tone_param->phase_degrees);

    return tone;
}

static int32_t ad9910_siggen_clamp_signed_sample(int32_t sample)
{
    if (sample > AD9910_SIGGEN_SIGNED_SAMPLE_MAX) {
        return AD9910_SIGGEN_SIGNED_SAMPLE_MAX;
    }
    if (sample < -AD9910_SIGGEN_SIGNED_SAMPLE_MAX) {
        return -AD9910_SIGGEN_SIGNED_SAMPLE_MAX;
    }

    return sample;
}

static int32_t ad9910_siggen_scale_sample(int32_t sample, uint8_t percent)
{
    sample = ad9910_siggen_clamp_signed_sample(sample);
    return (sample * (int32_t)percent) / 100;
}

static uint32_t ad9910_siggen_build_polar_word_from_sample(int32_t sample)
{
    uint16_t phase_degrees = AD9910_SIGGEN_POLAR_POSITIVE_PHASE_DEGREES;
    uint16_t amplitude;

    sample = ad9910_siggen_clamp_signed_sample(sample);
    if (sample < 0) {
        phase_degrees = AD9910_SIGGEN_POLAR_NEGATIVE_PHASE_DEGREES;
        sample = -sample;
    }

    amplitude = (uint16_t)sample;
    return AD9910_BuildRamPolarWord(phase_degrees, amplitude);
}

static int32_t ad9910_siggen_get_sine_sample(uint16_t index)
{
    return g_ad9910_siggen_sine64[index % AD9910_SIGGEN_RAM_SAMPLE_COUNT];
}

static int32_t ad9910_siggen_build_triangle_sample(uint16_t index)
{
    const uint16_t phase = index % AD9910_SIGGEN_RAM_SAMPLE_COUNT;

    if (phase < (AD9910_SIGGEN_RAM_SAMPLE_COUNT / 2U)) {
        return -AD9910_SIGGEN_SIGNED_SAMPLE_MAX +
               (((int32_t)phase * 4L * AD9910_SIGGEN_SIGNED_SAMPLE_MAX) /
                (int32_t)AD9910_SIGGEN_RAM_SAMPLE_COUNT);
    }

    return (3L * AD9910_SIGGEN_SIGNED_SAMPLE_MAX) -
           (((int32_t)phase * 4L * AD9910_SIGGEN_SIGNED_SAMPLE_MAX) /
            (int32_t)AD9910_SIGGEN_RAM_SAMPLE_COUNT);
}

static int32_t ad9910_siggen_build_saw_rise_sample(uint16_t index)
{
    const uint16_t phase = index % AD9910_SIGGEN_RAM_SAMPLE_COUNT;

    return -AD9910_SIGGEN_SIGNED_SAMPLE_MAX +
           (((int32_t)phase * 2L * AD9910_SIGGEN_SIGNED_SAMPLE_MAX) /
            (int32_t)(AD9910_SIGGEN_RAM_SAMPLE_COUNT - 1U));
}

static int32_t ad9910_siggen_build_wave_sample(
    const ad9910_siggen_ram_preset_t *preset,
    uint16_t index)
{
    const uint16_t phase_offset =
        (uint16_t)(((uint32_t)preset->tone.phase_degrees *
                    AD9910_SIGGEN_RAM_SAMPLE_COUNT) /
                   360U);
    const uint16_t phase =
        (uint16_t)((index + phase_offset) % AD9910_SIGGEN_RAM_SAMPLE_COUNT);
    int32_t sample;

    switch (preset->waveform) {
    case AD9910_SIGGEN_WAVEFORM_SQUARE:
        sample = (phase < ((uint32_t)AD9910_SIGGEN_RAM_SAMPLE_COUNT *
                           preset->duty_percent) /
                              100U) ?
                     AD9910_SIGGEN_SIGNED_SAMPLE_MAX :
                     -AD9910_SIGGEN_SIGNED_SAMPLE_MAX;
        break;

    case AD9910_SIGGEN_WAVEFORM_TRIANGLE:
        sample = ad9910_siggen_build_triangle_sample(phase);
        break;

    case AD9910_SIGGEN_WAVEFORM_SAW_RISE:
        sample = ad9910_siggen_build_saw_rise_sample(phase);
        break;

    case AD9910_SIGGEN_WAVEFORM_SAW_FALL:
        sample = -ad9910_siggen_build_saw_rise_sample(phase);
        break;

    case AD9910_SIGGEN_WAVEFORM_COMPOSITE:
        sample = ad9910_siggen_get_sine_sample(phase);
        sample += (ad9910_siggen_get_sine_sample((uint16_t)(phase * 2U)) *
                   (int32_t)preset->harmonic2_percent) /
                  100;
        sample += (ad9910_siggen_get_sine_sample((uint16_t)(phase * 3U)) *
                   (int32_t)preset->harmonic3_percent) /
                  100;
        sample = (sample * 100L) /
                 (100L + preset->harmonic2_percent +
                  preset->harmonic3_percent);
        break;

    case AD9910_SIGGEN_WAVEFORM_SINE:
    default:
        sample = ad9910_siggen_get_sine_sample(phase);
        break;
    }

    return ad9910_siggen_scale_sample(sample, preset->tone.amplitude_percent);
}

static HAL_StatusTypeDef ad9910_siggen_build_ram_config(void)
{
    const ad9910_siggen_ram_preset_t *preset =
        &g_ad9910_siggen.ram_presets[g_ad9910_siggen.requested_ram_preset];
    const uint32_t playback_sample_rate_hz =
        preset->tone.frequency_hz * AD9910_SIGGEN_RAM_SAMPLE_COUNT;

    if (ad9910_siggen_ram_tone_is_valid(&preset->tone) == 0U) {
        return HAL_ERROR;
    }

    /* Polar RAM 直接回放 64 点波形样本，FTW 保持为 0。 */
    for (uint16_t index = 0U; index < AD9910_SIGGEN_RAM_SAMPLE_COUNT; ++index) {
        g_ad9910_siggen.ram_words[index] =
            ad9910_siggen_build_polar_word_from_sample(
                ad9910_siggen_build_wave_sample(preset, index));
    }

    g_ad9910_siggen.ram_config.profile_index =
        AD9910_SIGGEN_SINGLE_TONE_PROFILE_INDEX;
    g_ad9910_siggen.ram_config.start_address = 0U;
    g_ad9910_siggen.ram_config.sample_count = AD9910_SIGGEN_RAM_SAMPLE_COUNT;
    g_ad9910_siggen.ram_config.address_step_rate = 0U;
    g_ad9910_siggen.ram_config.playback_sample_rate_hz =
        playback_sample_rate_hz;
    g_ad9910_siggen.ram_config.destination = AD9910_RAM_DESTINATION_POLAR;
    g_ad9910_siggen.ram_config.mode =
        AD9910_RAM_MODE_CONTINUOUS_RECIRCULATE;
    g_ad9910_siggen.ram_config.no_dwell_high = 0U;
    g_ad9910_siggen.ram_config.zero_crossing = 0U;
    g_ad9910_siggen.ram_config.base_tone.frequency_hz = 0U;
    g_ad9910_siggen.ram_config.base_tone.amplitude = AD9910_MAX_AMPLITUDE;
    g_ad9910_siggen.ram_config.base_tone.phase_offset = 0U;
    g_ad9910_siggen.ram_config.ram_words = g_ad9910_siggen.ram_words;
    g_ad9910_siggen.ram_config.ram_word_count = AD9910_SIGGEN_RAM_SAMPLE_COUNT;
    g_ad9910_siggen.ram_playback_sample_rate_hz =
        playback_sample_rate_hz;

    return HAL_OK;
}

static const ad9910_siggen_tone_param_t *ad9910_siggen_get_status_tone(void)
{
    if (g_ad9910_siggen.active_mode == AD9910_SIGGEN_MODE_RAM_WAVEFORM) {
        return &g_ad9910_siggen.active_ram_preset_config.tone;
    }

    return &g_ad9910_siggen.active_single_tone;
}

static void ad9910_siggen_update_status(void)
{
    const ad9910_siggen_ram_preset_t *preset =
        &g_ad9910_siggen.active_ram_preset_config;
    const ad9910_siggen_tone_param_t *tone =
        ad9910_siggen_get_status_tone();

    g_ad9910_siggen_status.app_state = g_ad9910_siggen.state;
    g_ad9910_siggen_status.app_error = g_ad9910_siggen.error;
    g_ad9910_siggen_status.service_state = AD9910_Service_GetState();
    g_ad9910_siggen_status.active_mode = g_ad9910_siggen.active_mode;
    g_ad9910_siggen_status.requested_mode = g_ad9910_siggen.requested_mode;
    g_ad9910_siggen_status.active_ram_waveform = preset->waveform;
    g_ad9910_siggen_status.active_ram_preset =
        g_ad9910_siggen.active_ram_preset;
    g_ad9910_siggen_status.requested_ram_preset =
        g_ad9910_siggen.requested_ram_preset;
    g_ad9910_siggen_status.ram_active = AD9910_Service_IsRamPlaybackActive();
    g_ad9910_siggen_status.pending_apply = g_ad9910_siggen.pending_apply;
    g_ad9910_siggen_status.pending_single_tone_update =
        g_ad9910_siggen.pending_single_tone_update;
    g_ad9910_siggen_status.frequency_hz = tone->frequency_hz;
    g_ad9910_siggen_status.amplitude_percent = tone->amplitude_percent;
    g_ad9910_siggen_status.phase_degrees = tone->phase_degrees;
    g_ad9910_siggen_status.ram_playback_sample_rate_hz =
        g_ad9910_siggen.active_ram_playback_sample_rate_hz;
    g_ad9910_siggen_status.ram_sample_count =
        AD9910_Service_GetRamSampleCount();
    g_ad9910_siggen_status.last_hal_error = AD9910_Service_GetLastHalError();
}

static void ad9910_siggen_init_ram_presets(void)
{
    /* 默认 8 个槽都有信号，后续 TJC 可以覆盖每个槽的参数。 */
    static const ad9910_siggen_waveform_t default_waveforms[AD9910_SIGGEN_RAM_PRESET_COUNT] = {
            AD9910_SIGGEN_WAVEFORM_SINE,
            AD9910_SIGGEN_WAVEFORM_SQUARE,
            AD9910_SIGGEN_WAVEFORM_TRIANGLE,
            AD9910_SIGGEN_WAVEFORM_SAW_RISE,
            AD9910_SIGGEN_WAVEFORM_SAW_FALL,
            AD9910_SIGGEN_WAVEFORM_COMPOSITE,
            AD9910_SIGGEN_WAVEFORM_SINE,
            AD9910_SIGGEN_WAVEFORM_SQUARE,
        };

    for (uint8_t index = 0U; index < AD9910_SIGGEN_RAM_PRESET_COUNT; ++index) {
        g_ad9910_siggen.ram_presets[index].waveform =
            default_waveforms[index];
        g_ad9910_siggen.ram_presets[index].tone.frequency_hz =
            AD9910_SIGGEN_DEFAULT_RAM_FREQUENCY_HZ *
            (uint32_t)(index + 1U);
        g_ad9910_siggen.ram_presets[index].tone.amplitude_percent =
            AD9910_SIGGEN_DEFAULT_AMPLITUDE_PERCENT;
        g_ad9910_siggen.ram_presets[index].tone.phase_degrees =
            AD9910_SIGGEN_DEFAULT_PHASE_DEGREES;
        g_ad9910_siggen.ram_presets[index].duty_percent =
            AD9910_SIGGEN_DEFAULT_DUTY_PERCENT;
        g_ad9910_siggen.ram_presets[index].harmonic2_percent =
            AD9910_SIGGEN_DEFAULT_HARMONIC2_PERCENT;
        g_ad9910_siggen.ram_presets[index].harmonic3_percent =
            AD9910_SIGGEN_DEFAULT_HARMONIC3_PERCENT;
    }
}

HAL_StatusTypeDef AD9910_SignalGenerator_App_Init(SPI_HandleTypeDef *spi)
{
#if AD9910_SIGGEN_ENABLE_SINGLE_TONE
    const ad9910_siggen_mode_t boot_mode =
        AD9910_SIGGEN_MODE_SINGLE_TONE;
#else
    const ad9910_siggen_mode_t boot_mode =
        AD9910_SIGGEN_MODE_RAM_WAVEFORM;
#endif
    HAL_StatusTypeDef status;

    g_ad9910_siggen.single_tone.frequency_hz =
        AD9910_SIGGEN_DEFAULT_SINGLE_FREQUENCY_HZ;
    g_ad9910_siggen.single_tone.amplitude_percent =
        AD9910_SIGGEN_DEFAULT_AMPLITUDE_PERCENT;
    g_ad9910_siggen.single_tone.phase_degrees =
        AD9910_SIGGEN_DEFAULT_PHASE_DEGREES;
    ad9910_siggen_init_ram_presets();
    g_ad9910_siggen.active_single_tone = g_ad9910_siggen.single_tone;
    g_ad9910_siggen.service_cycle_single_tone =
        g_ad9910_siggen.single_tone;
    g_ad9910_siggen.active_ram_preset_config =
        g_ad9910_siggen.ram_presets[0U];
    g_ad9910_siggen.service_cycle_ram_preset_config =
        g_ad9910_siggen.ram_presets[0U];

    g_ad9910_siggen.state = AD9910_SIGGEN_STATE_WAIT_SERVICE;
    g_ad9910_siggen.error = AD9910_SIGGEN_ERROR_NONE;
    g_ad9910_siggen.active_mode = boot_mode;
    g_ad9910_siggen.requested_mode = boot_mode;
    g_ad9910_siggen.active_ram_preset = 0U;
    g_ad9910_siggen.requested_ram_preset = 0U;
    g_ad9910_siggen.service_cycle_ram_preset = 0U;
    g_ad9910_siggen.pending_apply = 1U;
    g_ad9910_siggen.pending_single_tone_update =
        (boot_mode == AD9910_SIGGEN_MODE_SINGLE_TONE) ? 1U : 0U;
    g_ad9910_siggen.service_cycle_started = 0U;
    g_ad9910_siggen.ram_playback_sample_rate_hz = 0U;
    g_ad9910_siggen.active_ram_playback_sample_rate_hz = 0U;
    g_ad9910_siggen.service_cycle_ram_playback_sample_rate_hz = 0U;
    g_ad9910_siggen.apply_generation = 1U;
    g_ad9910_siggen.service_cycle_apply_generation = 0U;
    g_ad9910_siggen.single_tone_generation = 1U;
    g_ad9910_siggen.service_cycle_single_tone_generation = 0U;

    status = AD9910_Service_Init(spi);
    if (status != HAL_OK) {
        ad9910_siggen_set_error(AD9910_SIGGEN_ERROR_SERVICE_COMMAND);
        ad9910_siggen_update_status();
        return status;
    }

    ad9910_siggen_update_status();
    return HAL_OK;
}

void AD9910_SignalGenerator_App_Process(void)
{
    HAL_StatusTypeDef status;

    AD9910_Service_Process();

    if (AD9910_Service_GetState() == AD9910_SERVICE_STATE_ERROR) {
        ad9910_siggen_set_error(AD9910_SIGGEN_ERROR_SERVICE_STATE);
    }

    switch (g_ad9910_siggen.state) {
    case AD9910_SIGGEN_STATE_WAIT_SERVICE:
        if (AD9910_Service_GetState() == AD9910_SERVICE_STATE_READY) {
            g_ad9910_siggen.state = AD9910_SIGGEN_STATE_READY;
        }
        break;

    case AD9910_SIGGEN_STATE_READY:
        if ((g_ad9910_siggen.requested_mode ==
             AD9910_SIGGEN_MODE_SINGLE_TONE) &&
            (AD9910_Service_IsRamPlaybackActive() != 0U)) {
            status = AD9910_Service_StopRamPlayback(
                g_ad9910_siggen.single_tone.frequency_hz);
            if (status == HAL_OK) {
                ad9910_siggen_begin_service_cycle(
                    AD9910_SIGGEN_STATE_WAIT_RAM_STOP);
            } else if (status != HAL_BUSY) {
                ad9910_siggen_set_error(
                    AD9910_SIGGEN_ERROR_SERVICE_COMMAND);
            }
            break;
        }

        if ((g_ad9910_siggen.requested_mode ==
             AD9910_SIGGEN_MODE_SINGLE_TONE) &&
            ((g_ad9910_siggen.pending_apply != 0U) ||
             (g_ad9910_siggen.pending_single_tone_update != 0U))) {
            const ad9910_tone_config_t tone =
                ad9910_siggen_build_tone_config(
                    &g_ad9910_siggen.single_tone);
            g_ad9910_siggen.service_cycle_single_tone =
                g_ad9910_siggen.single_tone;
            status = AD9910_Service_SetTone(&tone);
            if (status == HAL_OK) {
                ad9910_siggen_begin_service_cycle(
                    AD9910_SIGGEN_STATE_WAIT_SINGLE_TONE);
            } else if (status != HAL_BUSY) {
                ad9910_siggen_set_error(
                    AD9910_SIGGEN_ERROR_SERVICE_COMMAND);
            }
            break;
        }

        if ((g_ad9910_siggen.requested_mode ==
             AD9910_SIGGEN_MODE_RAM_WAVEFORM) &&
            (g_ad9910_siggen.pending_apply != 0U)) {
            if (ad9910_siggen_build_ram_config() != HAL_OK) {
                g_ad9910_siggen.error = AD9910_SIGGEN_ERROR_INVALID_COMMAND;
                g_ad9910_siggen.pending_apply = 0U;
                break;
            }

            status = AD9910_Service_StartRamPlayback(
                &g_ad9910_siggen.ram_config);
            if (status == HAL_OK) {
                g_ad9910_siggen.service_cycle_ram_preset_config =
                    g_ad9910_siggen.ram_presets[
                        g_ad9910_siggen.requested_ram_preset];
                g_ad9910_siggen.service_cycle_ram_playback_sample_rate_hz =
                    g_ad9910_siggen.ram_playback_sample_rate_hz;
                ad9910_siggen_begin_service_cycle(
                    AD9910_SIGGEN_STATE_WAIT_RAM_START);
            } else if (status != HAL_BUSY) {
                ad9910_siggen_set_error(AD9910_SIGGEN_ERROR_SERVICE_COMMAND);
            }
        }
        break;

    case AD9910_SIGGEN_STATE_WAIT_SINGLE_TONE:
        if (ad9910_siggen_service_cycle_completed() != 0U) {
            g_ad9910_siggen.active_mode = AD9910_SIGGEN_MODE_SINGLE_TONE;
            g_ad9910_siggen.active_single_tone =
                g_ad9910_siggen.service_cycle_single_tone;
            if (g_ad9910_siggen.service_cycle_single_tone_generation ==
                g_ad9910_siggen.single_tone_generation) {
                g_ad9910_siggen.pending_single_tone_update = 0U;
            }
            ad9910_siggen_complete_apply_request();
            g_ad9910_siggen.state = AD9910_SIGGEN_STATE_READY;
        }
        break;

    case AD9910_SIGGEN_STATE_WAIT_RAM_START:
        if ((ad9910_siggen_service_cycle_completed() != 0U) &&
            (AD9910_Service_IsRamPlaybackActive() != 0U)) {
            g_ad9910_siggen.active_mode = AD9910_SIGGEN_MODE_RAM_WAVEFORM;
            g_ad9910_siggen.active_ram_preset =
                g_ad9910_siggen.service_cycle_ram_preset;
            g_ad9910_siggen.active_ram_preset_config =
                g_ad9910_siggen.service_cycle_ram_preset_config;
            g_ad9910_siggen.active_ram_playback_sample_rate_hz =
                g_ad9910_siggen.service_cycle_ram_playback_sample_rate_hz;
            ad9910_siggen_complete_apply_request();
            g_ad9910_siggen.state = AD9910_SIGGEN_STATE_READY;
        }
        break;

    case AD9910_SIGGEN_STATE_WAIT_RAM_STOP:
        if (ad9910_siggen_service_cycle_completed() != 0U) {
            g_ad9910_siggen.active_mode = AD9910_SIGGEN_MODE_SINGLE_TONE;
            g_ad9910_siggen.pending_single_tone_update = 1U;
            g_ad9910_siggen.pending_apply = 1U;
            g_ad9910_siggen.state = AD9910_SIGGEN_STATE_READY;
        }
        break;

    case AD9910_SIGGEN_STATE_ERROR:
    default:
        break;
    }

    ad9910_siggen_update_status();
}

HAL_StatusTypeDef AD9910_SignalGenerator_SetMode(ad9910_siggen_mode_t mode)
{
    if (ad9910_siggen_mode_is_valid(mode) == 0U) {
        return HAL_ERROR;
    }

    g_ad9910_siggen.error = AD9910_SIGGEN_ERROR_NONE;
    g_ad9910_siggen.requested_mode = mode;
    ad9910_siggen_request_apply();
    return HAL_OK;
}

HAL_StatusTypeDef AD9910_SignalGenerator_SetSingleTone(
    const ad9910_siggen_tone_param_t *tone)
{
    if ((AD9910_SIGGEN_ENABLE_SINGLE_TONE == 0U) ||
        (ad9910_siggen_tone_is_valid(tone) == 0U)) {
        return HAL_ERROR;
    }

    g_ad9910_siggen.error = AD9910_SIGGEN_ERROR_NONE;
    g_ad9910_siggen.single_tone = *tone;
    g_ad9910_siggen.single_tone_generation++;
    g_ad9910_siggen.pending_single_tone_update = 1U;
    if (g_ad9910_siggen.requested_mode == AD9910_SIGGEN_MODE_SINGLE_TONE) {
        ad9910_siggen_request_apply();
    }

    return HAL_OK;
}

HAL_StatusTypeDef AD9910_SignalGenerator_SelectRamPreset(uint8_t preset_index)
{
    if ((AD9910_SIGGEN_ENABLE_RAM_PLAYBACK == 0U) ||
        (ad9910_siggen_ram_preset_index_is_valid(preset_index) == 0U)) {
        return HAL_ERROR;
    }

    g_ad9910_siggen.error = AD9910_SIGGEN_ERROR_NONE;
    g_ad9910_siggen.requested_ram_preset = preset_index;
    if (g_ad9910_siggen.requested_mode == AD9910_SIGGEN_MODE_RAM_WAVEFORM) {
        ad9910_siggen_request_apply();
    }

    return HAL_OK;
}

HAL_StatusTypeDef AD9910_SignalGenerator_SetRamPresetTone(
    uint8_t preset_index,
    const ad9910_siggen_tone_param_t *tone)
{
    if ((AD9910_SIGGEN_ENABLE_RAM_PLAYBACK == 0U) ||
        (ad9910_siggen_ram_preset_index_is_valid(preset_index) == 0U) ||
        (ad9910_siggen_ram_tone_is_valid(tone) == 0U)) {
        return HAL_ERROR;
    }

    g_ad9910_siggen.error = AD9910_SIGGEN_ERROR_NONE;
    g_ad9910_siggen.ram_presets[preset_index].tone = *tone;
    if ((preset_index == g_ad9910_siggen.requested_ram_preset) &&
        (g_ad9910_siggen.requested_mode ==
         AD9910_SIGGEN_MODE_RAM_WAVEFORM)) {
        ad9910_siggen_request_apply();
    }

    return HAL_OK;
}

HAL_StatusTypeDef AD9910_SignalGenerator_SetRamPresetWaveform(
    uint8_t preset_index,
    ad9910_siggen_waveform_t waveform)
{
    if ((AD9910_SIGGEN_ENABLE_RAM_PLAYBACK == 0U) ||
        (ad9910_siggen_ram_preset_index_is_valid(preset_index) == 0U) ||
        (ad9910_siggen_waveform_is_valid(waveform) == 0U)) {
        return HAL_ERROR;
    }

    g_ad9910_siggen.error = AD9910_SIGGEN_ERROR_NONE;
    g_ad9910_siggen.ram_presets[preset_index].waveform = waveform;
    if ((preset_index == g_ad9910_siggen.requested_ram_preset) &&
        (g_ad9910_siggen.requested_mode ==
         AD9910_SIGGEN_MODE_RAM_WAVEFORM)) {
        ad9910_siggen_request_apply();
    }

    return HAL_OK;
}

HAL_StatusTypeDef AD9910_SignalGenerator_SetRamPresetDuty(
    uint8_t preset_index,
    uint8_t duty_percent)
{
    if ((AD9910_SIGGEN_ENABLE_RAM_PLAYBACK == 0U) ||
        (ad9910_siggen_ram_preset_index_is_valid(preset_index) == 0U) ||
        (duty_percent == 0U) ||
        (duty_percent > 100U)) {
        return HAL_ERROR;
    }

    g_ad9910_siggen.error = AD9910_SIGGEN_ERROR_NONE;
    g_ad9910_siggen.ram_presets[preset_index].duty_percent = duty_percent;
    if ((preset_index == g_ad9910_siggen.requested_ram_preset) &&
        (g_ad9910_siggen.requested_mode ==
         AD9910_SIGGEN_MODE_RAM_WAVEFORM)) {
        ad9910_siggen_request_apply();
    }

    return HAL_OK;
}

HAL_StatusTypeDef AD9910_SignalGenerator_SetRamPresetComposite(
    uint8_t preset_index,
    uint8_t harmonic2_percent,
    uint8_t harmonic3_percent)
{
    if ((AD9910_SIGGEN_ENABLE_RAM_PLAYBACK == 0U) ||
        (ad9910_siggen_ram_preset_index_is_valid(preset_index) == 0U) ||
        (harmonic2_percent > 100U) ||
        (harmonic3_percent > 100U)) {
        return HAL_ERROR;
    }

    g_ad9910_siggen.error = AD9910_SIGGEN_ERROR_NONE;
    g_ad9910_siggen.ram_presets[preset_index].waveform =
        AD9910_SIGGEN_WAVEFORM_COMPOSITE;
    g_ad9910_siggen.ram_presets[preset_index].harmonic2_percent =
        harmonic2_percent;
    g_ad9910_siggen.ram_presets[preset_index].harmonic3_percent =
        harmonic3_percent;
    if ((preset_index == g_ad9910_siggen.requested_ram_preset) &&
        (g_ad9910_siggen.requested_mode ==
         AD9910_SIGGEN_MODE_RAM_WAVEFORM)) {
        ad9910_siggen_request_apply();
    }

    return HAL_OK;
}

HAL_StatusTypeDef AD9910_SignalGenerator_HandleCommand(
    const ad9910_siggen_command_t *command)
{
    if (command == NULL) {
        return HAL_ERROR;
    }

    switch (command->type) {
    case AD9910_SIGGEN_COMMAND_SET_MODE:
        return AD9910_SignalGenerator_SetMode(command->mode);

    case AD9910_SIGGEN_COMMAND_SET_SINGLE_TONE:
        return AD9910_SignalGenerator_SetSingleTone(&command->tone);

    case AD9910_SIGGEN_COMMAND_SELECT_RAM_PRESET:
        return AD9910_SignalGenerator_SelectRamPreset(
            command->ram_preset_index);

    case AD9910_SIGGEN_COMMAND_SET_RAM_PRESET_TONE:
        return AD9910_SignalGenerator_SetRamPresetTone(
            command->ram_preset_index,
            &command->tone);

    case AD9910_SIGGEN_COMMAND_SET_RAM_PRESET_WAVEFORM:
        return AD9910_SignalGenerator_SetRamPresetWaveform(
            command->ram_preset_index,
            command->waveform);

    case AD9910_SIGGEN_COMMAND_SET_RAM_PRESET_DUTY:
        return AD9910_SignalGenerator_SetRamPresetDuty(
            command->ram_preset_index,
            command->duty_percent);

    case AD9910_SIGGEN_COMMAND_SET_RAM_PRESET_COMPOSITE:
        return AD9910_SignalGenerator_SetRamPresetComposite(
            command->ram_preset_index,
            command->harmonic2_percent,
            command->harmonic3_percent);

    case AD9910_SIGGEN_COMMAND_APPLY_ACTIVE:
        ad9910_siggen_request_apply();
        return HAL_OK;

    default:
        return HAL_ERROR;
    }
}

const ad9910_siggen_tone_param_t *AD9910_SignalGenerator_GetSingleTone(void)
{
    return &g_ad9910_siggen.single_tone;
}

const ad9910_siggen_ram_preset_t *AD9910_SignalGenerator_GetRamPreset(
    uint8_t preset_index)
{
    if (ad9910_siggen_ram_preset_index_is_valid(preset_index) == 0U) {
        return NULL;
    }

    return &g_ad9910_siggen.ram_presets[preset_index];
}

const ad9910_siggen_status_t *AD9910_SignalGenerator_GetStatus(void)
{
    return &g_ad9910_siggen_status;
}
