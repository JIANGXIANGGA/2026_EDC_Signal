#!/usr/bin/env python3
"""执行250 mVpp任意波频率边界、波形和幅值比例自动测试。"""

from __future__ import annotations

import csv
import json
import math
import statistics
import time
from dataclasses import dataclass
from pathlib import Path

import serial

from arb_expression_test import collect_stable_frames, make_bsv
from frequency_sweep import (
    DEFAULT_BAUD,
    DEFAULT_PORT,
    DEFAULT_RESOURCE,
    VisaInstrument,
)


OUTPUT_DIR = Path(
    "outputs/019fb655-4b94-7fc2-a7a5-46aa763fc330"
)
VPP_MV = 250.0
POINT_COUNT = 1000


@dataclass(frozen=True)
class TestCase:
    name: str
    fundamental_hz: float
    terms: tuple[tuple[int, float], ...]
    purpose: str


TEST_CASES = (
    TestCase(
        "T01_low_10k_descending",
        10_000.0,
        ((1, 1.0), (3, 0.5), (5, 0.25)),
        "10 kHz低端，三分量递减幅值",
    ),
    TestCase(
        "T02_low_10k_equal",
        10_000.0,
        ((1, 1.0), (2, 1.0), (3, 1.0)),
        "10 kHz低端，连续三次等幅分量",
    ),
    TestCase(
        "T03_mid_60k_3to1to1",
        60_000.0,
        ((1, 0.6), (3, 0.2), (6, 0.2)),
        "60 kHz，3:1:1比例，最高360 kHz",
    ),
    TestCase(
        "T04_80k_weak_fundamental",
        80_000.0,
        ((1, 0.08), (3, 0.42), (6, 0.50)),
        "弱基波、强高次谐波，最高480 kHz",
    ),
    TestCase(
        "T05_100k_very_weak_fundamental_500k",
        100_000.0,
        ((1, 0.05), (3, 0.45), (5, 0.50)),
        "极弱基波、强谐波，五次谐波正好500 kHz",
    ),
    TestCase(
        "T06_100k_descending_500k",
        100_000.0,
        ((1, 1.0), (2, 0.5), (5, 0.25)),
        "100 kHz递减比例，五次谐波500 kHz",
    ),
    TestCase(
        "T07_125k_equal_500k",
        125_000.0,
        ((1, 1.0), (3, 1.0), (4, 1.0)),
        "125/375/500 kHz三分量等幅边界",
    ),
    TestCase(
        "T08_125k_2to1to1_500k",
        125_000.0,
        ((1, 0.50), (3, 0.25), (4, 0.25)),
        "125/375/500 kHz，2:1:1比例",
    ),
    TestCase(
        "T09_166p5k_near_500k",
        166_500.0,
        ((1, 1.0), (2, 0.5), (3, 0.2)),
        "500 Hz频率网格，最高499.5 kHz",
    ),
    TestCase(
        "T10_200k_small_second",
        200_000.0,
        ((1, 1.0), (2, 0.08)),
        "200/400 kHz双分量，小二次谐波",
    ),
    TestCase(
        "T11_249p5k_equal_near_boundary",
        249_500.0,
        ((1, 1.0), (2, 1.0)),
        "249.5/499 kHz双分量近边界",
    ),
    TestCase(
        "T12_250k_3to1_boundary",
        250_000.0,
        ((1, 1.0), (2, 1.0 / 3.0)),
        "250/500 kHz双分量，3:1比例",
    ),
    TestCase(
        "T13_250k_weak_fundamental_boundary",
        250_000.0,
        ((1, 0.10), (2, 1.0)),
        "250/500 kHz，弱基波强二次谐波",
    ),
    TestCase(
        "T14_single_500k_boundary",
        500_000.0,
        ((1, 1.0),),
        "500 kHz单频上边界",
    ),
)


def expected_values(case: TestCase, bsv_info: dict[str, float | int]) -> dict:
    mv_per_unit = VPP_MV / float(bsv_info["raw_vpp"])
    components = [
        {
            "order": order,
            "frequency_hz": case.fundamental_hz * order,
            "peak_mv": abs(coefficient) * mv_per_unit,
        }
        for order, coefficient in case.terms
    ]
    return {
        "upp_mv": VPP_MV,
        "rms_mv": mv_per_unit
        * math.sqrt(
            sum(coefficient * coefficient for _, coefficient in case.terms)
            / 2.0
        ),
        "components": components,
    }


