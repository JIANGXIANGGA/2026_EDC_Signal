#!/usr/bin/env python3
"""根据250 mVpp单音节点数据计算当前硬件状态的软件标定参数。"""

from __future__ import annotations

import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path


OLD_INPUT_MV_PER_CODE = 0.802920
OLD_RESPONSE = {
    10_000: 1.0232124,
    52_000: 1.0274611,
    110_500: 1.0411755,
    180_000: 1.0696714,
    236_000: 1.1009701,
    294_000: 1.1423390,
    356_000: 1.1974369,
    419_500: 1.2654910,
    500_000: 1.3662787,
}
EXPECTED_PEAK_MV = 125.0


def main() -> int:
    input_path = Path(
        "outputs/019fb655-4b94-7fc2-a7a5-46aa763fc330/"
        "diagnostic_sine_250mV_nodes.csv"
    )
    output_path = Path(
        "outputs/019fb655-4b94-7fc2-a7a5-46aa763fc330/"
        "current_hardware_recalibration.json"
    )
    grouped: dict[int, list[dict[str, str]]] = defaultdict(list)
    with input_path.open("r", newline="", encoding="utf-8-sig") as stream:
        for row in csv.DictReader(stream):
            grouped[int(row["target_frequency_hz"])].append(row)

    points = []
    for frequency_hz, old_gain in OLD_RESPONSE.items():
        rows = grouped[frequency_hz]
        if len(rows) != 3:
            raise ValueError(f"{frequency_hz} Hz需要3帧，实际{len(rows)}帧")
        raw_peak_mv = statistics.fmean(float(row["raw_peak_mv"]) for row in rows)
        measured_peak_mv = statistics.fmean(
            float(row["firmware_peak_mv"]) for row in rows
        )
        required_total_gain_on_old_scale = EXPECTED_PEAK_MV / raw_peak_mv
        points.append(
            {
                "frequency_hz": frequency_hz,
                "raw_peak_mv": raw_peak_mv,
                "measured_peak_mv": measured_peak_mv,
                "old_response_gain": old_gain,
                "required_total_gain_on_old_scale": required_total_gain_on_old_scale,
                "additional_gain": EXPECTED_PEAK_MV / measured_peak_mv,
            }
        )

    low_point = points[0]
    global_gain = (
        low_point["required_total_gain_on_old_scale"]
        / low_point["old_response_gain"]
    )
    new_input_mv_per_code = OLD_INPUT_MV_PER_CODE * global_gain
    for point in points:
        point["new_response_gain"] = (
            point["required_total_gain_on_old_scale"] / global_gain
        )
        point["predicted_peak_mv"] = (
            point["raw_peak_mv"]
            * global_gain
            * point["new_response_gain"]
        )

    result = {
        "expected_peak_mv": EXPECTED_PEAK_MV,
        "old_input_mv_per_code": OLD_INPUT_MV_PER_CODE,
        "global_gain": global_gain,
        "new_input_mv_per_code": new_input_mv_per_code,
        "points": points,
    }
    output_path.write_text(
        json.dumps(result, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
