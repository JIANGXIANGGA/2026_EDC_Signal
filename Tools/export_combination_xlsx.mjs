#!/usr/bin/env node

import fs from "node:fs/promises";
import path from "node:path";
import process from "node:process";

import { SpreadsheetFile, Workbook } from "@oai/artifact-tool";

const VOLTAGE_LIMIT_MV = 5;
const FREQUENCY_LIMIT_HZ = 1000;
const FIRMWARE_SHA256 =
  "1A5B562CCEE6FCAFBCFA95F5A4B1AF2F510069BC1BC7FD5463CB530373F6233E";

const RESULT_HEADERS = [
  "测试阶段",
  "时间戳",
  "函数ID",
  "用例ID",
  "组合序号",
  "函数表达式",
  "分量数",
  "谐波阶次",
  "幅值系数",
  "基波频率(Hz)",
  "设定Upp(mV)",
  "偏移(V)",
  "相位(deg)",
  "理论Upp(mV)",
  "实测Upp(mV)",
  "Upp误差(mV)",
  "理论RMS(mV)",
  "实测RMS(mV)",
  "RMS误差(mV)",
  "H1阶次",
  "H1理论频率(Hz)",
  "H1实测频率(Hz)",
  "H1频率误差(Hz)",
  "H1理论峰值(mV)",
  "H1实测峰值(mV)",
  "H1峰值误差(mV)",
  "H2阶次",
  "H2理论频率(Hz)",
  "H2实测频率(Hz)",
  "H2频率误差(Hz)",
  "H2理论峰值(mV)",
  "H2实测峰值(mV)",
  "H2峰值误差(mV)",
  "H3阶次",
  "H3理论频率(Hz)",
  "H3实测频率(Hz)",
  "H3频率误差(Hz)",
  "H3理论峰值(mV)",
  "H3实测峰值(mV)",
  "H3峰值误差(mV)",
  "实测分量数",
  "信号有效",
  "峰值数量",
  "ADC溢出次数",
  "稳定帧数",
  "严格频率匹配",
  "最大电压误差(mV)",
  "最大频率误差(Hz)",
  "状态",
  "错误信息",
];

const FUNCTION_HEADERS = [
  "函数ID",
  "函数表达式",
  "分量数",
  "谐波阶次",
  "幅值系数",
  "原始Upp单位",
  "组合1基波频率(Hz)",
  "组合1Upp(mV)",
  "组合2基波频率(Hz)",
  "组合2Upp(mV)",
  "组合3基波频率(Hz)",
  "组合3Upp(mV)",
];

function parseArgs(argv) {
  const args = {};
  for (let index = 0; index < argv.length; index += 2) {
    const key = argv[index];
    const value = argv[index + 1];
    if (!key?.startsWith("--") || value === undefined) {
      throw new Error(`参数格式错误: ${key ?? ""}`);
    }
    args[key.slice(2)] = value;
  }
  for (const required of ["functions", "results", "summary", "output", "work-dir"]) {
    if (!args[required]) {
      throw new Error(`缺少参数 --${required}`);
    }
  }
  return args;
}

async function readCsv(filePath) {
  const csvText = await fs.readFile(filePath, "utf8");
  const imported = await Workbook.fromCSV(csvText, { sheetName: "CSV" });
  const values = imported.worksheets.getItem("CSV").getUsedRange(true).values;
  const headers = values[0].map((value, index) =>
    index === 0 ? String(value).replace(/^\uFEFF/, "") : String(value),
  );
  return {
    headers,
    rows: values.slice(1).map((row) =>
      Object.fromEntries(headers.map((header, index) => [header, row[index] ?? null])),
    ),
  };
}

function selectRows(allRows, stage) {
  const stageRows = allRows.filter((row) => row.stage === stage);
  const lastIndex = new Map();
  stageRows.forEach((row, index) => lastIndex.set(row.case_id, index));

  const finalRows = stageRows
    .filter((row, index) => lastIndex.get(row.case_id) === index)
    .sort((left, right) => String(left.case_id).localeCompare(String(right.case_id)));
  const historyRows = stageRows.filter(
    (row, index) => lastIndex.get(row.case_id) !== index,
  );
  return { finalRows, historyRows };
}

