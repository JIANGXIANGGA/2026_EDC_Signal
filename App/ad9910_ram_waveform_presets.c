#include "ad9910_ram_waveform_presets.h"

#include "ad9910.h"

/*
 * 用户主要修改区：
 * 1. 修改 g_user_amplitude_samples[] 可以改变自定义幅度波形。
 * 2. 样本值范围为 0~16383，对应 AD9910 14 bit ASF。
 * 3. playback_sample_rate_hz 是 RAM 取样率，不是最终载波频率。
 * 4. carrier_frequency_hz 是 AD9910 输出载波频率。
 */
static const uint16_t g_user_amplitude_samples[] = {
    8192U, 8991U, 9781U, 10553U, 11300U, 12013U, 12684U, 13307U,
    13873U, 14376U, 14810U, 15169U, 15447U, 15640U, 15743U, 15755U,
    15673U, 15498U, 15233U, 14882U, 14451U, 13947U, 13377U, 12750U,
    12076U, 11365U, 10627U, 9868U, 9098U, 8325U, 7556U, 6799U,
    6064U, 5358U, 4689U, 4066U, 3495U, 2985U, 2541U, 2170U,
    1877U, 1667U, 1544U, 1509U, 1563U, 1705U, 1934U, 2246U,
    2638U, 3105U, 3641U, 4241U, 4897U, 5602U, 6348U, 7127U,
    7930U, 8748U, 9572U, 10391U, 11196U, 11978U, 12727U, 13434U,
};

static const ad9910_ram_waveform_preset_t g_boot_preset = {
    .profile_index = 0U,
    .carrier_frequency_hz = 1000000U,
    .carrier_amplitude_percent = 100U,
    .carrier_phase_degrees = 0U,
    .playback_sample_rate_hz = 100000U,
    .destination = AD9910_RAM_DESTINATION_AMPLITUDE,
    .mode = AD9910_RAM_MODE_CONTINUOUS_RECIRCULATE,
    .no_dwell_high = 0U,
    .zero_crossing = 0U,
    .amplitude_samples = g_user_amplitude_samples,
    .sample_count =
        (uint16_t)(sizeof(g_user_amplitude_samples) /
                   sizeof(g_user_amplitude_samples[0])),
};

const ad9910_ram_waveform_preset_t *AD9910_Ram_Waveform_GetBootPreset(void)
{
    return &g_boot_preset;
}
