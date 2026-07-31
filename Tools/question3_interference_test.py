#!/usr/bin/env python3
"""第三问：组合波叠加 200 mVpp 高频单音干扰的实机验证。"""

from __future__ import annotations

import argparse
import json
import math
import time
from dataclasses import dataclass
from pathlib import Path

import serial

from arb_expression_test import make_bsv
from combination_wave_suite import collect_case_frames, median_measurement
from frequency_sweep import (
    DEFAULT_BAUD,
    DEFAULT_PORT,
    DEFAULT_RESOURCE,
    VisaInstrument,
)


POINT_COUNT = 1000
JAMMER_VPP_MV = 200.0
STABLE_FRAME_COUNT = 5
SETTLE_TIME_S = 0.65
VOLTAGE_ERROR_LIMIT_MV = 5.0
FREQUENCY_ERROR_LIMIT_HZ = 1000.0
DEFAULT_OUTPUT = Path(
    "outputs/20260801_question3/question3_interference_results.json"
)


@dataclass(frozen=True)
class InterferenceCase:
    name: str
    fundamental_hz: int
    ub_vpp_mv: float
    ub_terms: tuple[tuple[int, float], ...]
    jammer_order: int


CASES = (
    InterferenceCase(
        "low_edge_2tone_1mhz",
        10_000,
        50.0,
        ((1, 1.0), (50, 0.70)),
        100,
    ),
    InterferenceCase(
        "low_edge_3tone_1p25mhz",
        12_500,
        250.0,
        ((1, 1.0), (7, 0.55), (40, 0.80)),
        100,
    ),
    InterferenceCase(
        "low_band_2tone_1p5mhz",
        20_000,
        100.0,
        ((1, 1.0), (25, 0.80)),
        75,
    ),
    InterferenceCase(
        "low_band_3tone_1p8mhz",
        25_000,
        220.0,
        ((1, 1.0), (6, 0.65), (19, 0.45)),
        72,
    ),
    InterferenceCase(
        "mid_band_3tone_1mhz",
        40_000,
        200.0,
        ((1, 1.0), (3, 0.65), (9, 0.85)),
        25,
    ),
    InterferenceCase(
        "mid_band_3tone_1p25mhz",
        50_000,
        150.0,
        ((1, 1.0), (4, 0.50), (10, 0.80)),
        25,
    ),
    InterferenceCase(
        "mid_band_3tone_1p5mhz",
        62_500,
        50.0,
        ((1, 1.0), (2, 0.65), (8, 0.85)),
        24,
    ),
    InterferenceCase(
        "mid_band_2tone_2mhz",
        80_000,
        250.0,
        ((1, 1.0), (6, 0.70)),
        25,
    ),
    InterferenceCase(
        "upper_band_3tone_1mhz",
        100_000,
        180.0,
        ((1, 1.0), (3, 0.55), (5, 0.75)),
        10,
    ),
    InterferenceCase(
        "upper_band_2tone_1p25mhz",
        125_000,
        50.0,
        ((1, 1.0), (4, 0.80)),
        10,
    ),
    InterferenceCase(
        "upper_band_3tone_1p5mhz",
        166_666,
        200.0,
        ((1, 1.0), (2, 0.65), (3, 0.85)),
        9,
    ),
    InterferenceCase(
        "upper_edge_2tone_2mhz",
        250_000,
        250.0,
        ((1, 1.0), (2, 0.70)),
        8,
    ),
)


def scale_ub_terms(case: InterferenceCase) -> tuple[tuple[int, float], ...]:
    """把无量纲组合波按目标峰峰值换算为各分量物理峰值。"""
    _, information = make_bsv(list(case.ub_terms), POINT_COUNT)
    scale_mv = case.ub_vpp_mv / float(information["raw_vpp"])
    return tuple(
        (order, coefficient * scale_mv)
        for order, coefficient in case.ub_terms
    )


def expected_values(
    case: InterferenceCase,
    physical_terms: tuple[tuple[int, float], ...],
) -> dict[str, object]:
    components = [
        {
            "order": order,
            "frequency_hz": float(order * case.fundamental_hz),
            "peak_mv": abs(peak_mv),
        }
        for order, peak_mv in physical_terms
    ]
    return {
        "upp_mv": case.ub_vpp_mv,
        "rms_mv": math.sqrt(
            sum(float(item["peak_mv"]) ** 2 for item in components) / 2.0
        ),
        "components": components,
    }