def median_measurement(frames: list[dict[str, float]], expected: dict) -> dict:
    measured = {
        "upp_mv": statistics.median(frame["Upp_mV"] for frame in frames),
        "rms_mv": statistics.median(frame["U_mV"] for frame in frames),
        "components": [],
        "component_count": int(
            statistics.median(frame["component_count"] for frame in frames)
        ),
        "signal_valid": int(
            statistics.median(frame["signal_valid"] for frame in frames)
        ),
        "peak_count": int(
            statistics.median(frame["peak_count"] for frame in frames)
        ),
        "adc_overrun_max": max(int(frame["adc_overrun"]) for frame in frames),
    }
    for index, component in enumerate(expected["components"], start=1):
        measured["components"].append(
            {
                "order": component["order"],
                "frequency_hz": statistics.median(
                    frame[f"f{index}_Hz"] for frame in frames
                ),
                "peak_mv": statistics.median(
                    frame[f"U{index}_peak_mV"] for frame in frames
                ),
            }
        )
    return measured


def compare(expected: dict, measured: dict) -> list[dict]:
    comparisons = [
        {
            "name": "Upp",
            "unit": "mV",
            "expected": expected["upp_mv"],
            "measured": measured["upp_mv"],
            "limit": 5.0,
        },
        {
            "name": "Urms",
            "unit": "mV",
            "expected": expected["rms_mv"],
            "measured": measured["rms_mv"],
            "limit": 5.0,
        },
    ]
    for theoretical, actual in zip(expected["components"], measured["components"]):
        order = theoretical["order"]
        comparisons.extend(
            [
                {
                    "name": f"H{order}_frequency",
                    "unit": "Hz",
                    "expected": theoretical["frequency_hz"],
                    "measured": actual["frequency_hz"],
                    "limit": 1000.0,
                },
                {
                    "name": f"H{order}_peak",
                    "unit": "mV",
                    "expected": theoretical["peak_mv"],
                    "measured": actual["peak_mv"],
                    "limit": 5.0,
                },
            ]
        )
    for item in comparisons:
        item["error"] = float(item["measured"]) - float(item["expected"])
        item["passed"] = abs(float(item["error"])) <= float(item["limit"])
    return comparisons


def configure_case(
    instrument: VisaInstrument,
    case: TestCase,
    bsv: bytes,
) -> None:
    # 仪器固件对任意波文件名长度较敏感，使用短名称避免载荷阶段超时。
    filename = f"{case.name.split('_', 1)[0]}.bsv"
    instrument.write_bytes(
        f'WARB1:CARRier "{filename}"\r\n'.encode("ascii"),
        f"upload {filename}",
    )
    instrument.write_bytes(bsv, f"payload {filename}")
    time.sleep(1.0)
    commands = [
        "CHANnel2:LOAD 50",
        "CHANnel2:MODe CONTinue",
        "CHANnel2:BASE:WAVe DC",
        "CHANnel2:BASE:OFFSet 0",
        "CHANnel2:OUTPut ON",
        "CHANnel1:LOAD 50",
        "CHANnel1:MODe CONTinue",
        "CHANnel1:BASE:WAVe ARB",
        f"CHANnel1:BASE:FREQuency {case.fundamental_hz:.6f}",
        f"CHANnel1:BASE:AMPLitude {VPP_MV / 1000.0:.9f}",
        "CHANnel1:BASE:OFFSet 0",
        "CHANnel1:OUTPut ON",
        "SYSTEM:LOCK OFF",
    ]
    for command in commands:
        instrument.write_bytes((command + "\r\n").encode("ascii"), command)
        time.sleep(0.015)

    # 负载模式会引起2:1幅值标称换算，每组测试前必须回读确认。
    for channel in (1, 2):
        load_ohm = float(instrument.query(f"CHANnel{channel}:LOAD?"))
        if abs(load_ohm - 50.0) > 0.01:
            raise RuntimeError(f"CH{channel}负载不是50 Ω：{load_ohm}")


