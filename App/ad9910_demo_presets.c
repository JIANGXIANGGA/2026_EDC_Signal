#include "ad9910_demo_presets.h"

#include <stddef.h>

/* 修改这里即可选择上电自动运行的预设。 */
#define AD9910_DEMO_BOOT_PRESET AD9910_DEMO_PRESET_USER

/* 用户自定义参数：通常只需要修改这一组。 */
static const ad9910_demo_config_t g_ad9910_user_config = {
    .mode = AD9910_DEMO_MODE_CONTINUOUS_SWEEP,
    .start_frequency_hz = 100000U,
    .stop_frequency_hz = 10000000U,
    .sweep_time_ms = 1000U,
    .return_time_ms = 500U,
    .start_hold_ms = 100U,
    .stop_hold_ms = 100U,
    .target_steps = 10000U,
    .amplitude_percent = 100U,
    .phase_degrees = 0U,
};

static const ad9910_demo_config_t g_ad9910_presets[AD9910_DEMO_PRESET_COUNT] = {
    [AD9910_DEMO_PRESET_FIXED_1KHZ] = {
        .mode = AD9910_DEMO_MODE_FIXED,
        .start_frequency_hz = 1000U,
        .amplitude_percent = 100U,
        .phase_degrees = 0U,
    },
    [AD9910_DEMO_PRESET_FIXED_1MHZ] = {
        .mode = AD9910_DEMO_MODE_FIXED,
        .start_frequency_hz = 1000000U,
        .amplitude_percent = 100U,
        .phase_degrees = 0U,
    },
    [AD9910_DEMO_PRESET_AUDIO_SWEEP] = {
        .mode = AD9910_DEMO_MODE_CONTINUOUS_SWEEP,
        .start_frequency_hz = 1000U,
        .stop_frequency_hz = 100000U,
        .sweep_time_ms = 1000U,
        .return_time_ms = 1000U,
        .start_hold_ms = 100U,
        .stop_hold_ms = 100U,
        .target_steps = 5000U,
        .amplitude_percent = 100U,
        .phase_degrees = 0U,
    },
    [AD9910_DEMO_PRESET_RF_SWEEP] = {
        .mode = AD9910_DEMO_MODE_CONTINUOUS_SWEEP,
        .start_frequency_hz = 100000U,
        .stop_frequency_hz = 100000000U,
        .sweep_time_ms = 1000U,
        .return_time_ms = 500U,
        .start_hold_ms = 50U,
        .stop_hold_ms = 50U,
        .target_steps = 10000U,
        .amplitude_percent = 100U,
        .phase_degrees = 0U,
    },
    [AD9910_DEMO_PRESET_SINGLE_RF_SWEEP] = {
        .mode = AD9910_DEMO_MODE_SINGLE_SWEEP,
        .start_frequency_hz = 100000U,
        .stop_frequency_hz = 100000000U,
        .sweep_time_ms = 1000U,
        .return_time_ms = 0U,
        .start_hold_ms = 100U,
        .stop_hold_ms = 100U,
        .target_steps = 10000U,
        .amplitude_percent = 100U,
        .phase_degrees = 0U,
    },
};

ad9910_demo_preset_id_t AD9910_Demo_Presets_GetBootPreset(void)
{
    return AD9910_DEMO_BOOT_PRESET;
}

const ad9910_demo_config_t *AD9910_Demo_Presets_Get(
    ad9910_demo_preset_id_t preset)
{
    if (preset >= AD9910_DEMO_PRESET_COUNT) {
        return NULL;
    }

    if (preset == AD9910_DEMO_PRESET_USER) {
        return &g_ad9910_user_config;
    }

    return &g_ad9910_presets[preset];
}
