#!/usr/bin/env python3
"""第三问：保持当前 ub 不变，将 200 mVpp 干扰从 1 MHz 扫到 20 MHz。"""

from __future__ import annotations

import argparse
import csv
import json
import time
from pathlib import Path

import serial

from combination_wave_suite import collect_case_frames, median_measurement
from frequency_sweep import (
    DEFAULT_BAUD,
    DEFAULT_PORT,
    DEFAULT_RESOURCE,
    VisaInstrument,
)
from question3_interference_test import (
    FREQUENCY_ERROR_LIMIT_HZ,
    JAMMER_VPP_MV,
    SETTLE_TIME_S,
    STABLE_FRAME_COUNT,
    VOLTAGE_ERROR_LIMIT_MV,
    InterferenceCase,
    configure_generator,
    expected_values,
    result_row,
    scale_ub_terms,
)


FUNDAMENTAL_HZ = 250_000
UB_VPP_MV = 250.0
UB_TERMS = ((1, 1.0), (2, 0.70))
JAMMER_MIN_MHZ = 1
JAMMER_MAX_MHZ = 20
DEFAULT_OUTPUT = Path(
    "outputs/20260801_question3_1_20mhz/"
    "question3_jammer_1_20mhz_results.json"
)


def generate_cases() -> list[InterferenceCase]:
    return [
        InterferenceCase(
            name=f"jammer_{frequency_mhz:02d}mhz",
            fundamental_hz=FUNDAMENTAL_HZ,
            ub_vpp_mv=UB_VPP_MV,
            ub_terms=UB_TERMS,
            jammer_order=(frequency_mhz * 1_000_000) // FUNDAMENTAL_HZ,
        )
        for frequency_mhz in range(JAMMER_MIN_MHZ, JAMMER_MAX_MHZ + 1)
    ]


def csv_row(row: dict[str, object]) -> dict[str, object]:
    components = row["components"]
    assert isinstance(components, list)
    first = components[0]
    second = components[1]
    return {
        "name": row["name"],
        "jammer_frequency_hz": row["jammer_frequency_hz"],
        "jammer_vpp_mv": row["jammer_vpp_mv"],
        "generator_total_vpp_mv": row["generator_total_vpp_mv"],
        "expected_upp_mv": row["expected_upp_mv"],
        "measured_upp_mv": row["measured_upp_mv"],
        "upp_error_mv": row["upp_error_mv"],
        "expected_rms_mv": row["expected_rms_mv"],
        "measured_rms_mv": row["measured_rms_mv"],
        "rms_error_mv": row["rms_error_mv"],
        "expected_f1_hz": first["frequency_hz"],
        "measured_f1_hz": first["measured_frequency_hz"],
        "f1_error_hz": first["frequency_error_hz"],
        "expected_a1_peak_mv": first["peak_mv"],
        "measured_a1_peak_mv": first["measured_peak_mv"],
        "a1_error_mv": first["peak_error_mv"],
        "expected_f2_hz": second["frequency_hz"],
        "measured_f2_hz": second["measured_frequency_hz"],
        "f2_error_hz": second["frequency_error_hz"],
        "expected_a2_peak_mv": second["peak_mv"],
        "measured_a2_peak_mv": second["measured_peak_mv"],
        "a2_error_mv": second["peak_error_mv"],
        "measured_component_count": row["measured_component_count"],
        "strict_match": row["strict_match"],
        "signal_valid": row["signal_valid"],
        "measurement_elapsed_s": row["measurement_elapsed_s"],
        "adc_overrun_max": row["adc_overrun_max"],
        "pass": row["pass"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    cases = generate_cases()
    results: list[dict[str, object]] = []

    with VisaInstrument(DEFAULT_RESOURCE) as instrument, serial.Serial(
        DEFAULT_PORT,
        DEFAULT_BAUD,
        timeout=0.35,
    ) as port:
        print(f"INSTRUMENT {instrument.query('*IDN?')}", flush=True)
        try:
            for index, case in enumerate(cases, start=1):
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
                    [
                        float(component["frequency_hz"])
                        for component in expected_components
                    ],
                    STABLE_FRAME_COUNT,
                    8.0,
                )
                measured = median_measurement(frames)
                elapsed_s = time.monotonic() - measurement_start
                row = result_row(
                    case,
                    expected,
                    measured,
                    strict_match,
                    generator_information,
                    elapsed_s,
                )
                results.append(row)
                print(
                    f"{index:02d}/20 "
                    f"fJ={case.jammer_order * FUNDAMENTAL_HZ / 1e6:.0f}MHz "
                    f"Upp={row['measured_upp_mv']:.1f}mV "
                    f"err={row['upp_error_mv']:+.1f}mV "
                    f"components={row['measured_component_count']} "
                    f"time={row['measurement_elapsed_s']:.3f}s "
                    f"{'PASS' if row['pass'] else 'FAIL'}",
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
                instrument.write_bytes((command + "\r\n").encode("ascii"), command)

    summary = {
        "case_count": len(results),
        "pass_count": sum(bool(row["pass"]) for row in results),
        "maximum_abs_upp_error_mv": max(
            abs(float(row["upp_error_mv"])) for row in results
        ),
        "maximum_abs_rms_error_mv": max(
            abs(float(row["rms_error_mv"])) for row in results
        ),
        "maximum_component_peak_error_mv": max(
            float(row["maximum_component_peak_error_mv"]) for row in results
        ),
        "maximum_frequency_error_hz": max(
            float(row["maximum_frequency_error_hz"]) for row in results
        ),
        "maximum_measurement_elapsed_s": max(
            float(row["measurement_elapsed_s"]) for row in results
        ),
        "adc_overrun_max": max(int(row["adc_overrun_max"]) for row in results),
    }
    report = {
        "generated_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        "serial_port": DEFAULT_PORT,
        "serial_baud": DEFAULT_BAUD,
        "ub_fundamental_hz": FUNDAMENTAL_HZ,
        "ub_vpp_mv": UB_VPP_MV,
        "ub_terms": UB_TERMS,
        "jammer_vpp_mv": JAMMER_VPP_MV,
        "jammer_frequency_min_hz": JAMMER_MIN_MHZ * 1_000_000,
        "jammer_frequency_max_hz": JAMMER_MAX_MHZ * 1_000_000,
        "jammer_frequency_step_hz": 1_000_000,
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
    csv_path = args.output.with_suffix(".csv")
    rows = [csv_row(row) for row in results]
    with csv_path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)

    print(json.dumps(summary, ensure_ascii=False, indent=2), flush=True)
    print(f"JSON {args.output.resolve()}", flush=True)
    print(f"CSV  {csv_path.resolve()}", flush=True)
    return 0 if summary["pass_count"] == summary["case_count"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
