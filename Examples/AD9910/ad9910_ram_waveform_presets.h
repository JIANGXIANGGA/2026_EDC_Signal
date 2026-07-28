#ifndef AD9910_RAM_WAVEFORM_PRESETS_H
#define AD9910_RAM_WAVEFORM_PRESETS_H

#include <stdint.h>

#include "ad9910_service.h"

typedef struct {
    uint8_t profile_index;
    uint32_t carrier_frequency_hz;
    uint8_t carrier_amplitude_percent;
    uint16_t carrier_phase_degrees;
    uint32_t playback_sample_rate_hz;
    ad9910_ram_destination_t destination;
    ad9910_ram_mode_t mode;
    uint8_t no_dwell_high;
    uint8_t zero_crossing;
    const uint16_t *amplitude_samples;
    uint16_t sample_count;
} ad9910_ram_waveform_preset_t;

const ad9910_ram_waveform_preset_t *AD9910_Ram_Waveform_GetBootPreset(void);

#endif