function resultMatrix(rows, sourceHeaders) {
  const formulaFields = new Set([
    "upp_error_mv",
    "rms_error_mv",
    "h1_frequency_error_hz",
    "h1_peak_error_mv",
    "h2_frequency_error_hz",
    "h2_peak_error_mv",
    "h3_frequency_error_hz",
    "h3_peak_error_mv",
    "max_abs_voltage_error_mv",
    "max_abs_frequency_error_hz",
    "status",
  ]);
  return rows.map((row) =>
    sourceHeaders.map((header) => (formulaFields.has(header) ? null : row[header])),
  );
}

function applyResultFormulas(sheet, rowCount) {
  if (rowCount === 0) {
    return;
  }
  const lastRow = rowCount + 1;
  const formulas = new Map([
    ["P", '=IF(RC[34]<>"",0,RC[-1]-RC[-2])'],
    ["S", '=IF(RC[31]<>"",0,RC[-1]-RC[-2])'],
    ["W", '=IF(RC[27]<>"",0,RC[-1]-RC[-2])'],
    ["Z", '=IF(RC[24]<>"",0,RC[-1]-RC[-2])'],
    ["AD", '=IF(RC[20]<>"",0,RC[-1]-RC[-2])'],
    ["AG", '=IF(RC[17]<>"",0,RC[-1]-RC[-2])'],
    ["AK", '=IF(RC[13]<>"",0,IF(RC[-3]="",0,RC[-1]-RC[-2]))'],
    ["AN", '=IF(RC[10]<>"",0,IF(RC[-2]="",0,RC[-1]-RC[-2]))'],
    [
      "AU",
      '=IF(RC[3]<>"",0,MAX(ABS(RC[-31]),ABS(RC[-28]),ABS(RC[-21]),ABS(RC[-14]),ABS(RC[-7])))',
    ],
    ["AV", '=IF(RC[2]<>"",0,MAX(ABS(RC[-25]),ABS(RC[-18]),ABS(RC[-11])))'],
    [
      "AW",
      `=IF(RC[1]<>\"\",\"ERROR\",IF(AND(ABS(RC[-33])<=${VOLTAGE_LIMIT_MV},ABS(RC[-30])<=${VOLTAGE_LIMIT_MV},ABS(RC[-23])<=${VOLTAGE_LIMIT_MV},ABS(RC[-16])<=${VOLTAGE_LIMIT_MV},OR(RC[-42]=2,ABS(RC[-9])<=${VOLTAGE_LIMIT_MV}),RC[-1]<=${FREQUENCY_LIMIT_HZ},RC[-8]=RC[-42],RC[-7]=1,RC[-5]=0),\"PASS\",\"FAIL\"))`,
    ],
  ]);

  for (const [column, formula] of formulas) {
    sheet.getRange(`${column}2`).formulasR1C1 = [[formula]];
    sheet.getRange(`${column}2:${column}${lastRow}`).fillDown();
  }
}

function styleTableSheet(sheet, rowCount, lastColumn, freezeColumns) {
  const lastRow = rowCount + 1;
  const used = sheet.getRange(`A1:${lastColumn}${lastRow}`);
  used.format.font = { name: "Microsoft YaHei", size: 9, color: "#1F2937" };
  used.format.verticalAlignment = "center";
  sheet.getRange(`A1:${lastColumn}1`).format = {
    fill: "#0F766E",
    font: { name: "Microsoft YaHei", size: 9, bold: true, color: "#FFFFFF" },
    horizontalAlignment: "center",
    verticalAlignment: "center",
    wrapText: true,
    rowHeight: 42,
    borders: { preset: "outside", style: "thin", color: "#0B5F58" },
  };
  sheet.getRange(`A2:${lastColumn}${lastRow}`).format.rowHeight = 19;
  sheet.freezePanes.freezeRows(1);
  sheet.freezePanes.freezeColumns(freezeColumns);
  sheet.showGridLines = false;
}

