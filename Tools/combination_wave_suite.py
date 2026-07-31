#!/usr/bin/env python3
"""批量生成并实测零相位“基波 + 1/2 个谐波”组合波。

每个函数固定谐波阶次和幅值比例，再测试 3 组不同的基频与总峰峰值。
测试结果逐例追加到 CSV，支持断点续测；JSON 汇总用于后续算法分析。
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import statistics
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

import serial

from arb_expression_test import make_bsv, parse_telemetry
from frequency_sweep import (
    DEFAULT_BAUD,
    DEFAULT_PORT,
    DEFAULT_RESOURCE,
    VisaInstrument,
)


FREQUENCY_MIN_HZ = 50_000
FREQUENCY_MAX_HZ = 500_000
FREQUENCY_GRID_HZ = 500
VPP_MIN_MV = 50.0
VPP_MAX_MV = 250.0
VOLTAGE_ERROR_LIMIT_MV = 5.0
FREQUENCY_ERROR_LIMIT_HZ = 1_000.0
POINT_COUNT = 1_000
VARIANT_COUNT = 3
CSV_FIELDS = [
    "stage",
    "timestamp",
    "function_id",
    "case_id",
    "variant_index",
    "expression",
    "term_count",
    "orders",
    "coefficients",
    "fundamental_hz",
    "overall_vpp_mv",
    "offset_v",
    "phase_deg",
    "expected_upp_mv",
    "measured_upp_mv",
    "upp_error_mv",
    "expected_rms_mv",
    "measured_rms_mv",
    "rms_error_mv",
    "expected_h1_order",
    "expected_h1_frequency_hz",
    "measured_h1_frequency_hz",
    "h1_frequency_error_hz",
    "expected_h1_peak_mv",
    "measured_h1_peak_mv",
    "h1_peak_error_mv",
    "expected_h2_order",
    "expected_h2_frequency_hz",
    "measured_h2_frequency_hz",
    "h2_frequency_error_hz",
    "expected_h2_peak_mv",
    "measured_h2_peak_mv",
    "h2_peak_error_mv",
    "expected_h3_order",
    "expected_h3_frequency_hz",
    "measured_h3_frequency_hz",
    "h3_frequency_error_hz",
    "expected_h3_peak_mv",
    "measured_h3_peak_mv",
    "h3_peak_error_mv",
    "measured_component_count",
    "signal_valid",
    "peak_count",
    "adc_overrun_max",
    "stable_frame_count",
    "strict_frequency_match",
    "max_abs_voltage_error_mv",
    "max_abs_frequency_error_hz",
    "status",
    "error_message",
]


@dataclass(frozen=True)
class FunctionSpec:
    """固定谐波阶次和幅值比例的一个函数。"""

    function_id: int
    terms: tuple[tuple[int, float], ...]


@dataclass(frozen=True)
class TestCase:
    """一个函数在指定基频和峰峰值下的实机用例。"""

    function: FunctionSpec
    variant_index: int
    fundamental_hz: int
    overall_vpp_mv: float

    @property
    def case_id(self) -> str:
        return f"F{self.function.function_id:04d}_V{self.variant_index}"


def _expression(terms: tuple[tuple[int, float], ...]) -> str:
    return "+".join(
        f"{coefficient:.3f}*sin(2*pi*{order}*f*t)"
        for order, coefficient in terms
    )


def _triple_ratio_pairs() -> list[tuple[float, float]]:
    """给出均衡且唯一的三分量比例，避免低幅用例的分量低于 5 mV。"""
    values = [0.60 + 0.05 * index for index in range(17)]
    pairs: list[tuple[float, float]] = []
    for diagonal in range(len(values)):
        for first in range(len(values)):
            second = (first * 5 + diagonal * 7) % len(values)
            pairs.append((values[first], values[second]))
    return pairs


def generate_functions(function_count: int) -> list[FunctionSpec]:
    """确定性生成双分量和三分量函数，便于复测时完全复现。"""
    if function_count < 1:
        raise ValueError("function_count 必须为正数")

    dual_target = function_count // 2
    triple_target = function_count - dual_target
    functions: list[FunctionSpec] = []
    dual_orders = list(range(2, 10))

    for index in range(dual_target):
        order = dual_orders[index % len(dual_orders)]
        ratio_level = index // len(dual_orders)
        ratio = 0.35 + 0.05 * ratio_level
        functions.append(
            FunctionSpec(len(functions) + 1, ((1, 1.0), (order, ratio)))
        )

    triple_orders = [
        (first, second)
        for first in range(2, 10)
        for second in range(first + 1, 10)
    ]
    ratio_pairs = _triple_ratio_pairs()
    for index in range(triple_target):
        order_pair = triple_orders[index % len(triple_orders)]
        ratio_pair = ratio_pairs[index // len(triple_orders)]
        functions.append(
            FunctionSpec(
                len(functions) + 1,
                (
                    (1, 1.0),
                    (order_pair[0], ratio_pair[0]),
                    (order_pair[1], ratio_pair[1]),
                ),
            )
        )

    unique = {(item.terms, _expression(item.terms)) for item in functions}
    if len(unique) != len(functions):
        raise RuntimeError("函数生成器产生了重复表达式")
    return functions


def _frequency_variants(function_id: int, max_order: int) -> tuple[int, int, int]:
    """在允许区间的低、中、高三段各选一个 500 Hz 栅格频率。"""
    maximum_fundamental = (
        (FREQUENCY_MAX_HZ // max_order) // FREQUENCY_GRID_HZ
    ) * FREQUENCY_GRID_HZ
    span_steps = (
        maximum_fundamental - FREQUENCY_MIN_HZ
    ) // FREQUENCY_GRID_HZ
    if span_steps < 2:
        raise ValueError(f"{max_order} 次谐波无法生成 3 个不同基频")

    fractions = (
        ((function_id * 7) % 21) / 100.0,
        0.40 + ((function_id * 11) % 21) / 100.0,
        0.80 + ((function_id * 13) % 21) / 100.0,
    )
    steps = [round(span_steps * value) for value in fractions]
    steps[1] = max(steps[1], steps[0] + 1)
    steps[2] = max(steps[2], steps[1] + 1)
    steps[2] = min(steps[2], span_steps)
    steps[1] = min(steps[1], steps[2] - 1)
    steps[0] = min(steps[0], steps[1] - 1)
    return tuple(
        FREQUENCY_MIN_HZ + step * FREQUENCY_GRID_HZ for step in steps
    )


def _vpp_variants(function: FunctionSpec, raw_vpp: float) -> tuple[float, float, float]:
    """覆盖低、中、高幅值，并保证最弱理论分量不低于 5 mV。"""
    function_id = function.function_id
    minimum_coefficient = min(abs(item[1]) for item in function.terms)
    minimum_required_vpp = math.ceil(
        VOLTAGE_ERROR_LIMIT_MV * raw_vpp / minimum_coefficient
    )
    low_floor = 50 if len(function.terms) == 2 else 70
    low = max(low_floor + (function_id * 7) % 31, minimum_required_vpp)
    middle = 125 + (function_id * 11) % 51
    high = 200 + (function_id * 13) % 51
    if low > 110 or not (low < middle < high <= VPP_MAX_MV):
        raise ValueError(
            f"F{function_id:04d} 无法生成满足最小分量限制的三档幅值"
        )
    return float(low), float(middle), float(high)


def generate_cases(
    functions: list[FunctionSpec],
) -> tuple[list[TestCase], dict[int, dict[str, float | int]]]:
    """为每个函数生成三组频率/幅值组合。"""
    cases: list[TestCase] = []
    bsv_information: dict[int, dict[str, float | int]] = {}
    for function in functions:
        _, bsv_info = make_bsv(list(function.terms), POINT_COUNT)
        bsv_information[function.function_id] = bsv_info
        frequencies = _frequency_variants(
            function.function_id, max(item[0] for item in function.terms)
        )
        amplitudes = _vpp_variants(function, float(bsv_info["raw_vpp"]))
        for variant_index, (frequency_hz, vpp_mv) in enumerate(
            zip(frequencies, amplitudes), start=1
        ):
            cases.append(
                TestCase(function, variant_index, frequency_hz, vpp_mv)
            )
    return cases, bsv_information


def expected_values(
    case: TestCase,
    bsv_info: dict[str, float | int],
) -> dict:
    """按零相位谐波表达式计算题目真实值。"""
    mv_per_unit = case.overall_vpp_mv / float(bsv_info["raw_vpp"])
    components = [
        {
            "order": order,
            "frequency_hz": float(order * case.fundamental_hz),
            "peak_mv": abs(coefficient) * mv_per_unit,
        }
        for order, coefficient in case.function.terms
    ]
    return {
        "upp_mv": case.overall_vpp_mv,
        "rms_mv": mv_per_unit
        * math.sqrt(
            sum(value * value for _, value in case.function.terms) / 2.0
        ),
        "components": components,
    }


def validate_suite(
    functions: list[FunctionSpec],
    cases: list[TestCase],
    bsv_information: dict[int, dict[str, float | int]],
) -> None:
    """在接触硬件前完整校验函数唯一性和题目边界。"""
    if len(cases) != len(functions) * VARIANT_COUNT:
        raise RuntimeError("每个函数必须恰好生成 3 个用例")
    if len({_expression(item.terms) for item in functions}) != len(functions):
        raise RuntimeError("函数表达式不唯一")

    for function in functions:
        function_cases = [item for item in cases if item.function == function]
        if len({item.fundamental_hz for item in function_cases}) != VARIANT_COUNT:
            raise RuntimeError(f"F{function.function_id:04d} 的基频不唯一")
        if len({item.overall_vpp_mv for item in function_cases}) != VARIANT_COUNT:
            raise RuntimeError(f"F{function.function_id:04d} 的幅值不唯一")
        for case in function_cases:
            expected = expected_values(
                case, bsv_information[function.function_id]
            )
            frequencies = [
                item["frequency_hz"] for item in expected["components"]
            ]
            peaks = [item["peak_mv"] for item in expected["components"]]
            if not all(
                FREQUENCY_MIN_HZ <= value <= FREQUENCY_MAX_HZ
                for value in frequencies
            ):
                raise RuntimeError(f"{case.case_id} 的分量频率越界")
            if not (VPP_MIN_MV <= case.overall_vpp_mv <= VPP_MAX_MV):
                raise RuntimeError(f"{case.case_id} 的峰峰值越界")
            if min(peaks) < VOLTAGE_ERROR_LIMIT_MV:
                raise RuntimeError(
                    f"{case.case_id} 最弱分量仅 {min(peaks):.3f} mV"
                )


def write_function_manifest(
    path: Path,
    functions: list[FunctionSpec],
    cases: list[TestCase],
    bsv_information: dict[int, dict[str, float | int]],
) -> None:
    """保存可审计且可复现的函数库。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    case_map = {
        item.function_id: [
            case for case in cases if case.function == item
        ]
        for item in functions
    }
    fields = [
        "function_id",
        "expression",
        "term_count",
        "orders",
        "coefficients",
        "raw_vpp_units",
        "variant_1_fundamental_hz",
        "variant_1_vpp_mv",
        "variant_2_fundamental_hz",
        "variant_2_vpp_mv",
        "variant_3_fundamental_hz",
        "variant_3_vpp_mv",
    ]
    with path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for function in functions:
            function_cases = case_map[function.function_id]
            writer.writerow(
                {
                    "function_id": f"F{function.function_id:04d}",
                    "expression": _expression(function.terms),
                    "term_count": len(function.terms),
                    "orders": ",".join(str(item[0]) for item in function.terms),
                    "coefficients": ",".join(
                        f"{item[1]:.3f}" for item in function.terms
                    ),
                    "raw_vpp_units": bsv_information[function.function_id][
                        "raw_vpp"
                    ],
                    **{
                        f"variant_{case.variant_index}_fundamental_hz": (
                            case.fundamental_hz
                        )
                        for case in function_cases
                    },
                    **{
                        f"variant_{case.variant_index}_vpp_mv": (
                            case.overall_vpp_mv
                        )
                        for case in function_cases
                    },
                }
            )


