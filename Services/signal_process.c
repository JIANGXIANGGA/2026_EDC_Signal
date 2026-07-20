#include "signal_process.h"

#include <math.h>
#include <stddef.h>

#define SIGNAL_PROCESS_ADC_FULL_SCALE_PEAK 2047.5f
#define SIGNAL_PROCESS_MIN_MAGNITUDE        0.0001f

/** @brief 将完整时域采样等间隔抽取为固定点数的显示快照。 */
uint8_t SignalProcess_BuildTimeSnapshot(const uint16_t *source,
                                        uint32_t source_count,
                                        int32_t *destination,
                                        uint16_t destination_count)
{
    uint32_t point_index; /* 目标时域快照的数据点索引。 */

    if ((source == NULL) || (destination == NULL) ||
        (source_count < 2U) || (destination_count < 2U)) {
        return 0U;
    }

    for (point_index = 0U;
         point_index < destination_count;
         point_index++) {
        uint32_t source_index =
            (point_index * (source_count - 1U)) /
            (destination_count - 1U); /* 与当前显示点对应的原始采样索引。 */

        destination[point_index] =
            (int32_t)(source[source_index] & 0x0FFFU);
    }
    return 1U;
}

/** @brief 将线性单边幅度谱压缩为固定点数的 dBFS 显示快照。 */
uint8_t SignalProcess_BuildSpectrumSnapshot(const float *magnitudes,
                                            uint16_t bin_count,
                                            int32_t *destination,
                                            uint16_t destination_count)
{
    uint32_t point_index;     /* 目标频谱快照的数据点索引。 */
    uint32_t usable_bin_count; /* 去除直流分量后可用于显示的频点数。 */

    if ((magnitudes == NULL) || (destination == NULL) ||
        (bin_count < 3U) || (destination_count == 0U)) {
        return 0U;
    }

    /* 跳过直流 bin 0，将其余频点按区间最大值压缩，保留窄带峰值。 */
    usable_bin_count = (uint32_t)bin_count - 1U;
    for (point_index = 0U;
         point_index < destination_count;
         point_index++) {
        uint32_t bin_index; /* 当前压缩区间内的 FFT 频点索引。 */
        uint32_t begin_bin = 1U +
            (point_index * usable_bin_count) / destination_count; /* 当前显示点覆盖的首个频点。 */
        uint32_t end_bin = 1U +
            ((point_index + 1U) * usable_bin_count) / destination_count; /* 当前显示点覆盖范围的结束频点。 */
        float peak = 0.0f; /* 当前频点区间内的最大线性幅度。 */
        float dbfs;        /* 当前区间峰值换算后的 dBFS。 */

        if (end_bin <= begin_bin) {
            end_bin = begin_bin + 1U;
        }
        if (end_bin > bin_count) {
            end_bin = bin_count;
        }

        for (bin_index = begin_bin; bin_index < end_bin; bin_index++) {
            if (magnitudes[bin_index] > peak) {
                peak = magnitudes[bin_index];
            }
        }

        if (peak <= SIGNAL_PROCESS_MIN_MAGNITUDE) {
            destination[point_index] = 0;
            continue;
        }

        dbfs = 20.0f * log10f(
            peak / SIGNAL_PROCESS_ADC_FULL_SCALE_PEAK);
        if (dbfs < -(float)SIGNAL_PROCESS_SPECTRUM_FLOOR_DB) {
            dbfs = -(float)SIGNAL_PROCESS_SPECTRUM_FLOOR_DB;
        }
        if (dbfs > 0.0f) {
            dbfs = 0.0f;
        }
        destination[point_index] = (int32_t)(
            dbfs + (float)SIGNAL_PROCESS_SPECTRUM_FLOOR_DB + 0.5f);
    }

    return 1U;
}
