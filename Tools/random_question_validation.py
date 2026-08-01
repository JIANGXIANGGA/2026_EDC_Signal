#!/usr/bin/env python3
"""按赛题范围生成全新随机组合波，并逐问执行实机抽测。"""

from __future__ import annotations

import argparse
import json
import math
import random
import time
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path

import serial

from arb_expression_test import make_bsv
from combination_wave_suite import (
    FunctionSpec,
    TestCase,
    collect_case_frames,
    expected_values as combination_expected_values,
    median_measurement,
    result_row as combination_result_row,
)
from frequency_sweep import (
    DEFAULT_BAUD,
    DEFAULT_PORT,
    DEFAULT_RESOURCE,
    VisaInstrument,
)
from question3_interference_test import (
    JAMMER_VPP_MV,
    InterferenceCase,
    expected_values as interference_expected_values,
    result_row as interference_result_row,
    scale_ub_terms,
)


POINT_COUNT = 1_000
STABLE_FRAME_COUNT = 5
VOLTAGE_ERROR_LIMIT_MV = 5.0
FREQUENCY_ERROR_LIMIT_HZ = 1_000.0
QUESTION_LIMITS = {
    1: {
        "component_frequency_min_hz": 10_000,
        "component_frequency_max_hz": 200_000,
        "vpp_min_mv": 100.0,
        "vpp_max_mv": 250.0,
    },
    2: {
        "component_frequency_min_hz": 10_000,
        "component_frequency_max_hz": 500_000,
        "vpp_min_mv": 50.0,
        "vpp_max_mv": 250.0,
    },
    3: {
        "component_frequency_min_hz": 10_000,
        "component_frequency_max_hz": 500_000,
        "vpp_min_mv": 50.0,
        "vpp_max_mv": 250.0,
        "jammer_frequency_min_hz": 1_000_000,
        "jammer_frequency_max_hz": 20_000_000,
        "jammer_vpp_mv": JAMMER_VPP_MV,
    },
}


@dataclass(frozen=True)
class RandomCase:
    """一个可复现且满足对应题目边界的随机用例。"""

    question: int
    case_id: str
    fundamental_hz: int
    signal_vpp_mv: float
    terms: tuple[tuple[int, float], ...]
    jammer_frequency_hz: int = 0
    jammer_order: int = 0


def expression(terms: tuple[tuple[int, float], ...]) -> str:
    """生成便于报告阅读的零初相表达式。"""
    return "+".join(
        f"{coefficient:.3f}*sin(2*pi*{order}*f0*t)"
        for order, coefficient in terms
    )


def _random_half_mv(rng: random.Random, lower: float, upper: float) -> float:
    """生成以 0.5 mV 结尾的幅值，主动避开历史整数幅值清单。"""
    first = math.ceil(lower) if math.ceil(lower) % 1 == 0 else math.ceil(lower)
    minimum_index = math.ceil((first + 0.5) * 2.0)
    maximum_index = math.floor((upper - 0.5) * 2.0)
    candidates = [index for index in range(minimum_index, maximum_index + 1) if index & 1]
    if not candidates:
        raise ValueError("指定幅值范围内没有可用的半整数 mV 值")
    return rng.choice(candidates) / 2.0


def _random_fundamental(
    rng: random.Random,
    component_max_hz: int,
    require_three_terms: bool,
) -> int:
    """在 100 Hz 栅格上选基频，并排除历史测试使用的 500 Hz 栅格。"""
    minimum_hz = 10_000
    divisor = 3 if require_three_terms else 2
    maximum_hz = component_max_hz // divisor
    candidates = [
        value
        for value in range(minimum_hz, maximum_hz + 1, 100)
        if value % 500 != 0
    ]
    return rng.choice(candidates)


def _random_terms(
    rng: random.Random,
    maximum_order: int,
    term_count: int,
    used_expressions: set[str],
) -> tuple[tuple[int, float], ...]:
    """随机产生基波加一个或两个谐波，所有分量均为零初相。"""
    orders = list(range(2, maximum_order + 1))
    if len(orders) < term_count - 1:
        raise ValueError("当前基频无法容纳所需谐波数量")
    for _ in range(1_000):
        selected_orders = sorted(rng.sample(orders, term_count - 1))
        ratios = [rng.randint(55, 95) / 100.0 for _ in selected_orders]
        terms = ((1, 1.0),) + tuple(zip(selected_orders, ratios))
        key = expression(terms)
        if key not in used_expressions:
            used_expressions.add(key)
            return terms
    raise RuntimeError("无法生成唯一的随机组合函数")


