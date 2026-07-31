#!/usr/bin/env python3
"""汇总扫频原始数据，并从实测曲线选择分段线性补偿节点。"""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", newline="", encoding="utf-8-sig") as stream:
        return list(csv.DictReader(stream))


def median_smooth(values: list[float], window: int) -> list[float]:
    """用奇数窗中位数抑制单点量化抖动，不改变频响整体趋势。"""
    radius = window // 2
    smoothed: list[float] = []
    for index in range(len(values)):
        first = max(0, index - radius)
        last = min(len(values), index + radius + 1)
        smoothed.append(statistics.median(values[first:last]))
    return smoothed


def interpolate_nodes(
    frequencies: list[int],
    node_indices: list[int],
    node_gains: list[float],
) -> list[float]:
    """按固件同样的频率线性插值规则计算每个频点增益。"""
    result = [0.0] * len(frequencies)
    ordered = sorted(zip(node_indices, node_gains))
    for segment in range(1, len(ordered)):
        lower_index, lower_gain = ordered[segment - 1]
        upper_index, upper_gain = ordered[segment]
        lower_frequency = frequencies[lower_index]
        upper_frequency = frequencies[upper_index]
        span = upper_frequency - lower_frequency
        for index in range(lower_index, upper_index + 1):
            ratio = (frequencies[index] - lower_frequency) / span
            result[index] = lower_gain + (upper_gain - lower_gain) * ratio
    return result


def select_nodes(
    frequencies: list[int],
    raw_peaks_mv: list[float],
    target_peak_mv: float,
    smoothed_gains: list[float],
    target_error_mv: float,
    max_nodes: int,
) -> tuple[list[int], list[float], list[float]]:
    """迭代加入当前校正误差最大的频点，直至达到目标或节点上限。"""
    selected = [0, len(frequencies) - 1]
    while True:
        selected.sort()
        selected_gains = [smoothed_gains[index] for index in selected]
        interpolated = interpolate_nodes(frequencies, selected, selected_gains)
        errors = [
            raw_peak * gain - target_peak_mv
            for raw_peak, gain in zip(raw_peaks_mv, interpolated)
        ]
        worst_index = max(range(len(errors)), key=lambda i: abs(errors[i]))
        if abs(errors[worst_index]) <= target_error_mv:
            return selected, interpolated, errors
        if len(selected) >= max_nodes:
            return selected, interpolated, errors
        if worst_index in selected:
            # 平滑曲线在节点处仍可能留下微小残差，改用该频点实测增益。
            smoothed_gains[worst_index] = target_peak_mv / raw_peaks_mv[worst_index]
        else:
            selected.append(worst_index)


def percentile(values: list[float], percent: float) -> float:
    ordered = sorted(values)
    position = (len(ordered) - 1) * percent
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    ratio = position - lower
    return ordered[lower] * (1.0 - ratio) + ordered[upper] * ratio


