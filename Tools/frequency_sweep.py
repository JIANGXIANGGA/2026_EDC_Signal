#!/usr/bin/env python3
"""通过 NI-VISA 和串口执行信号链路扫频测试。

信号源通道1输出固定幅值正弦波，通道2始终保持直流0 V并开启。
串口只记录固件诊断帧，原始采样数据不会被软件补偿结果覆盖。
"""

from __future__ import annotations

import argparse
import csv
import ctypes
import math
import sys
import time
from datetime import datetime
from pathlib import Path

import serial


DEFAULT_RESOURCE = "USB0::0x6656::0x0834::AWG1225380174::INSTR"
DEFAULT_PORT = "COM14"
DEFAULT_BAUD = 460800
ADC_INPUT_MV_PER_CODE = 0.802920
VI_ATTR_TMO_VALUE = 0x3FFF001A


class VisaError(RuntimeError):
    """VISA调用失败。"""


class VisaInstrument:
    """仅实现本测试需要的VISA打开、写入、读取和关闭。"""

    def __init__(self, resource: str) -> None:
        self._visa = ctypes.WinDLL("visa64.dll")
        self._resource_manager = ctypes.c_uint32()
        self._session = ctypes.c_uint32()
        self._configure_signatures()

        self._check(
            self._visa.viOpenDefaultRM(ctypes.byref(self._resource_manager)),
            "viOpenDefaultRM",
        )
        self._check(
            self._visa.viOpen(
                self._resource_manager,
                resource.encode("ascii"),
                0,
                3000,
                ctypes.byref(self._session),
            ),
            "viOpen",
        )
        self._check(
            self._visa.viSetAttribute(
                self._session,
                VI_ATTR_TMO_VALUE,
                ctypes.c_size_t(3000),
            ),
            "viSetAttribute(VI_ATTR_TMO_VALUE)",
        )

    def _configure_signatures(self) -> None:
        self._visa.viOpenDefaultRM.argtypes = [ctypes.POINTER(ctypes.c_uint32)]
        self._visa.viOpenDefaultRM.restype = ctypes.c_int32
        self._visa.viOpen.argtypes = [
            ctypes.c_uint32,
            ctypes.c_char_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self._visa.viOpen.restype = ctypes.c_int32
        self._visa.viSetAttribute.argtypes = [
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_size_t,
        ]
        self._visa.viSetAttribute.restype = ctypes.c_int32
        self._visa.viWrite.argtypes = [
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self._visa.viWrite.restype = ctypes.c_int32
        self._visa.viRead.argtypes = [
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self._visa.viRead.restype = ctypes.c_int32
        self._visa.viClose.argtypes = [ctypes.c_uint32]
        self._visa.viClose.restype = ctypes.c_int32

    @staticmethod
    def _check(status: int, operation: str) -> None:
        if status < 0:
            raise VisaError(f"{operation}失败，VISA状态码={status}")

    def write(self, command: str) -> None:
        payload = (command.rstrip("\r\n") + "\n").encode("ascii")
        self.write_bytes(payload, command)

    def write_bytes(self, payload: bytes, label: str = "binary payload") -> None:
        """写入原始字节流，供厂商BSV任意波协议复用。"""
        buffer = (ctypes.c_uint8 * len(payload)).from_buffer_copy(payload)
        written = ctypes.c_uint32()
        self._check(
            self._visa.viWrite(
                self._session,
                buffer,
                len(payload),
                ctypes.byref(written),
            ),
            f"viWrite({label})",
        )
        if written.value != len(payload):
            raise VisaError(
                f"VISA短写入：期望{len(payload)}字节，实际{written.value}字节"
            )

    def query(self, command: str) -> str:
        self.write(command)
        buffer = (ctypes.c_uint8 * 4096)()
        received = ctypes.c_uint32()
        self._check(
            self._visa.viRead(
                self._session,
                buffer,
                len(buffer),
                ctypes.byref(received),
            ),
            f"viRead({command})",
        )
        return bytes(buffer[: received.value]).decode("ascii", "replace").strip()

    def close(self) -> None:
        if self._session.value:
            self._visa.viClose(self._session)
            self._session = ctypes.c_uint32()
        if self._resource_manager.value:
            self._visa.viClose(self._resource_manager)
            self._resource_manager = ctypes.c_uint32()

    def __enter__(self) -> "VisaInstrument":
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.close()


def parse_diagnostic_line(line: str) -> dict[str, int] | None:
    """解析以seq开头的VOFA诊断帧。"""
    line = line.strip()
    if not line.startswith("seq:"):
        return None

    fields: dict[str, int] = {}
    for item in line.split(","):
        key, separator, value = item.partition(":")
        if not separator:
            continue
        try:
            fields[key] = int(value)
        except ValueError:
            return None

    required = {
        "seq",
        "raw_f1_hz",
        "raw_a1_code_x100",
        "a1_mv_x10",
        "upp_mv_x10",
        "urms_mv_x10",
        "adc_overrun",
    }
    return fields if required.issubset(fields) else None


def force_safe_generator_state(
    instrument: VisaInstrument,
    amplitude_vpp: float,
    frequency_hz: int,
) -> None:
    """设置扫频波形，并强制保持通道2的0 V直流输入。"""
    commands = [
        "CHANnel2:LOAD 50",
        "CHANnel2:MODe CONTinue",
        "CHANnel2:BASE:WAVe DC",
        "CHANnel2:BASE:OFFSet 0",
        "CHANnel2:OUTPut ON",
        "CHANnel1:LOAD 50",
        "CHANnel1:MODe CONTinue",
        "CHANnel1:BASE:WAVe SINe",
        f"CHANnel1:BASE:AMPLitude {amplitude_vpp:.9f}",
        "CHANnel1:BASE:OFFSet 0",
        f"CHANnel1:BASE:FREQuency {frequency_hz}",
        "CHANnel1:OUTPut ON",
    ]
    for command in commands:
        instrument.write(command)
        time.sleep(0.015)


def set_frequency(instrument: VisaInstrument, frequency_hz: int) -> None:
    instrument.write(f"CHANnel1:BASE:FREQuency {frequency_hz}")


def read_matching_samples(
    port: serial.Serial,
    target_frequency_hz: int,
    repeat_count: int,
    timeout_s: float,
) -> list[dict[str, int]]:
    """等待新频率稳定后，采集指定数量的不重复诊断帧。"""
    deadline = time.monotonic() + timeout_s
    samples: list[dict[str, int]] = []
    seen_sequences: set[int] = set()

    while time.monotonic() < deadline and len(samples) < repeat_count:
        line = port.readline().decode("ascii", "replace")
        fields = parse_diagnostic_line(line)
        if fields is None:
            continue
        if fields["seq"] in seen_sequences:
            continue
        if abs(fields["raw_f1_hz"] - target_frequency_hz) > 150:
            continue

        seen_sequences.add(fields["seq"])
        samples.append(fields)

    if len(samples) != repeat_count:
        last_frequency = samples[-1]["raw_f1_hz"] if samples else "无匹配帧"
        raise TimeoutError(
            f"{target_frequency_hz} Hz仅收到{len(samples)}/{repeat_count}帧，"
            f"末次频率={last_frequency}"
        )
    return samples


def existing_keys(csv_path: Path, stage: str) -> set[tuple[int, int]]:
    """读取已完成的频点/重复序号，使中断后可以续扫。"""
    keys: set[tuple[int, int]] = set()
    if not csv_path.exists():
        return keys

    with csv_path.open("r", newline="", encoding="utf-8-sig") as stream:
        for row in csv.DictReader(stream):
            if row.get("stage") != stage:
                continue
            keys.add((int(row["target_frequency_hz"]), int(row["repeat"])))
    return keys


def append_samples(
    csv_path: Path,
    stage: str,
    target_frequency_hz: int,
    expected_vpp_mv: float,
    samples: list[dict[str, int]],
) -> None:
    """追加原始帧及可复核的单位换算结果。"""
    fieldnames = [
        "stage",
        "timestamp",
        "target_frequency_hz",
        "repeat",
        "expected_peak_mv",
        "expected_vpp_mv",
        "expected_rms_mv",
        "seq",
        "raw_frequency_hz",
        "frequency_error_hz",
        "raw_peak_code_x100",
        "raw_peak_mv",
        "raw_peak_error_mv",
        "required_correction_gain",
        "firmware_peak_mv",
        "firmware_peak_error_mv",
        "firmware_vpp_mv",
        "firmware_vpp_error_mv",
        "firmware_rms_mv",
        "firmware_rms_error_mv",
        "peak_count",
        "signal_valid",
        "adc_min",
        "adc_max",
        "adc_overrun",
    ]
    expected_peak_mv = expected_vpp_mv / 2.0
    expected_rms_mv = expected_peak_mv / math.sqrt(2.0)
    needs_header = not csv_path.exists() or csv_path.stat().st_size == 0
    csv_path.parent.mkdir(parents=True, exist_ok=True)

    with csv_path.open("a", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        if needs_header:
            writer.writeheader()
        for repeat, fields in enumerate(samples, start=1):
            raw_peak_mv = (
                fields["raw_a1_code_x100"] / 100.0 * ADC_INPUT_MV_PER_CODE
            )
            firmware_peak_mv = fields["a1_mv_x10"] / 10.0
            firmware_vpp_mv = fields["upp_mv_x10"] / 10.0
            firmware_rms_mv = fields["urms_mv_x10"] / 10.0
            writer.writerow(
                {
                    "stage": stage,
                    "timestamp": datetime.now().isoformat(timespec="milliseconds"),
                    "target_frequency_hz": target_frequency_hz,
                    "repeat": repeat,
                    "expected_peak_mv": f"{expected_peak_mv:.6f}",
                    "expected_vpp_mv": f"{expected_vpp_mv:.6f}",
                    "expected_rms_mv": f"{expected_rms_mv:.6f}",
                    "seq": fields["seq"],
                    "raw_frequency_hz": fields["raw_f1_hz"],
                    "frequency_error_hz": (
                        fields["raw_f1_hz"] - target_frequency_hz
                    ),
                    "raw_peak_code_x100": fields["raw_a1_code_x100"],
                    "raw_peak_mv": f"{raw_peak_mv:.6f}",
                    "raw_peak_error_mv": f"{raw_peak_mv - expected_peak_mv:.6f}",
                    "required_correction_gain": (
                        f"{expected_peak_mv / raw_peak_mv:.9f}"
                    ),
                    "firmware_peak_mv": f"{firmware_peak_mv:.6f}",
                    "firmware_peak_error_mv": (
                        f"{firmware_peak_mv - expected_peak_mv:.6f}"
                    ),
                    "firmware_vpp_mv": f"{firmware_vpp_mv:.6f}",
                    "firmware_vpp_error_mv": (
                        f"{firmware_vpp_mv - expected_vpp_mv:.6f}"
                    ),
                    "firmware_rms_mv": f"{firmware_rms_mv:.6f}",
                    "firmware_rms_error_mv": (
                        f"{firmware_rms_mv - expected_rms_mv:.6f}"
                    ),
                    "peak_count": fields.get("peak_count", 0),
                    "signal_valid": fields.get("signal_valid", 0),
                    "adc_min": fields.get("adc_min", 0),
                    "adc_max": fields.get("adc_max", 0),
                    "adc_overrun": fields["adc_overrun"],
                }
            )
        stream.flush()


def run_probe(args: argparse.Namespace) -> None:
    """只验证仪器身份和串口诊断帧，不改变波形。"""
    with VisaInstrument(args.resource) as instrument:
        print(f"VISA_IDN={instrument.query('*IDN?')}")

    with serial.Serial(args.port, args.baud, timeout=0.5) as port:
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            fields = parse_diagnostic_line(
                port.readline().decode("ascii", "replace")
            )
            if fields is not None:
                print(
                    "SERIAL_OK="
                    f"seq:{fields['seq']},raw_f1_hz:{fields['raw_f1_hz']},"
                    f"adc_overrun:{fields['adc_overrun']}"
                )
                return
    raise TimeoutError("3秒内未收到有效串口诊断帧")


def run_sweep(args: argparse.Namespace) -> None:
    """执行扫频，可按起止频率分段，也可从CSV自动续扫。"""
    csv_path = Path(args.output).resolve()
    completed = existing_keys(csv_path, args.stage)
    frequencies = list(range(args.start, args.stop + 1, args.step))

    with VisaInstrument(args.resource) as instrument, serial.Serial(
        args.port,
        args.baud,
        timeout=0.35,
    ) as port:
        print(f"VISA_IDN={instrument.query('*IDN?')}", flush=True)
        force_safe_generator_state(
            instrument,
            args.amplitude_vpp,
            frequencies[0],
        )
        port.reset_input_buffer()

        completed_points = 0
        started = time.monotonic()
        for index, frequency_hz in enumerate(frequencies, start=1):
            if all(
                (frequency_hz, repeat) in completed
                for repeat in range(1, args.repeats + 1)
            ):
                completed_points += 1
                continue

            set_frequency(instrument, frequency_hz)
            port.reset_input_buffer()
            try:
                samples = read_matching_samples(
                    port,
                    frequency_hz,
                    args.repeats,
                    args.timeout,
                )
            except TimeoutError:
                # 再次写入频率并清空旧帧，避免一次USB或串口抖动中断全扫。
                set_frequency(instrument, frequency_hz)
                port.reset_input_buffer()
                samples = read_matching_samples(
                    port,
                    frequency_hz,
                    args.repeats,
                    args.timeout,
                )

            append_samples(
                csv_path,
                args.stage,
                frequency_hz,
                args.amplitude_vpp * 1000.0,
                samples,
            )
            completed_points += 1

            if (completed_points % args.progress_every == 0) or (
                index == len(frequencies)
            ):
                elapsed = time.monotonic() - started
                print(
                    f"PROGRESS stage={args.stage} points={completed_points}/"
                    f"{len(frequencies)} frequency_hz={frequency_hz} "
                    f"elapsed_s={elapsed:.1f}",
                    flush=True,
                )

        # 扫频结束仍保留用户要求的通道2工作状态。
        instrument.write("CHANnel2:LOAD 50")
        instrument.write("CHANnel2:BASE:WAVe DC")
        instrument.write("CHANnel2:BASE:OFFSet 0")
        instrument.write("CHANnel2:OUTPut ON")

    print(
        f"SWEEP_DONE stage={args.stage} rows={len(frequencies) * args.repeats} "
        f"output={csv_path}",
        flush=True,
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", action="store_true", help="只检查VISA和串口")
    parser.add_argument("--resource", default=DEFAULT_RESOURCE)
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--start", type=int, default=10_000)
    parser.add_argument("--stop", type=int, default=500_000)
    parser.add_argument("--step", type=int, default=500)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--amplitude-vpp", type=float, default=0.200)
    parser.add_argument("--stage", default="before")
    parser.add_argument("--timeout", type=float, default=2.5)
    parser.add_argument("--progress-every", type=int, default=25)
    parser.add_argument(
        "--output",
        default=(
            "outputs/019fb655-4b94-7fc2-a7a5-46aa763fc330/"
            "frequency_sweep_raw.csv"
        ),
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.step <= 0 or args.repeats <= 0:
        raise ValueError("step和repeats必须为正数")
    if args.start > args.stop or (args.stop - args.start) % args.step != 0:
        raise ValueError("起止频率必须能被step完整覆盖")

    if args.probe:
        run_probe(args)
    else:
        run_sweep(args)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (VisaError, TimeoutError, serial.SerialException, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr, flush=True)
        raise SystemExit(1)