function setResultWidths(sheet, includeRetryColumn = false) {
  const widths = [
    ["A", 235], ["B", 170], ["C:D", 90], ["E", 65], ["F", 520],
    ["G", 65], ["H:I", 95], ["J:K", 110], ["L:M", 75],
    ["N:S", 100], ["T:AN", 94], ["AO:AT", 82], ["AU:AW", 108],
    ["AX", 260],
  ];
  if (includeRetryColumn) {
    widths.push(["AY", 115]);
  }
  for (const [range, widthPx] of widths) {
    sheet.getRange(range).format.columnWidthPx = widthPx;
  }
}

function formatResultNumbers(sheet, rowCount, includeRetryColumn = false) {
  const lastRow = rowCount + 1;
  if (rowCount === 0) {
    return;
  }
  for (const range of ["J2:J", "U2:W", "AB2:AD", "AI2:AK", "AV2:AV"]) {
    sheet.getRange(`${range}${lastRow}`).format.numberFormat = "#,##0";
  }
  for (const range of ["K2:K", "N2:P", "Q2:S", "X2:Z", "AE2:AG", "AL2:AN", "AU2:AU"]) {
    sheet.getRange(`${range}${lastRow}`).format.numberFormat = "0.000";
  }
  sheet.getRange(`L2:M${lastRow}`).format.numberFormat = "0.000";
  sheet.getRange(`P2:P${lastRow}`).format.fill = "#EFF6FF";
  sheet.getRange(`S2:S${lastRow}`).format.fill = "#EFF6FF";
  sheet.getRange(`W2:W${lastRow}`).format.fill = "#EFF6FF";
  sheet.getRange(`Z2:Z${lastRow}`).format.fill = "#EFF6FF";
  sheet.getRange(`AD2:AD${lastRow}`).format.fill = "#EFF6FF";
  sheet.getRange(`AG2:AG${lastRow}`).format.fill = "#EFF6FF";
  sheet.getRange(`AK2:AK${lastRow}`).format.fill = "#EFF6FF";
  sheet.getRange(`AN2:AN${lastRow}`).format.fill = "#EFF6FF";
  sheet.getRange(`AU2:AW${lastRow}`).format.fill = "#F0FDFA";

  sheet.getRange(`AW2:AW${lastRow}`).conditionalFormats.add("containsText", {
    text: "PASS",
    format: { fill: "#DCFCE7", font: { bold: true, color: "#166534" } },
  });
  sheet.getRange(`AW2:AW${lastRow}`).conditionalFormats.add("containsText", {
    text: "FAIL",
    format: { fill: "#FEE2E2", font: { bold: true, color: "#991B1B" } },
  });
  sheet.getRange(`AW2:AW${lastRow}`).conditionalFormats.add("containsText", {
    text: "ERROR",
    format: { fill: "#FEF3C7", font: { bold: true, color: "#92400E" } },
  });
  sheet.getRange(`AU2:AU${lastRow}`).conditionalFormats.add("cellIs", {
    operator: "greaterThan",
    formula: VOLTAGE_LIMIT_MV,
    format: { fill: "#FEE2E2", font: { color: "#991B1B" } },
  });
  sheet.getRange(`AV2:AV${lastRow}`).conditionalFormats.add("cellIs", {
    operator: "greaterThan",
    formula: FREQUENCY_LIMIT_HZ,
    format: { fill: "#FEE2E2", font: { color: "#991B1B" } },
  });
  if (includeRetryColumn) {
    sheet.getRange(`AY2:AY${lastRow}`).format.fill = "#FFF7ED";
  }
}

function addResultSheet(workbook, name, rows, sourceHeaders, tableName, history = false) {
  const sheet = workbook.worksheets.add(name);
  const headers = history ? [...RESULT_HEADERS, "重测结果"] : RESULT_HEADERS;
  sheet.getRangeByIndexes(0, 0, 1, headers.length).values = [headers];
  if (rows.length > 0) {
    const values = resultMatrix(rows, sourceHeaders);
    if (history) {
      values.forEach((row) => row.push(null));
    }
    sheet.getRangeByIndexes(1, 0, values.length, headers.length).values = values;
    applyResultFormulas(sheet, rows.length);
    if (history) {
      sheet.getRange("AY2").formulas = [[
        '=IF(COUNTIFS(\'1500最终结果\'!$D$2:$D$1501,D2,\'1500最终结果\'!$AW$2:$AW$1501,"PASS")>0,"已重测通过","未通过")',
      ]];
      sheet.getRange(`AY2:AY${rows.length + 1}`).fillDown();
    }
  }
  const lastColumn = history ? "AY" : "AX";
  styleTableSheet(sheet, rows.length, lastColumn, 4);
  setResultWidths(sheet, history);
  formatResultNumbers(sheet, rows.length, history);
  const table = sheet.tables.add(`A1:${lastColumn}${rows.length + 1}`, true, tableName);
  table.style = "TableStyleMedium2";
  return sheet;
}

