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

/** @brief 根据 DDS 实例和指定相位返回一个波形采样值的函数类型。 */
typedef uint16_t (*dds_waveform_fn_t)(const dds_t *dds, uint32_t phase);

typedef struct {
    uint32_t sample_rate;          /**< DDS 每秒生成的采样点数。 */
    dds_waveform_t waveform;       /**< 当前选用的波形类型。 */
    uint8_t amplitude_percent;     /**< 相对满量程的输出幅度百分比。 */
} dds_config_t;

struct dds_s {
    dds_config_t cfg;                         /**< 当前实例的运行配置。 */
    uint32_t freq_hz;                         /**< 当前输出频率，单位 Hz。 */
    uint32_t phase_acc;                       /**< 32 位相位累加器。 */
    uint32_t phase_inc;                       /**< 每个采样点增加的相位步进。 */
    dds_waveform_fn_t waveform_fn;             /**< 当前波形对应的采样函数。 */
    uint16_t sine_table[DDS_TABLE_SIZE];       /**< 12 位正弦波查找表。 */
    uint16_t composite_table[DDS_TABLE_SIZE];  /**< 12 位复合波查找表。 */
};

/** @brief 初始化 DDS 实例、波形查找表和运行参数。 */
void dds_init(dds_t *dds, const dds_config_t *cfg);
/** @brief 设置 DDS 采样率并更新相位步进。 */
void dds_set_sample_rate(dds_t *dds, uint32_t sample_rate);
/** @brief 设置 DDS 输出频率并更新相位步进。 */
void dds_set_freq(dds_t *dds, uint32_t freq_hz);
/** @brief 设置 DDS 输出幅度百分比。 */
void dds_set_amplitude(dds_t *dds, uint8_t amplitude_percent);
/** @brief 设置 DDS 输出波形并复位相位。 */
void dds_set_waveform(dds_t *dds, dds_waveform_t waveform);
/** @brief 循环切换到下一种 DDS 波形。 */
void dds_next_waveform(dds_t *dds);
/** @brief 读取 DDS 当前采样率。 */
uint32_t dds_get_sample_rate(const dds_t *dds);
/** @brief 读取 DDS 当前输出频率。 */
uint32_t dds_get_freq(const dds_t *dds);
/** @brief 读取 DDS 当前幅度百分比。 */
uint8_t dds_get_amplitude(const dds_t *dds);
/** @brief 读取 DDS 当前波形类型。 */
dds_waveform_t dds_get_waveform(const dds_t *dds);
/** @brief 生成一个采样值并推进相位累加器。 */
uint16_t dds_get_sample(dds_t *dds);
/** @brief 读取指定相位采样值且不推进相位。 */
uint16_t dds_peek_sample(const dds_t *dds, uint32_t phase);