def summarize_stage(
    rows: list[dict[str, str]],
    stage: str,
    start: int,
    stop: int,
    step: int,
    repeats: int,
) -> list[dict[str, float | int]]:
    grouped: dict[int, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        if row["stage"] == stage:
            grouped[int(row["target_frequency_hz"])].append(row)

    expected_frequencies = list(range(start, stop + 1, step))
    missing = [frequency for frequency in expected_frequencies if frequency not in grouped]
    bad_counts = {
        frequency: len(grouped[frequency])
        for frequency in expected_frequencies
        if frequency in grouped and len(grouped[frequency]) != repeats
    }
    if missing or bad_counts:
        raise ValueError(
            f"{stage}数据不完整：缺失频点={missing[:10]}，重复数异常={bad_counts}"
        )

    summary: list[dict[str, float | int]] = []
    for frequency in expected_frequencies:
        samples = grouped[frequency]
        raw_peaks = [float(row["raw_peak_mv"]) for row in samples]
        measured_peaks = [float(row["firmware_peak_mv"]) for row in samples]
        measured_vpp = [float(row["firmware_vpp_mv"]) for row in samples]
        measured_rms = [float(row["firmware_rms_mv"]) for row in samples]
        frequency_errors = [float(row["frequency_error_hz"]) for row in samples]
        summary.append(
            {
                "frequency_hz": frequency,
                "raw_peak_mean_mv": statistics.fmean(raw_peaks),
                "raw_peak_std_mv": statistics.stdev(raw_peaks),
                "required_gain": 100.0 / statistics.fmean(raw_peaks),
                "measured_peak_mean_mv": statistics.fmean(measured_peaks),
                "measured_peak_error_mv": statistics.fmean(measured_peaks) - 100.0,
                "measured_vpp_mean_mv": statistics.fmean(measured_vpp),
                "measured_vpp_error_mv": statistics.fmean(measured_vpp) - 200.0,
                "measured_rms_mean_mv": statistics.fmean(measured_rms),
                "measured_rms_error_mv": (
                    statistics.fmean(measured_rms) - 100.0 / math.sqrt(2.0)
                ),
                "frequency_error_mean_hz": statistics.fmean(frequency_errors),
                "frequency_error_max_abs_hz": max(map(abs, frequency_errors)),
                "adc_overrun_max": max(int(row["adc_overrun"]) for row in samples),
            }
        )
    return summary


def write_frequency_summary(
    path: Path,
    stage_summaries: dict[str, list[dict[str, float | int]]],
) -> None:
    """写出每频点汇总，供工作簿和独立复核共同使用。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    stages = list(stage_summaries)
    base = stage_summaries[stages[0]]
    fieldnames = ["frequency_hz"]
    metric_names = [name for name in base[0] if name != "frequency_hz"]
    for stage in stages:
        fieldnames.extend(f"{stage}_{name}" for name in metric_names)

    with path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        for index, item in enumerate(base):
            row: dict[str, float | int] = {"frequency_hz": item["frequency_hz"]}
            for stage in stages:
                for name in metric_names:
                    row[f"{stage}_{name}"] = stage_summaries[stage][index][name]
            writer.writerow(row)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input",
        default=(
            "outputs/019fb655-4b94-7fc2-a7a5-46aa763fc330/"
            "frequency_sweep_raw.csv"
        ),
    )
    parser.add_argument(
        "--output-json",
        default=(
            "outputs/019fb655-4b94-7fc2-a7a5-46aa763fc330/"
            "frequency_sweep_analysis.json"
        ),
    )
    parser.add_argument(
        "--output-summary",
        default=(
            "outputs/019fb655-4b94-7fc2-a7a5-46aa763fc330/"
            "frequency_sweep_summary.csv"
        ),
    )
    parser.add_argument("--start", type=int, default=10_000)
    parser.add_argument("--stop", type=int, default=500_000)
    parser.add_argument("--step", type=int, default=500)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--stage", default="before")
    parser.add_argument("--smooth-window", type=int, default=9)
    parser.add_argument("--target-peak-error-mv", type=float, default=0.35)
    parser.add_argument("--max-nodes", type=int, default=32)
    args = parser.parse_args()

    rows = load_rows(Path(args.input))
    available_stages = sorted({row["stage"] for row in rows})
    summaries = {
        stage: summarize_stage(
            rows,
            stage,
            args.start,
            args.stop,
            args.step,
            args.repeats,
        )
        for stage in available_stages
    }
    write_frequency_summary(Path(args.output_summary), summaries)

    selected_summary = summaries[args.stage]
    frequencies = [int(item["frequency_hz"]) for item in selected_summary]
    raw_peaks = [float(item["raw_peak_mean_mv"]) for item in selected_summary]
    required_gains = [float(item["required_gain"]) for item in selected_summary]
    smoothed_gains = median_smooth(required_gains, args.smooth_window)
    node_indices, interpolated_gains, predicted_errors = select_nodes(
        frequencies,
        raw_peaks,
        100.0,
        smoothed_gains,
        args.target_peak_error_mv,
        args.max_nodes,
    )

    stage_metrics: dict[str, dict[str, float | int]] = {}
    for stage, summary in summaries.items():
        peak_errors = [abs(float(item["measured_peak_error_mv"])) for item in summary]
        vpp_errors = [abs(float(item["measured_vpp_error_mv"])) for item in summary]
        rms_errors = [abs(float(item["measured_rms_error_mv"])) for item in summary]
        frequency_errors = [
            abs(float(item["frequency_error_max_abs_hz"])) for item in summary
        ]
        stage_metrics[stage] = {
            "row_count": sum(1 for row in rows if row["stage"] == stage),
            "frequency_point_count": len(summary),
            "max_abs_peak_error_mv": max(peak_errors),
            "p95_abs_peak_error_mv": percentile(peak_errors, 0.95),
            "max_abs_vpp_error_mv": max(vpp_errors),
            "p95_abs_vpp_error_mv": percentile(vpp_errors, 0.95),
            "max_abs_rms_error_mv": max(rms_errors),
            "max_abs_frequency_error_hz": max(frequency_errors),
            "max_adc_overrun": max(int(item["adc_overrun_max"]) for item in summary),
        }

    analysis = {
        "input": str(Path(args.input).resolve()),
        "grid": {
            "start_hz": args.start,
            "stop_hz": args.stop,
            "step_hz": args.step,
            "repeats": args.repeats,
            "expected_peak_mv": 100.0,
            "expected_vpp_mv": 200.0,
            "expected_rms_mv": 100.0 / math.sqrt(2.0),
        },
        "stage_metrics": stage_metrics,
        "compensation": {
            "source_stage": args.stage,
            "smoothing": f"median window {args.smooth_window}",
            "target_peak_error_mv": args.target_peak_error_mv,
            "max_nodes": args.max_nodes,
            "selected_node_count": len(node_indices),
            "predicted_max_abs_peak_error_mv": max(map(abs, predicted_errors)),
            "predicted_p95_abs_peak_error_mv": percentile(
                list(map(abs, predicted_errors)), 0.95
            ),
            "nodes": [
                {
                    "frequency_hz": frequencies[index],
                    "correction_gain": interpolated_gains[index],
                }
                for index in node_indices
            ],
        },
    }

    output_json = Path(args.output_json)
    output_json.parent.mkdir(parents=True, exist_ok=True)
    output_json.write_text(
        json.dumps(analysis, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    print(json.dumps(analysis, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