def generate_cases(question: int, seed: int, count: int) -> list[RandomCase]:
    """生成分层随机用例，使少量抽测仍覆盖低、中、高幅值。"""
    if count < 1:
        raise ValueError("用例数量必须为正整数")
    limits = QUESTION_LIMITS[question]
    rng = random.Random(seed ^ (question * 0x5A17))
    cases: list[RandomCase] = []
    used_expressions: set[str] = set()
    vpp_min = float(limits["vpp_min_mv"])
    vpp_max = float(limits["vpp_max_mv"])

    for index in range(1, count + 1):
        band_lower = vpp_min + (vpp_max - vpp_min) * (index - 1) / count
        band_upper = vpp_min + (vpp_max - vpp_min) * index / count
        signal_vpp_mv = _random_half_mv(rng, band_lower, band_upper)
        term_count = 2 if index & 1 else 3
        fundamental_hz = _random_fundamental(
            rng,
            int(limits["component_frequency_max_hz"]),
            term_count == 3,
        )
        maximum_order = int(limits["component_frequency_max_hz"]) // fundamental_hz
        terms = _random_terms(rng, maximum_order, term_count, used_expressions)

        jammer_order = 0
        jammer_frequency_hz = 0
        if question == 3:
            minimum_order = math.ceil(
                int(limits["jammer_frequency_min_hz"]) / fundamental_hz
            )
            maximum_jammer_order = min(
                450,
                int(limits["jammer_frequency_max_hz"]) // fundamental_hz,
            )
            jammer_candidates = [
                order
                for order in range(minimum_order, maximum_jammer_order + 1)
                if (order * fundamental_hz) % 100_000 != 0
            ]
            jammer_order = rng.choice(jammer_candidates)
            jammer_frequency_hz = jammer_order * fundamental_hz

        cases.append(
            RandomCase(
                question=question,
                case_id=f"Q{question}-R{index:02d}",
                fundamental_hz=fundamental_hz,
                signal_vpp_mv=signal_vpp_mv,
                terms=terms,
                jammer_frequency_hz=jammer_frequency_hz,
                jammer_order=jammer_order,
            )
        )
    return cases


def validate_cases(cases: list[RandomCase]) -> None:
    """在控制仪器前检查题目边界、函数唯一性及历史去重特征。"""
    if len({expression(case.terms) for case in cases}) != len(cases):
        raise RuntimeError("本轮随机函数存在重复")
    for case in cases:
        limits = QUESTION_LIMITS[case.question]
        component_frequencies = [
            order * case.fundamental_hz for order, _ in case.terms
        ]
        if not all(
            int(limits["component_frequency_min_hz"])
            <= frequency
            <= int(limits["component_frequency_max_hz"])
            for frequency in component_frequencies
        ):
            raise RuntimeError(f"{case.case_id} 有效分量频率越界")
        if not (
            float(limits["vpp_min_mv"])
            <= case.signal_vpp_mv
            <= float(limits["vpp_max_mv"])
        ):
            raise RuntimeError(f"{case.case_id} 峰峰值越界")
        if case.signal_vpp_mv.is_integer():
            raise RuntimeError(f"{case.case_id} 未避开历史整数幅值清单")
        if case.fundamental_hz % 500 == 0:
            raise RuntimeError(f"{case.case_id} 未避开历史 500 Hz 频率栅格")
        if case.question == 3 and not (
            int(limits["jammer_frequency_min_hz"])
            <= case.jammer_frequency_hz
            <= int(limits["jammer_frequency_max_hz"])
        ):
            raise RuntimeError(f"{case.case_id} 干扰频率越界")


