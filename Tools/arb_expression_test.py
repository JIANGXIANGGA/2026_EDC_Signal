#!/usr/bin/env python3
"""上传零相位多音任意波，读取STM32串口并与理论值比较。"""

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
import struct
import time
from pathlib import Path

import serial

from frequency_sweep import DEFAULT_BAUD, DEFAULT_PORT, DEFAULT_RESOURCE, VisaInstrument


def parse_terms(text: str) -> list[tuple[int, float]]:
    """解析“谐波次数:系数”列表。"""
    terms: list[tuple[int, float]] = []
    for item in text.split(","):
        order_text, separator, coefficient_text = item.partition(":")
        if not separator:
            raise ValueError(f"无效谐波项：{item}")
        order = int(order_text)
        coefficient = float(coefficient_text)
        if order <= 0 or coefficient == 0.0:
            raise ValueError("谐波次数必须为正整数，系数不能为0")
        terms.append((order, coefficient))
    terms.sort()
    return terms


def make_bsv(
    terms: list[tuple[int, float]],
    point_count: int,
) -> tuple[bytes, dict[str, float | int]]:
    """按UTG900E厂商格式生成1000点、小端int16的BSV字节流。"""
    samples = [
        sum(
            coefficient * math.sin(2.0 * math.pi * order * index / point_count)
            for order, coefficient in terms
        )
        for index in range(point_count)
    ]
    minimum = min(samples)
    maximum = max(samples)
    raw_vpp = maximum - minimum
    rate = max(maximum / 32767.0, abs(minimum) / 32768.0)
    codes = [
        max(-32768, min(32767, round(sample / rate)))
        for sample in samples
    ]
    body = (
        f"VPP:{raw_vpp:.6f}\r\n"
        "OFFSET:0.000000\r\n"
        "CHANNEL:1\r\n"
        f"RATEPOS:{rate:.6f}\r\n"
        f"RATENEG:{rate:.6f}\r\n"
        f"MAX:{max(codes):.6f}\r\n"
        f"MIN:{min(codes):.6f}\r\n"
    )
    header = (
        f"[HEAD]:{len(body)}\r\n{body}[DATA]:{point_count}\r\n"
    ).encode("ascii")
    payload = struct.pack(f"<{point_count}h", *codes)
    return header + payload, {
        "point_count": point_count,
        "raw_minimum": minimum,
        "raw_maximum": maximum,
        "raw_vpp": raw_vpp,
        "rate": rate,
        "code_minimum": min(codes),
        "code_maximum": max(codes),
        "bsv_bytes": len(header) + len(payload),
    }


def parse_telemetry(line: str) -> dict[str, float]:
    return {
        key: float(value)
        for key, value in re.findall(
            r"([A-Za-z0-9_]+):(-?\d+(?:\.\d+)?)",
            line,
        )
    }