def _configure_common(instrument: VisaInstrument) -> None:
    commands = (
        "CHANnel2:LOAD 50",
        "CHANnel2:MODe CONTinue",
        "CHANnel2:BASE:WAVe DC",
        "CHANnel2:BASE:OFFSet 0",
        "CHANnel2:OUTPut ON",
        "CHANnel1:LOAD 50",
        "CHANnel1:MODe CONTinue",
        "CHANnel1:BASE:WAVe ARB",
        "CHANnel1:BASE:OFFSet 0",
        "CHANnel1:OUTPut ON",
        "SYSTEM:LOCK OFF",
    )
    for command in commands:
        instrument.write_bytes((command + "\r\n").encode("ascii"), command)
        time.sleep(0.015)
    for channel in (1, 2):
        load_ohm = float(instrument.query(f"CHANnel{channel}:LOAD?"))
        if abs(load_ohm - 50.0) > 0.01:
            raise RuntimeError(f"CH{channel} 负载不是 50 Ω：{load_ohm}")


def upload_function(
    instrument: VisaInstrument,
    function: FunctionSpec,
    upload_wait_s: float,
) -> dict[str, float | int]:
    """交替覆盖两个短文件名，避免任意波文件累计占用仪器存储。"""
    bsv, bsv_info = make_bsv(list(function.terms), POINT_COUNT)
    filename = f"cb{function.function_id % 2}.bsv"
    instrument.write_bytes(
        f'WARB1:CARRier "{filename}"\r\n'.encode("ascii"),
        f"upload {filename}",
    )
    instrument.write_bytes(bsv, f"payload {filename}")
    time.sleep(upload_wait_s)
    instrument.write_bytes(b"CHANnel1:BASE:WAVe ARB\r\n", "select ARB")
    return bsv_info


