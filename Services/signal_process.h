#ifndef SIGNAL_PROCESS_H
#define SIGNAL_PROCESS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define SIGNAL_PROCESS_SPECTRUM_FLOOR_DB 80U

/** @brief 将时域采样等间隔抽取为显示快照。 */
uint8_t SignalProcess_BuildTimeSnapshot(const uint16_t *source,
                                        uint32_t source_count,
                                        int32_t *destination,
                                        uint16_t destination_count);

/**
 * @brief 将单边幅度谱按区间峰值压缩为 0 至 80 dBFS 显示快照。
 * @note 输出 0 表示 -80 dBFS 或更低，80 表示 0 dBFS。
 */
uint8_t SignalProcess_BuildSpectrumSnapshot(const float *magnitudes,
                                            uint16_t bin_count,
                                            int32_t *destination,
                                            uint16_t destination_count);

#ifdef __cplusplus
}
#endif

#endif /* SIGNAL_PROCESS_H */