def collect_stable_frames(
    port: serial.Serial,
    expected_frequencies: list[float],
    frame_count: int,
    timeout_s: float,
) -> tuple[list[dict[str, float]], list[str]]:
    """只保留9帧平均完成、三分量频率匹配且无ADC溢出的帧。"""
    deadline = time.monotonic() + timeout_s
    frames: list[dict[str, float]] = []
    evidence_lines: list[str] = []
    pending: dict[str, float] | None = None
    pending_line = ""

    while time.monotonic() < deadline and len(frames) < frame_count:
        line = port.readline().decode("ascii", "replace").strip()
        if line.startswith("Upp_mV:"):
            pending = parse_telemetry(line)
            pending_line = line
            continue
        if not line.startswith("seq:") or pending is None:
            continue

        diagnostic = parse_telemetry(line)
        if int(pending.get("component_count", 0)) != len(expected_frequencies):
            pending = None
            continue
        if int(diagnostic.get("avg_count", 0)) < 9:
            pending = None
            continue
        if int(diagnostic.get("adc_overrun", 1)) != 0:
            pending = None
            continue
        measured_frequencies = [
            pending.get(f"f{index}_Hz", 0.0)
            for index in range(1, len(expected_frequencies) + 1)
        ]
        if any(
            abs(measured - expected) > 1000.0
            for measured, expected in zip(measured_frequencies, expected_frequencies)
        ):
            pending = None
            continue

        frames.append({**pending, **diagnostic})
        evidence_lines.extend([pending_line, line])
        pending = None

    if len(frames) != frame_count:
        raise TimeoutError(
            f"稳定串口帧不足：收到{len(frames)}/{frame_count}帧"
        )
    return frames, evidence_lines[-6:]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fundamental-hz", type=float, default=60_000.0)
    parser.add_argument("--vpp-mv", type=float, default=200.0)
    parser.add_argument("--terms", default="1:0.6,3:0.2,6:0.2")
    parser.add_argument("--points", type=int, default=1000)
    parser.add_argument("--frames", type=int, default=12)
    parser.add_argument("--resource", default=DEFAULT_RESOURCE)
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument(
        "--output",
        default=(
            "outputs/019fb655-4b94-7fc2-a7a5-46aa763fc330/"
            "arb_expression_60k_200mV_test.json"
        ),
    )
    args = parser.parse_args()
    terms = parse_terms(args.terms)
    bsv, bsv_info = make_bsv(terms, args.points)
    raw_vpp = float(bsv_info["raw_vpp"])
    mv_per_unit = args.vpp_mv / raw_vpp
    expected_components = [
        {
            "order": order,
            "frequency_hz": order * args.fundamental_hz,
            "peak_mv": abs(coefficient) * mv_per_unit,
        }
        for order, coefficient in terms
    ]
    expected = {
        "upp_mv": args.vpp_mv,
        "rms_mv": mv_per_unit * math.sqrt(
            sum(coefficient * coefficient for _, coefficient in terms) / 2.0
        ),
        "components": expected_components,
    }

    with VisaInstrument(args.resource) as instrument:
        identity = instrument.query("*IDN?")
        instrument.write_bytes(
            b'WARB1:CARRier "codex_f60k_h1h3h6.bsv"\r\n',
            "WARB1:CARRier",
        )
        instrument.write_bytes(bsv, "BSV payload")
        time.sleep(1.2)
        commands = [
            "CHANnel2:LOAD 50",
            "CHANnel2:MODe CONTinue",
            "CHANnel2:BASE:WAVe DC",
            "CHANnel2:BASE:OFFSet 0",
            "CHANnel2:OUTPut ON",
            "CHANnel1:LOAD 50",
            "CHANnel1:MODe CONTinue",
            "CHANnel1:BASE:WAVe ARB",
            f"CHANnel1:BASE:FREQuency {args.fundamental_hz:.6f}",
            f"CHANnel1:BASE:AMPLitude {args.vpp_mv / 1000.0:.9f}",
            "CHANnel1:BASE:OFFSet 0",
            "CHANnel1:OUTPut ON",
            "SYSTEM:LOCK OFF",
        ]
        for command in commands:
            instrument.write_bytes(
                (command + "\r\n").encode("ascii"),
                command,
            )
            time.sleep(0.02)

        # 负载模式会引起2:1幅值标称换算，发送波形后必须回读确认。
        for channel in (1, 2):
            load_ohm = float(instrument.query(f"CHANnel{channel}:LOAD?"))
            if abs(load_ohm - 50.0) > 0.01:
                raise RuntimeError(f"CH{channel}负载不是50 Ω：{load_ohm}")

    with serial.Serial(args.port, args.baud, timeout=0.35) as port:
        port.reset_input_buffer()
        settle_deadline = time.monotonic() + 1.5
        while time.monotonic() < settle_deadline:
            port.read(port.in_waiting or 1)
        frames, evidence_lines = collect_stable_frames(
            port,
            [item["frequency_hz"] for item in expected_components],
            args.frames,
            8.0,
        )

    measured = {
        "upp_mv": statistics.median(frame["Upp_mV"] for frame in frames),
        "rms_mv": statistics.median(frame["U_mV"] for frame in frames),
        "components": [],
    }
    for index, component in enumerate(expected_components, start=1):
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

    comparisons = [
        {
            "name": "Upp",
            "unit": "mV",
            "expected": expected["upp_mv"],
            "measured": measured["upp_mv"],
            "error": measured["upp_mv"] - expected["upp_mv"],
            "limit": 5.0,
        },
        {
            "name": "Urms",
            "unit": "mV",
            "expected": expected["rms_mv"],
            "measured": measured["rms_mv"],
            "error": measured["rms_mv"] - expected["rms_mv"],
            "limit": 5.0,
        },
    ]
    for expected_component, measured_component in zip(
        expected_components,
        measured["components"],
    ):
        order = expected_component["order"]
        comparisons.extend(
            [
                {
                    "name": f"H{order} frequency",
                    "unit": "Hz",
                    "expected": expected_component["frequency_hz"],
                    "measured": measured_component["frequency_hz"],
                    "error": (
                        measured_component["frequency_hz"]
                        - expected_component["frequency_hz"]
                    ),
                    "limit": 1000.0,
                },
                {
                    "name": f"H{order} peak",
                    "unit": "mV",
                    "expected": expected_component["peak_mv"],
                    "measured": measured_component["peak_mv"],
                    "error": (
                        measured_component["peak_mv"]
                        - expected_component["peak_mv"]
                    ),
                    "limit": 5.0,
                },
            ]
        )
    for item in comparisons:
        item["passed"] = abs(float(item["error"])) <= float(item["limit"])

    result = {
        "expression": "0.6sin(2*pi*x)+0.2sin(6*pi*x)+0.2sin(12*pi*x)",
        "amplitude_definition": "overall waveform Vpp",
        "instrument": identity,
        "channel_1": {
            "fundamental_hz": args.fundamental_hz,
            "vpp_mv": args.vpp_mv,
            "offset_v": 0.0,
            "load_ohm": 50.0,
            "waveform": "ARB",
            "output": "ON",
        },
        "channel_2": {
            "waveform": "DC",
            "offset_v": 0.0,
            "load_ohm": 50.0,
            "output": "ON",
        },
        "bsv": bsv_info,
        "expected": expected,
        "measured": measured,
        "comparisons": comparisons,
        "stable_frame_count": len(frames),
        "adc_overrun_max": max(int(frame["adc_overrun"]) for frame in frames),
        "all_passed": all(bool(item["passed"]) for item in comparisons),
        "serial_evidence": evidence_lines,
    }
    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")

    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["all_passed"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