function addFunctionSheet(workbook, rows, sourceHeaders) {
  const sheet = workbook.worksheets.add("500函数库");
  sheet.getRange("A1:L1").values = [FUNCTION_HEADERS];
  const values = rows.map((row) => sourceHeaders.map((header) => row[header]));
  sheet.getRangeByIndexes(1, 0, values.length, 12).values = values;
  styleTableSheet(sheet, rows.length, "L", 2);
  for (const [range, widthPx] of [
    ["A", 82], ["B", 520], ["C", 70], ["D:E", 105], ["F", 100],
    ["G:L", 125],
  ]) {
    sheet.getRange(range).format.columnWidthPx = widthPx;
  }
  sheet.getRange(`F2:F${rows.length + 1}`).format.numberFormat = "0.000000";
  for (const range of ["G2:G", "I2:I", "K2:K"]) {
    sheet.getRange(`${range}${rows.length + 1}`).format.numberFormat = "#,##0";
  }
  for (const range of ["H2:H", "J2:J", "L2:L"]) {
    sheet.getRange(`${range}${rows.length + 1}`).format.numberFormat = "0.0";
  }
  const table = sheet.tables.add(`A1:L${rows.length + 1}`, true, "FunctionLibrary");
  table.style = "TableStyleMedium2";
  return sheet;
}

