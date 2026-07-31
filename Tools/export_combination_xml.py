#!/usr/bin/env python3
"""把组合波函数库和实测结果导出为 Excel 可直接打开的 XML 表格。"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
from pathlib import Path
from xml.etree import ElementTree as ET


SS = "urn:schemas-microsoft-com:office:spreadsheet"
O = "urn:schemas-microsoft-com:office:office"
X = "urn:schemas-microsoft-com:office:excel"
ET.register_namespace("", SS)
ET.register_namespace("o", O)
ET.register_namespace("x", X)
ET.register_namespace("ss", SS)


def attribute(name: str) -> str:
    return f"{{{SS}}}{name}"


def clean_text(value: object) -> str:
    """删除 XML 1.0 不允许的控制字符。"""
    return re.sub(
        r"[^\x09\x0A\x0D\x20-\uD7FF\uE000-\uFFFD]",
        "",
        str(value),
    )


def cell_type(value: object) -> tuple[str, str]:
    text = clean_text(value)
    if text == "":
        return "String", ""
    try:
        number = float(text)
        if math.isfinite(number):
            return "Number", format(number, ".15g")
    except (TypeError, ValueError):
        pass
    return "String", text


def add_cell(
    row: ET.Element,
    value: object,
    style_id: str = "",
) -> None:
    cell = ET.SubElement(row, f"{{{SS}}}Cell")
    if style_id:
        cell.set(attribute("StyleID"), style_id)
    data_type, text = cell_type(value)
    data = ET.SubElement(cell, f"{{{SS}}}Data")
    data.set(attribute("Type"), data_type)
    data.text = text


def add_styles(workbook: ET.Element) -> None:
    styles = ET.SubElement(workbook, f"{{{SS}}}Styles")

    default = ET.SubElement(styles, f"{{{SS}}}Style")
    default.set(attribute("ID"), "Default")
    ET.SubElement(default, f"{{{SS}}}Alignment").set(
        attribute("Vertical"), "Center"
    )
    ET.SubElement(default, f"{{{SS}}}Font").set(
        attribute("FontName"), "Microsoft YaHei"
    )

    header = ET.SubElement(styles, f"{{{SS}}}Style")
    header.set(attribute("ID"), "Header")
    ET.SubElement(header, f"{{{SS}}}Alignment", {
        attribute("Horizontal"): "Center",
        attribute("Vertical"): "Center",
        attribute("WrapText"): "1",
    })
    ET.SubElement(header, f"{{{SS}}}Font", {
        attribute("FontName"): "Microsoft YaHei",
        attribute("Bold"): "1",
        attribute("Color"): "#FFFFFF",
    })
    ET.SubElement(header, f"{{{SS}}}Interior", {
        attribute("Color"): "#0F766E",
        attribute("Pattern"): "Solid",
    })

    for style_id, color, font_color in (
        ("Pass", "#DCFCE7", "#166534"),
        ("Fail", "#FEE2E2", "#991B1B"),
        ("Error", "#FEF3C7", "#92400E"),
        ("Title", "#DBEAFE", "#1E3A8A"),
    ):
        style = ET.SubElement(styles, f"{{{SS}}}Style")
        style.set(attribute("ID"), style_id)
        ET.SubElement(style, f"{{{SS}}}Font", {
            attribute("FontName"): "Microsoft YaHei",
            attribute("Bold"): "1",
            attribute("Color"): font_color,
        })
        ET.SubElement(style, f"{{{SS}}}Interior", {
            attribute("Color"): color,
            attribute("Pattern"): "Solid",
        })


def add_sheet(
    workbook: ET.Element,
    name: str,
    headers: list[str],
    rows: list[dict[str, object]],
    status_field: str = "",
) -> None:
    worksheet = ET.SubElement(workbook, f"{{{SS}}}Worksheet")
    worksheet.set(attribute("Name"), name[:31])
    table = ET.SubElement(worksheet, f"{{{SS}}}Table")
    table.set(attribute("ExpandedColumnCount"), str(len(headers)))
    table.set(attribute("ExpandedRowCount"), str(len(rows) + 1))

    for header in headers:
        column = ET.SubElement(table, f"{{{SS}}}Column")
        width = 170 if header in {"expression", "error_message"} else 92
        if header in {"function_id", "case_id", "status"}:
            width = 75
        column.set(attribute("Width"), str(width))

    header_row = ET.SubElement(table, f"{{{SS}}}Row")
    header_row.set(attribute("Height"), "32")
    for header in headers:
        add_cell(header_row, header, "Header")

    for item in rows:
        row = ET.SubElement(table, f"{{{SS}}}Row")
        status = str(item.get(status_field, "")) if status_field else ""
        status_style = {
            "PASS": "Pass",
            "FAIL": "Fail",
            "ERROR": "Error",
        }.get(status, "")
        for header in headers:
            add_cell(
                row,
                item.get(header, ""),
                status_style if header == status_field else "",
            )

    options = ET.SubElement(worksheet, f"{{{X}}}WorksheetOptions")
    ET.SubElement(options, f"{{{X}}}FreezePanes")
    ET.SubElement(options, f"{{{X}}}FrozenNoSplit")
    ET.SubElement(options, f"{{{X}}}SplitHorizontal").text = "1"
    ET.SubElement(options, f"{{{X}}}TopRowBottomPane").text = "1"
    ET.SubElement(options, f"{{{X}}}ActivePane").text = "2"


def load_csv(path: Path) -> list[dict[str, str]]:
    with path.open("r", newline="", encoding="utf-8-sig") as stream:
        return list(csv.DictReader(stream))


def latest_stage_rows(rows: list[dict[str, str]], stage: str) -> list[dict[str, str]]:
    latest: dict[str, dict[str, str]] = {}
    for row in rows:
        if row.get("stage") == stage:
            latest[row["case_id"]] = row
    return sorted(latest.values(), key=lambda item: item["case_id"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--functions", required=True)
    parser.add_argument("--results", required=True)
    parser.add_argument("--summary", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    function_path = Path(args.functions)
    result_path = Path(args.results)
    summary_path = Path(args.summary)
    output_path = Path(args.output)
    functions = load_csv(function_path)
    results = latest_stage_rows(load_csv(result_path), args.stage)
    summary = json.loads(summary_path.read_text(encoding="utf-8"))

    summary_rows = [
        {"项目": "测试阶段", "数值": args.stage, "单位/说明": ""},
        {"项目": "函数数量", "数值": len(functions), "单位/说明": "个"},
        {
            "项目": "计划测试用例",
            "数值": summary.get("expected_case_count", 0),
            "单位/说明": "每个函数 3 组",
        },
        {
            "项目": "完成用例",
            "数值": summary.get("completed_case_count", 0),
            "单位/说明": "组",
        },
        {"项目": "通过", "数值": summary.get("pass_count", 0), "单位/说明": "组"},
        {"项目": "失败", "数值": summary.get("fail_count", 0), "单位/说明": "组"},
        {"项目": "采集错误", "数值": summary.get("error_count", 0), "单位/说明": "组"},
        {
            "项目": "通过率",
            "数值": summary.get("pass_rate", 0.0),
            "单位/说明": "比例",
        },
        {
            "项目": "最大绝对电压误差",
            "数值": summary.get("max_abs_voltage_error_mv", 0.0),
            "单位/说明": "mV，限值 5 mV",
        },
        {
            "项目": "最大绝对频率误差",
            "数值": summary.get("max_abs_frequency_error_hz", 0.0),
            "单位/说明": "Hz，限值 1000 Hz",
        },
        {
            "项目": "ADC DMA 溢出最大值",
            "数值": summary.get("adc_overrun_max", 0),
            "单位/说明": "次",
        },
        {"项目": "相位", "数值": 0, "单位/说明": "全部分量 0°"},
        {"项目": "直流偏移", "数值": 0, "单位/说明": "信号源 0 V"},
        {"项目": "频率范围", "数值": "50–500 kHz", "单位/说明": "全部分量"},
        {"项目": "峰峰值范围", "数值": "50–250 mV", "单位/说明": "总波形 Upp"},
    ]

    workbook = ET.Element(f"{{{SS}}}Workbook")
    properties = ET.SubElement(workbook, f"{{{O}}}DocumentProperties")
    ET.SubElement(properties, f"{{{O}}}Author").text = "Codex"
    ET.SubElement(properties, f"{{{O}}}Title").text = "STM32G474 组合波实测报告"
    add_styles(workbook)
    add_sheet(workbook, "测试汇总", ["项目", "数值", "单位/说明"], summary_rows)
    add_sheet(workbook, "函数库", list(functions[0]) if functions else [], functions)
    add_sheet(
        workbook,
        "组合波实测结果",
        list(results[0]) if results else [],
        results,
        "status",
    )
    failed = [item for item in results if item.get("status") != "PASS"]
    add_sheet(
        workbook,
        "超差与错误",
        list(results[0]) if results else [],
        failed,
        "status",
    )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    tree = ET.ElementTree(workbook)
    ET.indent(tree, space="  ")
    tree.write(output_path, encoding="utf-8", xml_declaration=True)
    print(
        f"XML_DONE functions={len(functions)} results={len(results)} "
        f"failed={len(failed)} output={output_path.resolve()}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
