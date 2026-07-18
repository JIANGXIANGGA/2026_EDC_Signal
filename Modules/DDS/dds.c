#include "dds.h"
#include <math.h>
#include <stddef.h>

#define DDS_PI       3.14159265358979323846f
#define DDS_DAC_MAX  4095U
#define DDS_DAC_MID  2048

static float dds_sine_value(float theta)
{
    return sinf(theta);
}

static float dds_composite_value(float theta)
{
    return 0.55f * sinf(theta)
         + 0.25f * cosf(2.0f * theta)
         + 0.15f * sinf(3.0f * theta + 0.45f)
         + 0.08f * cosf(5.0f * theta - 0.70f);
}

static void dds_build_table(uint16_t *table, float (*value_fn)(float))
{
    /* 由波形函数生成并归一化为 12 位 DAC 查找表。 */
    float min_value = value_fn(0.0f);
    float max_value = min_value;

    for (uint32_t i = 1; i < DDS_TABLE_SIZE; i++) {
        float theta = 2.0f * DDS_PI * (float)i / (float)DDS_TABLE_SIZE;
        float value = value_fn(theta);

        if (value < min_value) min_value = value;
        if (value > max_value) max_value = value;
    }

    float span = max_value - min_value;
    if (span <= 0.0f) span = 1.0f;

    for (uint32_t i = 0; i < DDS_TABLE_SIZE; i++) {
        float theta = 2.0f * DDS_PI * (float)i / (float)DDS_TABLE_SIZE;
        float value = value_fn(theta);
        float normalized = (value - min_value) / span;
        float dac_value = normalized * (float)DDS_DAC_MAX;

        if (dac_value < 0.0f) dac_value = 0.0f;
        if (dac_value > (float)DDS_DAC_MAX) dac_value = (float)DDS_DAC_MAX;

        table[i] = (uint16_t)(dac_value + 0.5f);
    }
}

static uint16_t dds_sample_from_table(const uint16_t *table, uint32_t phase)
{
    /*
     * 高 8 位作为表索引，接下来的 8 位作为插值系数。
     */
    uint8_t index = (uint8_t)(phase >> 24);
    uint32_t frac = (phase >> 16) & 0xFFU;

    uint16_t y0 = table[index];
    uint16_t y1 = table[(index + 1U) & DDS_TABLE_MASK];
    int32_t res = (int32_t)y0 + (((int32_t)(y1 - y0) * (int32_t)frac) >> 8);

    return (uint16_t)res;
}

static uint16_t dds_sine_sample(const dds_t *dds, uint32_t phase)
{
    return dds_sample_from_table(dds->sine_table, phase);
}

static uint16_t dds_composite_sample(const dds_t *dds, uint32_t phase)
{
    return dds_sample_from_table(dds->composite_table, phase);
}

static dds_waveform_fn_t dds_get_waveform_fn(dds_waveform_t waveform)
{
    switch (waveform) {
    case DDS_WAVE_SINE:
        return dds_sine_sample;
    case DDS_WAVE_COMPOSITE:
    default:
        return dds_composite_sample;
    }
}

static int dds_waveform_is_valid(dds_waveform_t waveform)
{
    return (uint32_t)waveform < (uint32_t)DDS_WAVE_COUNT;
}

static uint8_t dds_clamp_amplitude(uint8_t amplitude_percent)
{
    if (amplitude_percent < DDS_AMPLITUDE_MIN_PERCENT) {
        return DDS_AMPLITUDE_MIN_PERCENT;
    }
    if (amplitude_percent > DDS_AMPLITUDE_MAX_PERCENT) {
        return DDS_AMPLITUDE_MAX_PERCENT;
    }
    return amplitude_percent;
}

static void dds_update_phase_inc(dds_t *dds)
{
    if (dds->cfg.sample_rate == 0U) {
        dds->phase_inc = 0U;
        return;
    }

    dds->phase_inc = (uint32_t)(((uint64_t)dds->freq_hz << 32) /
                                dds->cfg.sample_rate);
}