def configure_generator(
    instrument: VisaInstrument,
    case_index: int,
    case: InterferenceCase,
    physical_terms: tuple[tuple[int, float], ...],
) -> dict[str, float | int]:
    combined_terms = [
        *physical_terms,
        (case.jammer_order, JAMMER_VPP_MV / 2.0),
    ]
    payload, information = make_bsv(combined_terms, POINT_COUNT)
    filename = f"q3_{case_index % 2}.bsv"

    instrument.write_bytes(
        f'WARB1:CARRier "{filename}"\r\n'.encode("ascii"),
        f"upload {filename}",
    )
    instrument.write_bytes(payload, f"payload {filename}")
    time.sleep(0.8)

    commands = (
        "CHANnel2:LOAD 50",
        "CHANnel2:MODe CONTinue",
        "CHANnel2:BASE:WAVe DC",
        "CHANnel2:BASE:OFFSet 0",
        "CHANnel2:OUTPut ON",
        "CHANnel1:LOAD 50",
        "CHANnel1:MODe CONTinue",
        "CHANnel1:BASE:WAVe ARB",
        f"CHANnel1:BASE:FREQuency {case.fundamental_hz}",
        f"CHANnel1:BASE:AMPLitude {float(information['raw_vpp']) / 1000.0:.9f}",
        "CHANnel1:BASE:OFFSet 0",
        "CHANnel1:OUTPut ON",
        "SYSTEM:LOCK OFF",
    )
    for command in commands:
        instrument.write_bytes((command + "\r\n").encode("ascii"), command)
        time.sleep(0.015)
    return information