def configure_case(instrument: VisaInstrument, case: TestCase) -> None:
    commands = (
        f"CHANnel1:BASE:FREQuency {case.fundamental_hz}",
        f"CHANnel1:BASE:AMPLitude {case.overall_vpp_mv / 1000.0:.9f}",
        "CHANnel1:BASE:OFFSet 0",
        "CHANnel1:OUTPut ON",
    )
    for command in commands:
        instrument.write_bytes((command + "\r\n").encode("ascii"), command)
        time.sleep(0.012)


def _extract_pair(
    pending: dict[str, float], diagnostic: dict[str, float]
) -> dict[str, float]:
    return {**pending, **diagnostic}


def collect_case_frames(
    port: serial.Serial,
    expected_frequencies: list[float],
    frame_count: int,
    timeout_s: float,
) -> tuple[list[dict[str, float]], list[str], bool]:
    """优先返回严格匹配帧；超时后保留当前波形的稳定帧供失败分析。"""
    deadline = time.monotonic() + timeout_s
    strict_frames: list[dict[str, float]] = []
    fallback_frames: list[dict[str, float]] = []
    evidence: list[str] = []
    pending: dict[str, float] | None = None
    pending_line = ""

    while time.monotonic() < deadline and len(strict_frames) < frame_count:
        line = port.readline().decode("ascii", "replace").strip()
        if line.startswith("Upp_mV:"):
            pending = parse_telemetry(line)
            pending_line = line
            continue
        if not line.startswith("seq:") or pending is None:
            continue

        diagnostic = parse_telemetry(line)
        if int(diagnostic.get("avg_count", 0)) < 9:
            pending = None
            continue
        pair = _extract_pair(pending, diagnostic)
        fallback_frames.append(pair)
        measured_frequencies = [
            pending.get(f"f{index}_Hz", 0.0)
            for index in range(1, len(expected_frequencies) + 1)
        ]
        strict = (
            int(pending.get("component_count", 0)) == len(expected_frequencies)
            and all(
                abs(measured - expected) <= FREQUENCY_ERROR_LIMIT_HZ
                for measured, expected in zip(
                    measured_frequencies, expected_frequencies
                )
            )
        )
        if strict:
            strict_frames.append(pair)
            evidence.extend((pending_line, line))
        pending = None

    if len(strict_frames) == frame_count:
        return strict_frames, evidence[-6:], True
    if fallback_frames:
        selected = fallback_frames[-frame_count:]
        return selected, evidence[-6:], False
    raise TimeoutError("未收到 avg_count >= 9 的稳定串口帧")