def save_results(results: list[dict], identity: str) -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    json_path = OUTPUT_DIR / "arb_250mV_boundary_suite.json"
    csv_path = OUTPUT_DIR / "arb_250mV_boundary_suite.csv"
    document = {
        "instrument": identity,
        "channel_1_vpp_mv": VPP_MV,
        "channel_1_offset_v": 0.0,
        "channel_1_load_ohm": 50.0,
        "channel_2": "DC 0 V, Output ON",
        "channel_2_load_ohm": 50.0,
        "phase": "all terms zero phase",
        "test_count": len(TEST_CASES),
        "completed_count": len(results),
        "pass_count": sum(item["status"] == "PASS" for item in results),
        "all_passed": (
            len(results) == len(TEST_CASES)
            and all(item["status"] == "PASS" for item in results)
        ),
        "results": results,
    }
    json_path.write_text(
        json.dumps(document, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )

    rows: list[dict] = []
    for result in results:
        if result["status"] == "ERROR":
            rows.append(
                {
                    "case": result["name"],
                    "purpose": result["purpose"],
                    "status": result["status"],
                    "metric": "collection",
                    "error_message": result["error_message"],
                }
            )
            continue
        for item in result["comparisons"]:
            rows.append(
                {
                    "case": result["name"],
                    "purpose": result["purpose"],
                    "status": result["status"],
                    "metric": item["name"],
                    "unit": item["unit"],
                    "expected": item["expected"],
                    "measured": item["measured"],
                    "error": item["error"],
                    "limit": item["limit"],
                    "passed": item["passed"],
                    "error_message": "",
                }
            )
    fieldnames = [
        "case",
        "purpose",
        "status",
        "metric",
        "unit",
        "expected",
        "measured",
        "error",
        "limit",
        "passed",
        "error_message",
    ]
    with csv_path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    results: list[dict] = []
    identity = ""
    with VisaInstrument(DEFAULT_RESOURCE) as instrument, serial.Serial(
        DEFAULT_PORT,
        DEFAULT_BAUD,
        timeout=0.30,
    ) as port:
        identity = instrument.query("*IDN?")
        print(f"INSTRUMENT {identity}", flush=True)
        for index, case in enumerate(TEST_CASES, start=1):
            bsv, bsv_info = make_bsv(list(case.terms), POINT_COUNT)
            expected = expected_values(case, bsv_info)
            minimum_peak = min(
                component["peak_mv"] for component in expected["components"]
            )
            if minimum_peak < 5.0:
                raise ValueError(
                    f"{case.name}理论最小分量仅{minimum_peak:.3f} mV，低于5 mV"
                )
            print(
                f"CASE_START {index}/{len(TEST_CASES)} {case.name} "
                f"f0={case.fundamental_hz:.1f}Hz min_peak={minimum_peak:.3f}mV",
                flush=True,
            )
            try:
                configure_case(instrument, case, bsv)
                port.reset_input_buffer()
                discard_deadline = time.monotonic() + 1.25
                while time.monotonic() < discard_deadline:
                    port.read(port.in_waiting or 1)
                frames, evidence = collect_stable_frames(
                    port,
                    [component["frequency_hz"] for component in expected["components"]],
                    6,
                    6.0,
                )
                measured = median_measurement(frames, expected)
                comparisons = compare(expected, measured)
                multi_valid = (
                    len(expected["components"]) == 1
                    or measured["signal_valid"] == 1
                )
                status = (
                    "PASS"
                    if all(item["passed"] for item in comparisons)
                    and measured["component_count"] == len(expected["components"])
                    and measured["adc_overrun_max"] == 0
                    and multi_valid
                    else "FAIL"
                )
                result = {
                    "name": case.name,
                    "purpose": case.purpose,
                    "fundamental_hz": case.fundamental_hz,
                    "terms": [list(item) for item in case.terms],
                    "status": status,
                    "bsv": bsv_info,
                    "expected": expected,
                    "measured": measured,
                    "comparisons": comparisons,
                    "serial_evidence": evidence,
                }
                results.append(result)
                worst_voltage = max(
                    (
                        abs(item["error"])
                        for item in comparisons
                        if item["unit"] == "mV"
                    ),
                    default=0.0,
                )
                worst_frequency = max(
                    (
                        abs(item["error"])
                        for item in comparisons
                        if item["unit"] == "Hz"
                    ),
                    default=0.0,
                )
                print(
                    f"CASE_DONE {case.name} status={status} "
                    f"max_voltage_error_mv={worst_voltage:.3f} "
                    f"max_frequency_error_hz={worst_frequency:.1f} "
                    f"adc_overrun={measured['adc_overrun_max']}",
                    flush=True,
                )
            except Exception as exc:  # 保留失败并继续后续边界测试。
                results.append(
                    {
                        "name": case.name,
                        "purpose": case.purpose,
                        "fundamental_hz": case.fundamental_hz,
                        "terms": [list(item) for item in case.terms],
                        "status": "ERROR",
                        "error_message": str(exc),
                        "expected": expected,
                    }
                )
                print(f"CASE_ERROR {case.name} {exc}", flush=True)
            save_results(results, identity)

        # 结束时再次强制CH2保持用户要求的工作状态。
        for command in (
            "CHANnel2:LOAD 50",
            "CHANnel2:MODe CONTinue",
            "CHANnel2:BASE:WAVe DC",
            "CHANnel2:BASE:OFFSet 0",
            "CHANnel2:OUTPut ON",
        ):
            instrument.write_bytes((command + "\r\n").encode("ascii"), command)

    save_results(results, identity)
    pass_count = sum(item["status"] == "PASS" for item in results)
    print(
        f"SUITE_DONE pass={pass_count}/{len(TEST_CASES)} "
        f"json={OUTPUT_DIR / 'arb_250mV_boundary_suite.json'}",
        flush=True,
    )
    return 0 if pass_count == len(TEST_CASES) else 2


if __name__ == "__main__":
    raise SystemExit(main())