function addSummarySheet(workbook, stage, finalRowCount, historyRowCount) {
  const sheet = workbook.worksheets.add("测试汇总");
  sheet.getRange("A1:D1").values = [["STM32G474 组合波实机测试汇总", null, null, null]];
  sheet.getRange("A2:B3").values = [
    ["测试阶段", stage],
    ["固件 SHA-256", FIRMWARE_SHA256],
  ];
  sheet.getRange("A5:D5").values = [["项目", "数值", "单位", "判定/说明"]];
  sheet.getRange("A6:D27").values = [
    ["函数数量", null, "个", "唯一函数"],
    ["计划测试用例", null, "组", "每个函数 3 组"],
    ["完成测试用例", null, "组", "最终有效记录"],
    ["通过", null, "组", "公式统计"],
    ["失败/错误", null, "组", "最终有效记录"],
    ["通过率", null, "%", "目标 100%"],
    ["首轮超差/异常", null, "条", "均已自动重测"],
    ["最大绝对电压误差", null, "mV", `限值 ${VOLTAGE_LIMIT_MV} mV`],
    ["P95 最大电压误差", null, "mV", "最终有效记录"],
    ["Upp 最大误差", null, "mV", `限值 ${VOLTAGE_LIMIT_MV} mV`],
    ["RMS 最大误差", null, "mV", `限值 ${VOLTAGE_LIMIT_MV} mV`],
    ["H1 峰值最大误差", null, "mV", `限值 ${VOLTAGE_LIMIT_MV} mV`],
    ["H2 峰值最大误差", null, "mV", `限值 ${VOLTAGE_LIMIT_MV} mV`],
    ["H3 峰值最大误差", null, "mV", `限值 ${VOLTAGE_LIMIT_MV} mV`],
    ["最大频率误差", null, "Hz", `限值 ${FREQUENCY_LIMIT_HZ} Hz`],
    ["ADC DMA 溢出最大值", null, "次", "要求为 0"],
    ["严格频率匹配为否", null, "组", "仍需满足频率误差限值"],
    ["相位", 0, "deg", "全部分量"],
    ["直流偏移", 0, "V", "信号源设置"],
    ["频率范围", "50-500", "kHz", "全部分量"],
    ["峰峰值范围", "50-250", "mV", "总波形 Upp"],
    ["最终结论", null, "", "公式判定"],
  ];
  const formulas = [
    "=COUNTA('500函数库'!$A$2:$A$501)",
    "=B6*3",
    `=COUNTA('1500最终结果'!$D$2:$D$${finalRowCount + 1})`,
    `=COUNTIF('1500最终结果'!$AW$2:$AW$${finalRowCount + 1},\"PASS\")`,
    `=COUNTIF('1500最终结果'!$AW$2:$AW$${finalRowCount + 1},\"FAIL\")+COUNTIF('1500最终结果'!$AW$2:$AW$${finalRowCount + 1},\"ERROR\")`,
    "=IF(B8=0,0,B9/B8)",
    `=COUNTA('超差重测记录'!$D$2:$D$${historyRowCount + 1})`,
    `=MAX('1500最终结果'!$AU$2:$AU$${finalRowCount + 1})`,
    `=PERCENTILE.INC('1500最终结果'!$AU$2:$AU$${finalRowCount + 1},0.95)`,
    `=MAX(MAX('1500最终结果'!$P$2:$P$${finalRowCount + 1}),-MIN('1500最终结果'!$P$2:$P$${finalRowCount + 1}))`,
    `=MAX(MAX('1500最终结果'!$S$2:$S$${finalRowCount + 1}),-MIN('1500最终结果'!$S$2:$S$${finalRowCount + 1}))`,
    `=MAX(MAX('1500最终结果'!$Z$2:$Z$${finalRowCount + 1}),-MIN('1500最终结果'!$Z$2:$Z$${finalRowCount + 1}))`,
    `=MAX(MAX('1500最终结果'!$AG$2:$AG$${finalRowCount + 1}),-MIN('1500最终结果'!$AG$2:$AG$${finalRowCount + 1}))`,
    `=MAX(MAX('1500最终结果'!$AN$2:$AN$${finalRowCount + 1}),-MIN('1500最终结果'!$AN$2:$AN$${finalRowCount + 1}))`,
    `=MAX('1500最终结果'!$AV$2:$AV$${finalRowCount + 1})`,
    `=MAX('1500最终结果'!$AR$2:$AR$${finalRowCount + 1})`,
    `=COUNTIF('1500最终结果'!$AT$2:$AT$${finalRowCount + 1},0)`,
  ];
  sheet.getRange("B6:B22").formulas = formulas.map((formula) => [formula]);
  sheet.getRange("B27").formulas = [[
    `=IF(AND(B8=B7,B9=B8,B10=0,B13<=${VOLTAGE_LIMIT_MV},B20<=${FREQUENCY_LIMIT_HZ},B21=0),\"全部通过\",\"需复核\")`,
  ]];

  sheet.showGridLines = false;
  sheet.freezePanes.freezeRows(5);
  sheet.getRange("A1:D27").format.font = {
    name: "Microsoft YaHei",
    size: 10,
    color: "#1F2937",
  };
  sheet.getRange("A1:D1").format = {
    fill: "#134E4A",
    font: { name: "Microsoft YaHei", size: 18, bold: true, color: "#FFFFFF" },
    rowHeight: 36,
    verticalAlignment: "center",
  };
  sheet.getRange("A2:A3").format.font = { bold: true, color: "#115E59" };
  sheet.getRange("A5:D5").format = {
    fill: "#0F766E",
    font: { name: "Microsoft YaHei", size: 10, bold: true, color: "#FFFFFF" },
    horizontalAlignment: "center",
    rowHeight: 26,
    borders: { preset: "outside", style: "thin", color: "#0B5F58" },
  };
  sheet.getRange("A6:D27").format.borders = {
    insideHorizontal: { style: "thin", color: "#D1D5DB" },
  };
  sheet.getRange("B6:B22").format.fill = "#EFF6FF";
  sheet.getRange("B11").format.numberFormat = "0.00%";
  sheet.getRange("B13:B19").format.numberFormat = "0.000";
  sheet.getRange("B20:B22").format.numberFormat = "#,##0";
  sheet.getRange("A27:D27").format = {
    fill: "#F0FDFA",
    font: { name: "Microsoft YaHei", size: 11, bold: true, color: "#115E59" },
    borders: { preset: "outside", style: "medium", color: "#0F766E" },
    rowHeight: 28,
  };
  sheet.getRange("B27").conditionalFormats.add("containsText", {
    text: "全部通过",
    format: { fill: "#DCFCE7", font: { bold: true, color: "#166534" } },
  });
  for (const [range, widthPx] of [
    ["A", 190], ["B", 520], ["C", 90], ["D", 210],
  ]) {
    sheet.getRange(range).format.columnWidthPx = widthPx;
  }
  sheet.getRange("A1:D27").format.verticalAlignment = "center";
  sheet.getRange("A6:A27").format.rowHeight = 23;
  return sheet;
}