def median_measurement(frames: list[dict[str, float]]) -> dict:
    """按谐波阶次聚合三帧，避免缺失分量造成位置错配。"""
    result = {
        "upp_mv": statistics.median(item.get("Upp_mV", 0.0) for item in frames),
        "rms_mv": statistics.median(item.get("U_mV", 0.0) for item in frames),
        "component_count": int(
            statistics.median(item.get("component_count", 0.0) for item in frames)
        ),
        "signal_valid": int(
            statistics.median(item.get("signal_valid", 0.0) for item in frames)
        ),
        "peak_count": int(
            statistics.median(item.get("peak_count", 0.0) for item in frames)
        ),
        "adc_overrun_max": max(int(item.get("adc_overrun", 1)) for item in frames),
        "components": {},
    }
    orders = {
        int(frame.get(f"n{index}", 0))
        for frame in frames
        for index in range(1, 4)
        if int(frame.get(f"n{index}", 0)) > 0
    }
    for order in orders:
        frequencies: list[float] = []
        amplitudes: list[float] = []
        for frame in frames:
            for index in range(1, 4):
                if int(frame.get(f"n{index}", 0)) == order:
                    frequencies.append(frame.get(f"f{index}_Hz", 0.0))
                    amplitudes.append(frame.get(f"U{index}_peak_mV", 0.0))
        if frequencies:
            result["components"][order] = {
                "frequency_hz": statistics.median(frequencies),
                "peak_mv": statistics.median(amplitudes),
            }
    return result