static uint16_t dds_apply_amplitude(const dds_t *dds, uint16_t sample)
{
    int32_t centered;
    int32_t scaled;

    centered = (int32_t)sample - DDS_DAC_MID;
    scaled = DDS_DAC_MID +
             (centered * (int32_t)dds->cfg.amplitude_percent) / 100;

    if (scaled < 0) {
        scaled = 0;
    }
    if (scaled > (int32_t)DDS_DAC_MAX) {
        scaled = (int32_t)DDS_DAC_MAX;
    }
    return (uint16_t)scaled;
}

void dds_init(dds_t *dds, const dds_config_t *cfg)
{
    if (dds == NULL)  return;

    /* 即使 cfg 为空或只填了一部分，也给出默认值，保证模块可用。 */
    dds->cfg.sample_rate = (cfg != NULL && cfg->sample_rate != 0U) ? cfg->sample_rate : DDS_SAMPLE_RATE;
    dds->cfg.waveform = (cfg != NULL && dds_waveform_is_valid(cfg->waveform)) ? cfg->waveform : DDS_WAVE_SINE;
    dds->cfg.amplitude_percent =
        (cfg != NULL && cfg->amplitude_percent != 0U) ?
        dds_clamp_amplitude(cfg->amplitude_percent) :
        DDS_AMPLITUDE_DEFAULT_PERCENT;
    dds->freq_hz = 0U;
    dds->phase_acc = 0U;
    dds->phase_inc = 0U;

    dds_build_table(dds->sine_table, dds_sine_value);
    dds_build_table(dds->composite_table, dds_composite_value);

    dds->waveform_fn = dds_get_waveform_fn(dds->cfg.waveform);
}

void dds_set_sample_rate(dds_t *dds, uint32_t sample_rate)
{
    if (dds == NULL || sample_rate == 0U) return;

    dds->cfg.sample_rate = sample_rate;
    /* 保留当前频率和相位，仅按新采样率更新相位步进。 */
    dds_update_phase_inc(dds);
}

void dds_set_freq(dds_t *dds, uint32_t freq_hz)
{
    if (dds == NULL) return;

    dds->freq_hz = freq_hz;
    /* 32 位相位累加器按配置的更新频率换算出步进值。 */
    dds_update_phase_inc(dds);
}

void dds_set_amplitude(dds_t *dds, uint8_t amplitude_percent)
{
    if (dds == NULL) return;

    dds->cfg.amplitude_percent = dds_clamp_amplitude(amplitude_percent);
}

void dds_set_waveform(dds_t *dds, dds_waveform_t waveform)
{
    if (dds == NULL || !dds_waveform_is_valid(waveform)) return;

    /* 切换波形时复位相位，保证从 0 相位重新开始。 */
    dds->phase_acc = 0U;
    dds->cfg.waveform = waveform;
    dds->waveform_fn = dds_get_waveform_fn(dds->cfg.waveform);
}

void dds_next_waveform(dds_t *dds)
{
    if (dds == NULL) return;

    dds_waveform_t next = (dds->cfg.waveform + 1U) % DDS_WAVE_COUNT;
    dds_set_waveform(dds, next);
}

uint32_t dds_get_sample_rate(const dds_t *dds)
{
    return (dds != NULL) ? dds->cfg.sample_rate : 0U;
}

uint32_t dds_get_freq(const dds_t *dds)
{
    return (dds != NULL) ? dds->freq_hz : 0U;
}

uint8_t dds_get_amplitude(const dds_t *dds)
{
    return (dds != NULL) ? dds->cfg.amplitude_percent : 0U;
}

dds_waveform_t dds_get_waveform(const dds_t *dds)
{
    return (dds != NULL) ? dds->cfg.waveform : DDS_WAVE_SINE;
}

uint16_t dds_get_sample(dds_t *dds)
{
    if (dds == NULL || dds->waveform_fn == NULL) return 0U;

    uint16_t sample = dds->waveform_fn(dds, dds->phase_acc);
    dds->phase_acc += dds->phase_inc;
    return dds_apply_amplitude(dds, sample);
}

uint16_t dds_peek_sample(const dds_t *dds, uint32_t phase)
{
    if (dds == NULL || dds->waveform_fn == NULL) return 0U;

    return dds_apply_amplitude(dds, dds->waveform_fn(dds, phase));
}
