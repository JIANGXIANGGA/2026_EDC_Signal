#pragma once
#include <stdint.h>

/*
 * DDS 模块：
 * - 根据相位累加器生成 DAC 采样值
 * - 支持多实例
 * - 波形和采样率由每个实例单独配置
 */

#define DDS_TABLE_SIZE 256U
#define DDS_TABLE_MASK  (DDS_TABLE_SIZE - 1U)
#define DDS_SAMPLE_RATE 10000U
#define DDS_AMPLITUDE_MIN_PERCENT     10U
#define DDS_AMPLITUDE_MAX_PERCENT     100U
#define DDS_AMPLITUDE_DEFAULT_PERCENT 100U

#if ((DDS_TABLE_SIZE & DDS_TABLE_MASK) != 0U)
#error "DDS_TABLE_SIZE must be a power of two"
#endif

typedef enum {
    DDS_WAVE_SINE,
    DDS_WAVE_COMPOSITE,
    DDS_WAVE_COUNT
} dds_waveform_t;

typedef struct dds_s dds_t;

typedef uint16_t (*dds_waveform_fn_t)(const dds_t *dds, uint32_t phase);

typedef struct {
    uint32_t sample_rate;
    dds_waveform_t waveform;
    uint8_t amplitude_percent;
} dds_config_t;

struct dds_s {
    dds_config_t cfg;
    uint32_t freq_hz;
    uint32_t phase_acc;
    uint32_t phase_inc;
    dds_waveform_fn_t waveform_fn;
    uint16_t sine_table[DDS_TABLE_SIZE];
    uint16_t composite_table[DDS_TABLE_SIZE];
};

void dds_init(dds_t *dds, const dds_config_t *cfg);
void dds_set_sample_rate(dds_t *dds, uint32_t sample_rate);
void dds_set_freq(dds_t *dds, uint32_t freq_hz);
void dds_set_amplitude(dds_t *dds, uint8_t amplitude_percent);
void dds_set_waveform(dds_t *dds, dds_waveform_t waveform);
void dds_next_waveform(dds_t *dds);
uint32_t dds_get_sample_rate(const dds_t *dds);
uint32_t dds_get_freq(const dds_t *dds);
uint8_t dds_get_amplitude(const dds_t *dds);
dds_waveform_t dds_get_waveform(const dds_t *dds);
uint16_t dds_get_sample(dds_t *dds);
uint16_t dds_peek_sample(const dds_t *dds, uint32_t phase);