async function savePreview(workbook, workDir, fileName, sheetName, range) {
  const preview = await workbook.render({
    sheetName,
    range,
    scale: 1.2,
    format: "png",
  });
  await fs.writeFile(
    path.join(workDir, fileName),
    new Uint8Array(await preview.arrayBuffer()),
  );
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const functionsCsv = await readCsv(args.functions);
  const resultsCsv = await readCsv(args.results);
  const summary = JSON.parse(await fs.readFile(args.summary, "utf8"));
  const { finalRows, historyRows } = selectRows(resultsCsv.rows, summary.stage);

  if (functionsCsv.rows.length !== 500) {
    throw new Error(`函数数量异常: ${functionsCsv.rows.length}`);
  }
  if (finalRows.length !== 1500) {
    throw new Error(`最终用例数量异常: ${finalRows.length}`);
  }
  if (historyRows.length !== 15) {
    throw new Error(`历史超差/异常数量异常: ${historyRows.length}`);
  }

  const workbook = Workbook.create();
  addFunctionSheet(workbook, functionsCsv.rows, functionsCsv.headers);
  addResultSheet(
    workbook,
    "1500最终结果",
    finalRows,
    resultsCsv.headers,
    "FinalResults",
  );
  addResultSheet(
    workbook,
    "超差重测记录",
    historyRows,
    resultsCsv.headers,
    "RetryHistory",
    true,
  );
  addSummarySheet(workbook, summary.stage, finalRows.length, historyRows.length);

  const summaryCheck = await workbook.inspect({
    kind: "table",
    range: "测试汇总!A1:D27",
    include: "values,formulas",
    tableMaxRows: 30,
    tableMaxCols: 4,
    maxChars: 12000,
  });
  console.log("SUMMARY_CHECK");
  console.log(summaryCheck.ndjson);

  const errorCheck = await workbook.inspect({
    kind: "match",
    searchTerm: "#REF!|#DIV/0!|#VALUE!|#NAME\\?|#N/A",
    options: { useRegex: true, maxResults: 300 },
    summary: "最终公式错误扫描",
  });
  console.log("FORMULA_ERROR_CHECK");
  console.log(errorCheck.ndjson);

  await fs.mkdir(args["work-dir"], { recursive: true });
  await Promise.all([
    savePreview(workbook, args["work-dir"], "summary.png", "测试汇总", "A1:D27"),
    savePreview(workbook, args["work-dir"], "functions.png", "500函数库", "A1:L16"),
    savePreview(workbook, args["work-dir"], "final_left.png", "1500最终结果", "A1:P14"),
    savePreview(workbook, args["work-dir"], "final_mid.png", "1500最终结果", "Q1:AG14"),
    savePreview(workbook, args["work-dir"], "final_right.png", "1500最终结果", "AH1:AX14"),
    savePreview(workbook, args["work-dir"], "history.png", "超差重测记录", "A1:AY16"),
  ]);

  await fs.mkdir(path.dirname(args.output), { recursive: true });
  const output = await SpreadsheetFile.exportXlsx(workbook);
  await output.save(args.output);
  console.log(
    `XLSX_DONE functions=${functionsCsv.rows.length} final=${finalRows.length} ` +
      `history=${historyRows.length} output=${path.resolve(args.output)}`,
  );
}

await main();