def result_row(
    stage: str,
    case: TestCase,
    expected: dict,
    measured: dict | None,
    frame_count: int,
    strict_match: bool,
    error_message: str,
) -> dict:
    """生成一行完整、扁平、便于 XML/Excel 导入的测试结果。"""
    row: dict[str, str | int | float] = {
        field: "" for field in CSV_FIELDS
    }
    row.update(
        {
            "stage": stage,
            "timestamp": datetime.now().isoformat(timespec="milliseconds"),
            "function_id": f"F{case.function.function_id:04d}",
            "case_id": case.case_id,
            "variant_index": case.variant_index,
            "expression": _expression(case.function.terms),
            "term_count": len(case.function.terms),
            "orders": ",".join(str(item[0]) for item in case.function.terms),
            "coefficients": ",".join(
                f"{item[1]:.3f}" for item in case.function.terms
            ),
            "fundamental_hz": case.fundamental_hz,
            "overall_vpp_mv": case.overall_vpp_mv,
            "offset_v": 0.0,
            "phase_deg": 0.0,
            "expected_upp_mv": expected["upp_mv"],
            "expected_rms_mv": expected["rms_mv"],
            "stable_frame_count": frame_count,
            "strict_frequency_match": int(strict_match),
            "error_message": error_message,
        }
    )
    for index, component in enumerate(expected["components"], start=1):
        row[f"expected_h{index}_order"] = component["order"]
        row[f"expected_h{index}_frequency_hz"] = component["frequency_hz"]
        row[f"expected_h{index}_peak_mv"] = component["peak_mv"]

    if measured is None:
        row["status"] = "ERROR"
        return row

    row.update(
        {
            "measured_upp_mv": measured["upp_mv"],
            "upp_error_mv": measured["upp_mv"] - expected["upp_mv"],
            "measured_rms_mv": measured["rms_mv"],
            "rms_error_mv": measured["rms_mv"] - expected["rms_mv"],
            "measured_component_count": measured["component_count"],
            "signal_valid": measured["signal_valid"],
            "peak_count": measured["peak_count"],
            "adc_overrun_max": measured["adc_overrun_max"],
        }
    )
    voltage_errors = [abs(float(row["upp_error_mv"])), abs(float(row["rms_error_mv"]))]
    frequency_errors: list[float] = []
    components_passed = True
    for index, component in enumerate(expected["components"], start=1):
        actual = measured["components"].get(component["order"])
        if actual is None:
            components_passed = False
            continue
        frequency_error = actual["frequency_hz"] - component["frequency_hz"]
        peak_error = actual["peak_mv"] - component["peak_mv"]
        row[f"measured_h{index}_frequency_hz"] = actual["frequency_hz"]
        row[f"h{index}_frequency_error_hz"] = frequency_error
        row[f"measured_h{index}_peak_mv"] = actual["peak_mv"]
        row[f"h{index}_peak_error_mv"] = peak_error
        frequency_errors.append(abs(frequency_error))
        voltage_errors.append(abs(peak_error))
        components_passed = components_passed and (
            abs(frequency_error) <= FREQUENCY_ERROR_LIMIT_HZ
            and abs(peak_error) <= VOLTAGE_ERROR_LIMIT_MV
        )

    row["max_abs_voltage_error_mv"] = max(voltage_errors)
    row["max_abs_frequency_error_hz"] = max(frequency_errors, default=0.0)
    passed = (
        abs(float(row["upp_error_mv"])) <= VOLTAGE_ERROR_LIMIT_MV
        and abs(float(row["rms_error_mv"])) <= VOLTAGE_ERROR_LIMIT_MV
        and components_passed
        and measured["component_count"] == len(expected["components"])
        and measured["signal_valid"] == 1
        and measured["adc_overrun_max"] == 0
    )
    row["status"] = "PASS" if passed else "FAIL"
    return row