def result_row(
    case: InterferenceCase,
    expected: dict[str, object],
    measured: dict[str, object],
    strict_match: bool,
    generator_information: dict[str, float | int],
    measurement_elapsed_s: float,
) -> dict[str, object]:
    expected_components = expected["components"]
    measured_components = measured["components"]
    assert isinstance(expected_components, list)
    assert isinstance(measured_components, dict)

    components: list[dict[str, object]] = []
    for component in expected_components:
        order = int(component["order"])
        detected = measured_components.get(order)
        components.append(
            {
                **component,
                "measured_frequency_hz": (
                    detected["frequency_hz"] if detected is not None else None
                ),
                "frequency_error_hz": (
                    detected["frequency_hz"] - component["frequency_hz"]
                    if detected is not None
                    else None
                ),
                "measured_peak_mv": (
                    detected["peak_mv"] if detected is not None else None
                ),
                "peak_error_mv": (
                    detected["peak_mv"] - component["peak_mv"]
                    if detected is not None
                    else None
                ),
            }
        )

    peak_errors = [
        abs(float(item["peak_error_mv"]))
        for item in components
        if item["peak_error_mv"] is not None
    ]
    frequency_errors = [
        abs(float(item["frequency_error_hz"]))
        for item in components
        if item["frequency_error_hz"] is not None
    ]
    maximum_peak_error_mv = max(peak_errors, default=float("inf"))
    maximum_frequency_error_hz = max(frequency_errors, default=float("inf"))
    upp_error_mv = float(measured["upp_mv"]) - float(expected["upp_mv"])
    rms_error_mv = float(measured["rms_mv"]) - float(expected["rms_mv"])
    passed = (
        strict_match
        and int(measured["signal_valid"]) == 1
        and int(measured["adc_overrun_max"]) == 0
        and len(peak_errors) == len(expected_components)
        and abs(upp_error_mv) <= VOLTAGE_ERROR_LIMIT_MV
        and abs(rms_error_mv) <= VOLTAGE_ERROR_LIMIT_MV
        and maximum_peak_error_mv <= VOLTAGE_ERROR_LIMIT_MV
        and maximum_frequency_error_hz <= FREQUENCY_ERROR_LIMIT_HZ
        and measurement_elapsed_s <= 2.0
    )

    return {
        "name": case.name,
        "fundamental_hz": case.fundamental_hz,
        "ub_vpp_mv": case.ub_vpp_mv,
        "jammer_frequency_hz": case.fundamental_hz * case.jammer_order,
        "jammer_vpp_mv": JAMMER_VPP_MV,
        "generator_total_vpp_mv": float(generator_information["raw_vpp"]),
        "expected_upp_mv": expected["upp_mv"],
        "measured_upp_mv": measured["upp_mv"],
        "upp_error_mv": upp_error_mv,
        "expected_rms_mv": expected["rms_mv"],
        "measured_rms_mv": measured["rms_mv"],
        "rms_error_mv": rms_error_mv,
        "measured_component_count": measured["component_count"],
        "signal_valid": measured["signal_valid"],
        "adc_overrun_max": measured["adc_overrun_max"],
        "strict_match": strict_match,
        "measurement_elapsed_s": measurement_elapsed_s,
        "maximum_component_peak_error_mv": maximum_peak_error_mv,
        "maximum_frequency_error_hz": maximum_frequency_error_hz,
        "pass": passed,
        "detected_components": [
            {
                "order": order,
                "frequency_hz": component["frequency_hz"],
                "peak_mv": component["peak_mv"],
            }
            for order, component in sorted(measured_components.items())
        ],
        "components": components,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument(
        "--case",
        choices=[item.name for item in CASES],
        help="只运行指定用例",
    )
    args = parser.parse_args()
    results: list[dict[str, object]] = []
    selected_cases = [item for item in CASES if args.case in (None, item.name)]
    with VisaInstrument(DEFAULT_RESOURCE) as instrument, serial.Serial(
        DEFAULT_PORT,
        DEFAULT_BAUD,
        timeout=0.35,
    ) as port:
        print(f"INSTRUMENT {instrument.query('*IDN?')}", flush=True)
        try:
            for index, case in enumerate(selected_cases, start=1):
                physical_terms = scale_ub_terms(case)
                expected = expected_values(case, physical_terms)
                generator_information = configure_generator(
                    instrument,
                    index,
                    case,
                    physical_terms,
                )
                measurement_start = time.monotonic()
                port.reset_input_buffer()
                settle_deadline = time.monotonic() + SETTLE_TIME_S
                while time.monotonic() < settle_deadline:
                    port.read(port.in_waiting or 1)

                expected_components = expected["components"]
                assert isinstance(expected_components, list)
                frames, _, strict_match = collect_case_frames(
                    port,
                    [float(item["frequency_hz"]) for item in expected_components],
                    STABLE_FRAME_COUNT,
                    8.0,
                )
                measured = median_measurement(frames)
                measurement_elapsed_s = time.monotonic() - measurement_start
                row = result_row(
                    case,
                    expected,
                    measured,
                    strict_match,
                    generator_information,
                    measurement_elapsed_s,
                )
                results.append(row)
                print(json.dumps(row, ensure_ascii=False), flush=True)
        finally:
            for command in (
                "CHANnel1:OUTPut OFF",
                "CHANnel2:LOAD 50",
                "CHANnel2:BASE:WAVe DC",
                "CHANnel2:BASE:OFFSet 0",
                "CHANnel2:OUTPut ON",
            ):
                instrument.write_bytes((command + "\r\n").encode("ascii"), command)

    summary = {
        "case_count": len(results),
        "pass_count": sum(bool(item["pass"]) for item in results),
        "maximum_abs_upp_error_mv": max(
            abs(float(item["upp_error_mv"])) for item in results
        ),
        "maximum_abs_rms_error_mv": max(
            abs(float(item["rms_error_mv"])) for item in results
        ),
        "maximum_component_peak_error_mv": max(
            float(item["maximum_component_peak_error_mv"])
            for item in results
        ),
        "maximum_frequency_error_hz": max(
            float(item["maximum_frequency_error_hz"]) for item in results
        ),
        "maximum_measurement_elapsed_s": max(
            float(item["measurement_elapsed_s"]) for item in results
        ),
        "adc_overrun_max": max(int(item["adc_overrun_max"]) for item in results),
    }
    report = {
        "generated_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        "serial_port": DEFAULT_PORT,
        "serial_baud": DEFAULT_BAUD,
        "jammer_vpp_mv": JAMMER_VPP_MV,
        "voltage_error_limit_mv": VOLTAGE_ERROR_LIMIT_MV,
        "frequency_error_limit_hz": FREQUENCY_ERROR_LIMIT_HZ,
        "time_limit_s": 2.0,
        "summary": summary,
        "results": results,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))
    print(f"REPORT {args.output.resolve()}", flush=True)
    return 0 if summary["pass_count"] == summary["case_count"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