def configure_common(instrument: VisaInstrument) -> None:
    """只启用实际接线的 CH1；CH2 明确关闭。"""
    commands = (
        "CHANnel2:OUTPut OFF",
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
    load_ohm = float(instrument.query("CHANnel1:LOAD?"))
    if abs(load_ohm - 50.0) > 0.01:
        raise RuntimeError(f"CH1 负载不是 50 Ω：{load_ohm}")


def upload_payload(
    instrument: VisaInstrument,
    filename: str,
    terms: list[tuple[int, float]],
) -> dict[str, float | int]:
    """上传任意波文件，并返回发生器计算所需的原始峰峰值。"""
    payload, information = make_bsv(terms, POINT_COUNT)
    instrument.write_bytes(
        f'WARB1:CARRier "{filename}"\r\n'.encode("ascii"),
        f"upload {filename}",
    )
    instrument.write_bytes(payload, f"payload {filename}")
    time.sleep(0.8)
    instrument.write_bytes(b"CHANnel1:BASE:WAVe ARB\r\n", "select ARB")
    return information


def configure_ch1_case(
    instrument: VisaInstrument,
    fundamental_hz: int,
    overall_vpp_mv: float,
) -> None:
    """设置 CH1 基频、完整波形峰峰值及零偏移。"""
    commands = (
        f"CHANnel1:BASE:FREQuency {fundamental_hz}",
        f"CHANnel1:BASE:AMPLitude {overall_vpp_mv / 1_000.0:.9f}",
        "CHANnel1:BASE:OFFSet 0",
        "CHANnel1:OUTPut ON",
    )
    for command in commands:
        instrument.write_bytes((command + "\r\n").encode("ascii"), command)
        time.sleep(0.012)


def _common_record(case: RandomCase) -> dict[str, object]:
    return {
        **asdict(case),
        "terms": [list(term) for term in case.terms],
        "expression": expression(case.terms),
        "phase_deg": 0.0,
        "offset_v": 0.0,
        "connection": "UTG900E CH1 -> DUT; CH2 OFF",
    }


def run_q1_q2_case(
    instrument: VisaInstrument,
    port: serial.Serial,
    case: RandomCase,
    case_index: int,
) -> dict[str, object]:
    """运行第一问或第二问的一个组合波用例。"""
    function = FunctionSpec(case_index, case.terms)
    test_case = TestCase(function, 1, case.fundamental_hz, case.signal_vpp_mv)
    information = upload_payload(
        instrument,
        f"qr{case.question}{case_index % 2}.bsv",
        list(case.terms),
    )
    expected = combination_expected_values(test_case, information)
    configure_ch1_case(instrument, case.fundamental_hz, case.signal_vpp_mv)
    measurement_start = time.monotonic()
    port.reset_input_buffer()
    settle_deadline = time.monotonic() + 0.65
    while time.monotonic() < settle_deadline:
        port.read(port.in_waiting or 1)
    expected_frequencies = [
        float(component["frequency_hz"])
        for component in expected["components"]
    ]
    frames, evidence, strict_match = collect_case_frames(
        port,
        expected_frequencies,
        STABLE_FRAME_COUNT,
        8.0,
    )
    measured = median_measurement(frames)
    elapsed_s = time.monotonic() - measurement_start
    row = combination_result_row(
        f"random_q{case.question}",
        test_case,
        expected,
        measured,
        len(frames),
        strict_match,
        "",
    )
    row["case_id"] = case.case_id
    return {
        **_common_record(case),
        "expected": expected,
        "measured": measured,
        "measurement_elapsed_s": elapsed_s,
        "strict_frequency_match": strict_match,
        "stable_frame_count": len(frames),
        "max_abs_voltage_error_mv": row["max_abs_voltage_error_mv"],
        "max_abs_frequency_error_hz": row["max_abs_frequency_error_hz"],
        "status": row["status"],
        "serial_evidence": evidence,
    }


def run_q3_case(
    instrument: VisaInstrument,
    port: serial.Serial,
    case: RandomCase,
    case_index: int,
) -> dict[str, object]:
    """在 CH1 内合成有效信号与 200 mVpp 高频干扰并运行第三问。"""
    interference_case = InterferenceCase(
        name=case.case_id,
        fundamental_hz=case.fundamental_hz,
        ub_vpp_mv=case.signal_vpp_mv,
        ub_terms=case.terms,
        jammer_order=case.jammer_order,
    )
    physical_terms = scale_ub_terms(interference_case)
    combined_terms = [*physical_terms, (case.jammer_order, JAMMER_VPP_MV / 2.0)]
    information = upload_payload(
        instrument,
        f"q3r{case_index % 2}.bsv",
        combined_terms,
    )
    configure_ch1_case(
        instrument,
        case.fundamental_hz,
        float(information["raw_vpp"]),
    )
    expected = interference_expected_values(interference_case, physical_terms)
    measurement_start = time.monotonic()
    port.reset_input_buffer()
    settle_deadline = time.monotonic() + 0.65
    while time.monotonic() < settle_deadline:
        port.read(port.in_waiting or 1)
    expected_components = expected["components"]
    assert isinstance(expected_components, list)
    frames, evidence, strict_match = collect_case_frames(
        port,
        [float(component["frequency_hz"]) for component in expected_components],
        STABLE_FRAME_COUNT,
        8.0,
    )
    measured = median_measurement(frames)
    elapsed_s = time.monotonic() - measurement_start
    row = interference_result_row(
        interference_case,
        expected,
        measured,
        strict_match,
        information,
        elapsed_s,
    )
    return {
        **_common_record(case),
        "generator_total_vpp_mv": information["raw_vpp"],
        "expected": expected,
        "measured": measured,
        "measurement_elapsed_s": elapsed_s,
        "strict_frequency_match": strict_match,
        "stable_frame_count": len(frames),
        "max_abs_voltage_error_mv": max(
            abs(float(row["upp_error_mv"])),
            abs(float(row["rms_error_mv"])),
            float(row["maximum_component_peak_error_mv"]),
        ),
        "max_abs_frequency_error_hz": row["maximum_frequency_error_hz"],
        "status": "PASS" if row["pass"] else "FAIL",
        "serial_evidence": evidence,
    }


def save_results(
    path: Path,
    question: int,
    seed: int,
    instrument_identity: str,
    cases: list[RandomCase],
    results: list[dict[str, object]],
) -> None:
    """每完成一例即覆盖保存，避免中断时丢失已经完成的实测数据。"""
    passed = sum(result.get("status") == "PASS" for result in results)
    payload = {
        "generated_at": datetime.now().isoformat(timespec="seconds"),
        "question": question,
        "random_seed": seed,
        "instrument": instrument_identity,
        "serial_port": DEFAULT_PORT,
        "serial_baud": DEFAULT_BAUD,
        "connection": "UTG900E CH1 -> DUT; CH2 OFF",
        "limits": QUESTION_LIMITS[question],
        "planned_cases": [
            {
                **_common_record(case),
                "historical_duplicate": False,
            }
            for case in cases
        ],
        "summary": {
            "planned_case_count": len(cases),
            "completed_case_count": len(results),
            "pass_count": passed,
            "fail_count": len(results) - passed,
            "all_passed": len(results) == len(cases) and passed == len(cases),
            "max_abs_voltage_error_mv": max(
                (float(result["max_abs_voltage_error_mv"]) for result in results),
                default=0.0,
            ),
            "max_abs_frequency_error_hz": max(
                (float(result["max_abs_frequency_error_hz"]) for result in results),
                default=0.0,
            ),
            "max_measurement_elapsed_s": max(
                (float(result["measurement_elapsed_s"]) for result in results),
                default=0.0,
            ),
        },
        "results": results,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--question", type=int, choices=(1, 2, 3), required=True)
    parser.add_argument("--seed", type=int, default=20_260_801_17)
    parser.add_argument("--case-count", type=int, default=5)
    parser.add_argument("--resource", default=DEFAULT_RESOURCE)
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--output-dir",
        default="outputs/20260801_wzh_random_new_cases",
    )
    args = parser.parse_args()

    cases = generate_cases(args.question, args.seed, args.case_count)
    validate_cases(cases)
    output_path = (
        Path(args.output_dir).resolve()
        / f"question{args.question}_random_validation.json"
    )
    print(
        json.dumps(
            {
                "question": args.question,
                "seed": args.seed,
                "case_count": len(cases),
                "cases": [
                    {
                        **_common_record(case),
                        "historical_duplicate": False,
                    }
                    for case in cases
                ],
            },
            ensure_ascii=False,
            indent=2,
        ),
        flush=True,
    )
    if args.dry_run:
        return 0

    results: list[dict[str, object]] = []
    with VisaInstrument(args.resource) as instrument, serial.Serial(
        args.port,
        args.baud,
        timeout=0.35,
    ) as port:
        identity = instrument.query("*IDN?")
        print(f"INSTRUMENT {identity}", flush=True)
        configure_common(instrument)
        try:
            for case_index, case in enumerate(cases, start=1):
                try:
                    if args.question in (1, 2):
                        result = run_q1_q2_case(
                            instrument, port, case, case_index
                        )
                    else:
                        result = run_q3_case(instrument, port, case, case_index)
                except Exception as exc:
                    result = {
                        **_common_record(case),
                        "status": "ERROR",
                        "error_message": str(exc),
                        "max_abs_voltage_error_mv": math.inf,
                        "max_abs_frequency_error_hz": math.inf,
                        "measurement_elapsed_s": 0.0,
                    }
                results.append(result)
                save_results(
                    output_path,
                    args.question,
                    args.seed,
                    identity,
                    cases,
                    results,
                )
                print(
                    f"CASE_DONE {case_index}/{len(cases)} {case.case_id} "
                    f"status={result['status']} f0={case.fundamental_hz}Hz "
                    f"vpp={case.signal_vpp_mv:.1f}mV "
                    f"max_u_err={result['max_abs_voltage_error_mv']} "
                    f"max_f_err={result['max_abs_frequency_error_hz']}",
                    flush=True,
                )
        finally:
            for command in (
                "CHANnel1:OUTPut OFF",
                "CHANnel2:OUTPut OFF",
            ):
                instrument.write_bytes((command + "\r\n").encode("ascii"), command)

    all_passed = all(result["status"] == "PASS" for result in results)
    print(f"OUTPUT {output_path}", flush=True)
    return 0 if all_passed else 2


if __name__ == "__main__":
    raise SystemExit(main())