def append_row(path: Path, row: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    write_header = not path.exists() or path.stat().st_size == 0
    with path.open("a", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=CSV_FIELDS)
        if write_header:
            writer.writeheader()
        writer.writerow(row)
        stream.flush()


def load_completed(path: Path, stage: str) -> set[str]:
    """只跳过已经通过的用例，FAIL/ERROR 在恢复运行时自动重试。"""
    if not path.exists():
        return set()
    with path.open("r", newline="", encoding="utf-8-sig") as stream:
        return {
            row["case_id"]
            for row in csv.DictReader(stream)
            if row["stage"] == stage and row["status"] == "PASS"
        }


def write_summary(path: Path, csv_path: Path, stage: str, expected_count: int) -> dict:
    rows: list[dict[str, str]] = []
    if csv_path.exists():
        with csv_path.open("r", newline="", encoding="utf-8-sig") as stream:
            rows = [row for row in csv.DictReader(stream) if row["stage"] == stage]
    latest: dict[str, dict[str, str]] = {}
    for row in rows:
        latest[row["case_id"]] = row
    values = list(latest.values())
    voltage_errors = [
        float(row["max_abs_voltage_error_mv"])
        for row in values
        if row["max_abs_voltage_error_mv"]
    ]
    frequency_errors = [
        float(row["max_abs_frequency_error_hz"])
        for row in values
        if row["max_abs_frequency_error_hz"]
    ]
    summary = {
        "stage": stage,
        "expected_case_count": expected_count,
        "completed_case_count": len(values),
        "pass_count": sum(row["status"] == "PASS" for row in values),
        "fail_count": sum(row["status"] == "FAIL" for row in values),
        "error_count": sum(row["status"] == "ERROR" for row in values),
        "pass_rate": (
            sum(row["status"] == "PASS" for row in values) / len(values)
            if values else 0.0
        ),
        "max_abs_voltage_error_mv": max(voltage_errors, default=0.0),
        "max_abs_frequency_error_hz": max(frequency_errors, default=0.0),
        "adc_overrun_max": max(
            (int(row["adc_overrun_max"]) for row in values if row["adc_overrun_max"]),
            default=0,
        ),
        "all_passed": (
            len(values) == expected_count
            and all(row["status"] == "PASS" for row in values)
        ),
    }
    path.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--function-count", type=int, default=500)
    parser.add_argument("--stage", default="combination_after_calibration")
    parser.add_argument("--frames", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=3.5)
    parser.add_argument("--discard-s", type=float, default=0.65)
    parser.add_argument("--upload-wait-s", type=float, default=0.60)
    parser.add_argument("--start-function", type=int, default=1)
    parser.add_argument("--stop-function", type=int, default=0)
    parser.add_argument("--progress-every", type=int, default=25)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--resource", default=DEFAULT_RESOURCE)
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument(
        "--output-dir", default="outputs/20260731_combination_suite"
    )
    args = parser.parse_args()

    output_dir = Path(args.output_dir).resolve()
    csv_path = output_dir / "combination_wave_results.csv"
    functions = generate_functions(args.function_count)
    cases, bsv_information = generate_cases(functions)
    validate_suite(functions, cases, bsv_information)
    write_function_manifest(
        output_dir / "combination_wave_functions.csv",
        functions,
        cases,
        bsv_information,
    )
    print(
        f"SUITE_VALID functions={len(functions)} cases={len(cases)} "
        f"frequency={FREQUENCY_MIN_HZ}-{FREQUENCY_MAX_HZ}Hz "
        f"vpp={VPP_MIN_MV:.0f}-{VPP_MAX_MV:.0f}mV",
        flush=True,
    )
    if args.dry_run:
        return 0

    stop_function = args.stop_function or len(functions)
    selected_functions = [
        item
        for item in functions
        if args.start_function <= item.function_id <= stop_function
    ]
    completed = load_completed(csv_path, args.stage)
    total_pending = sum(
        case.case_id not in completed
        for case in cases
        if case.function in selected_functions
    )
    completed_now = 0
    started = time.monotonic()

    with VisaInstrument(args.resource) as instrument, serial.Serial(
        args.port, args.baud, timeout=0.25
    ) as port:
        identity = instrument.query("*IDN?")
        print(f"INSTRUMENT {identity}", flush=True)
        _configure_common(instrument)
        try:
            for function in selected_functions:
                function_cases = [
                    case
                    for case in cases
                    if case.function == function and case.case_id not in completed
                ]
                if not function_cases:
                    continue
                bsv_info = upload_function(
                    instrument, function, args.upload_wait_s
                )
                for case in function_cases:
                    expected = expected_values(case, bsv_info)
                    configure_case(instrument, case)
                    port.reset_input_buffer()
                    deadline = time.monotonic() + args.discard_s
                    while time.monotonic() < deadline:
                        port.read(port.in_waiting or 1)
                    measured = None
                    strict_match = False
                    frames: list[dict[str, float]] = []
                    error_message = ""
                    try:
                        frames, _, strict_match = collect_case_frames(
                            port,
                            [
                                item["frequency_hz"]
                                for item in expected["components"]
                            ],
                            args.frames,
                            args.timeout,
                        )
                        measured = median_measurement(frames)
                    except Exception as exc:
                        error_message = str(exc)
                    row = result_row(
                        args.stage,
                        case,
                        expected,
                        measured,
                        len(frames),
                        strict_match,
                        error_message,
                    )
                    append_row(csv_path, row)
                    completed_now += 1
                    if (
                        completed_now % args.progress_every == 0
                        or completed_now == total_pending
                        or row["status"] != "PASS"
                    ):
                        elapsed = time.monotonic() - started
                        print(
                            f"CASE_DONE {completed_now}/{total_pending} "
                            f"{case.case_id} status={row['status']} "
                            f"f0={case.fundamental_hz}Hz "
                            f"vpp={case.overall_vpp_mv:.1f}mV "
                            f"max_u_err={row['max_abs_voltage_error_mv'] or 'NA'} "
                            f"max_f_err={row['max_abs_frequency_error_hz'] or 'NA'} "
                            f"elapsed_s={elapsed:.1f}",
                            flush=True,
                        )
        finally:
            for command in (
                "CHANnel1:OUTPut OFF",
                "CHANnel2:LOAD 50",
                "CHANnel2:BASE:WAVe DC",
                "CHANnel2:BASE:OFFSet 0",
                "CHANnel2:OUTPut ON",
            ):
                instrument.write_bytes(
                    (command + "\r\n").encode("ascii"), command
                )

    summary = write_summary(
        output_dir / f"combination_wave_summary_{args.stage}.json",
        csv_path,
        args.stage,
        len(cases),
    )
    print(json.dumps(summary, ensure_ascii=False), flush=True)
    return 0 if summary["all_passed"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
