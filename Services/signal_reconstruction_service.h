#ifndef SIGNAL_RECONSTRUCTION_SERVICE_H
#define SIGNAL_RECONSTRUCTION_SERVICE_H

#include <stdint.h>

#include "signal_measurement_service.h"

/**
 * @brief 根据带内谐波分量重构零相位周期波形。
 * @param measurement 已完成频响补偿的带内测量结果。
 * @param cycle_count 输出包含的完整周期数。
 * @param samples_mv 可选输出数组；为 NULL 时只计算极值。
 * @param point_count 重构点数，必须大于 1。
 * @param minimum_mv 输出最小值。
 * @param maximum_mv 输出最大值。
 * @retval 1 重构成功；0 参数或测量结果无效。
 */
uint8_t Signal_Reconstruction_Service_Generate(
    const signal_measurement_result_t *measurement,
    uint8_t cycle_count,
    float *samples_mv,
    uint32_t point_count,
    float *minimum_mv,
    float *maximum_mv);

#endif
