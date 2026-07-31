import fs from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { SpreadsheetFile, Workbook } from "@oai/artifact-tool";

const currentDir = path.dirname(fileURLToPath(import.meta.url));
const rawCsvPath = path.join(currentDir, "frequency_sweep_raw.csv");
const analysisPath = path.join(currentDir, "frequency_sweep_analysis.json");
const outputPath = path.join(
  currentDir,
  "STM32G474_10k-500k_500Hz扫频补偿报告.xlsx",
);

const rawCsv = await fs.readFile(rawCsvPath, "utf8");
const analysis = JSON.parse(await fs.readFile(analysisPath, "utf8"));
const workbook = await Workbook.fromCSV(rawCsv, { sheetName: "原始数据" });
const rawSheet = workbook.worksheets.getItem("原始数据");
const infoSheet = workbook.worksheets.add("说明");
const summarySheet = workbook.worksheets.add("频点汇总");
const compensationSheet = workbook.worksheets.add("补偿结果");
const chartSheet = workbook.worksheets.add("误差曲线");

const colors = {
  navy: "#17365D",
  blue: "#1F4E78",
  teal: "#0F6B78",
  lightBlue: "#D9EAF7",
  lighterBlue: "#EEF5FB",
  green: "#E2F0D9",
  greenText: "#375623",
  amber: "#FFF2CC",
  red: "#FCE4D6",
  redText: "#9C0006",
  gray: "#666666",
  lightGray: "#E7E6E6",
  white: "#FFFFFF",
};

function styleTitle(sheet, rangeAddress, text) {
  const range = sheet.getRange(rangeAddress);
  range.merge();
  range.values = [[text]];
  range.format = {
    fill: colors.navy,
    font: { bold: true, color: colors.white, size: 16 },
    horizontalAlignment: "center",
    verticalAlignment: "center",
  };
  range.format.rowHeight = 30;
}

function styleHeader(range) {
  range.format = {
    fill: colors.blue,
    font: { bold: true, color: colors.white },
    horizontalAlignment: "center",
    verticalAlignment: "center",
    wrapText: true,
    borders: { preset: "outside", style: "thin", color: colors.navy },
  };
}

function setWidths(sheet, widths) {
  for (const [column, width] of Object.entries(widths)) {
    sheet.getRange(`${column}:${column}`).format.columnWidth = width;
  }
}

// 说明
infoSheet.showGridLines = false;
styleTitle(infoSheet, "A1:H1", "STM32G474 10 kHz–500 kHz 全链路扫频与软件补偿报告");
infoSheet.getRange("A3:B15").values = [
  ["项目", "内容"],
  ["测试日期", "2026-07-31"],
  ["信号源", "UNI-T UTG900E，USB VISA直连"],
  ["CH1设置", "单音正弦，200 mVpp，Offset 0 V，Output ON"],
  ["标称峰值 (mV)", 100.0],
  ["标称Vpp (mV)", 200.0],
  ["标称RMS (mV)", 100.0 / Math.sqrt(2.0)],
  ["CH2设置", "DC 0 V，Output ON（全程保持开启）"],
  ["扫频网格", "10 kHz–500 kHz，步进500 Hz，共981个频点"],
  ["采样次数", "每频点3次；补偿前2943条，补偿后2943条"],
  ["题目验收线", "|Upp误差|≤5 mV，|Urms误差|≤5 mV，|频率误差|≤1 kHz"],
  ["数据链路", "信号源→加法器1(+2.5 V偏置)→加法器2(CH2=0 V)→四阶有源低通→ADC"],
  ["signal_valid说明", "当前判据要求至少2个频谱分量；本次为单音扫频，因此该字段为0不代表采样无效。"],
];
styleHeader(infoSheet.getRange("A3:B3"));
infoSheet.getRange("A4:A15").format = {
  fill: colors.lighterBlue,
  font: { bold: true, color: colors.navy },
  verticalAlignment: "center",
};
infoSheet.getRange("B4:B15").format = {
  verticalAlignment: "center",
  wrapText: true,
};
infoSheet.getRange("B7:B9").format.numberFormat = "0.000";
infoSheet.getRange("A16:H16").merge();
infoSheet.getRange("A16:H16").values = [[
  "方法说明：补偿节点由补偿前981点的三次均值计算所需增益，经9点中位数平滑后，迭代加入分段线性插值误差最大的频点。固件与本工作簿使用相同的频率线性插值规则。",
]];
infoSheet.getRange("A16:H16").format = {
  fill: colors.amber,
  font: { color: "#7F6000" },
  wrapText: true,
  verticalAlignment: "center",
};
infoSheet.getRange("A16:H16").format.rowHeight = 42;
setWidths(infoSheet, { A: 20, B: 76, C: 3, D: 3, E: 3, F: 3, G: 3, H: 3 });
infoSheet.freezePanes.freezeRows(3);

// 原始数据
rawSheet.showGridLines = false;
const rawRowCount = 5887;
const rawHeader = rawSheet.getRange("A1:Y1");
styleHeader(rawHeader);
rawHeader.format.rowHeight = 36;
rawSheet.freezePanes.freezeRows(1);
rawSheet.freezePanes.freezeColumns(3);
rawSheet.getRange(`B2:B${rawRowCount}`).format.numberFormat = "yyyy-mm-dd hh:mm:ss.000";
rawSheet.getRange(`C2:C${rawRowCount}`).format.numberFormat = "#,##0";
rawSheet.getRange(`E2:G${rawRowCount}`).format.numberFormat = "0.000";
rawSheet.getRange(`I2:K${rawRowCount}`).format.numberFormat = "#,##0";
rawSheet.getRange(`L2:T${rawRowCount}`).format.numberFormat = "0.000";
rawSheet.getRange(`U2:Y${rawRowCount}`).format.numberFormat = "#,##0";
setWidths(rawSheet, {
  A: 11, B: 23, C: 18, D: 9, E: 16, F: 15, G: 15, H: 12, I: 17,
  J: 17, K: 21, L: 15, M: 17, N: 20, O: 17, P: 19, Q: 16, R: 18,
  S: 16, T: 18, U: 12, V: 12, W: 11, X: 11, Y: 13,
});
const rawTable = rawSheet.tables.add(`A1:Y${rawRowCount}`, true, "RawSweepTable");
rawTable.style = "TableStyleMedium2";
rawTable.showBandedRows = true;

// 频点汇总：所有派生值均使用公式引用原始数据。
summarySheet.showGridLines = false;
const summaryHeaders = [
  "频率 (Hz)", "标称峰值 (mV)", "补偿前原始峰值均值 (mV)", "补偿前原始峰值标准差 (mV)",
  "补偿前测量峰值均值 (mV)", "补偿前峰值误差 (mV)", "补偿后原始峰值均值 (mV)",
  "补偿后原始峰值标准差 (mV)", "补偿后测量峰值均值 (mV)", "补偿后峰值误差 (mV)",
  "补偿前Vpp均值 (mV)", "补偿前Vpp误差 (mV)", "补偿后Vpp均值 (mV)",
  "补偿后Vpp误差 (mV)", "补偿前RMS均值 (mV)", "补偿前RMS误差 (mV)",
  "补偿后RMS均值 (mV)", "补偿后RMS误差 (mV)", "补偿前频率误差均值 (Hz)",
  "补偿后频率误差均值 (Hz)", "所需补偿增益", "实测应用增益", "补偿前|峰值误差|",
  "补偿后|峰值误差|", "补偿前|Vpp误差|", "补偿后|Vpp误差|", "补偿前|RMS误差|",
  "补偿后|RMS误差|", "补偿前|频率误差|", "补偿后|频率误差|",
];
summarySheet.getRange("A1:AD1").values = [summaryHeaders];
styleHeader(summarySheet.getRange("A1:AD1"));
summarySheet.getRange("A1:AD1").format.rowHeight = 48;
const frequencies = Array.from({ length: 981 }, (_, index) => [10000 + index * 500]);
summarySheet.getRange("A2:A982").values = frequencies;
const summaryFormulas = [];
for (let index = 0; index < 981; index += 1) {
  const row = index + 2;
  const beforeStart = 2 + index * 3;
  const beforeEnd = beforeStart + 2;
  const afterStart = 2945 + index * 3;
  const afterEnd = afterStart + 2;
  summaryFormulas.push([
    "='说明'!$B$7",
    `=AVERAGE('原始数据'!L${beforeStart}:L${beforeEnd})`,
    `=STDEV.S('原始数据'!L${beforeStart}:L${beforeEnd})`,
    `=AVERAGE('原始数据'!O${beforeStart}:O${beforeEnd})`,
    `=E${row}-B${row}`,
    `=AVERAGE('原始数据'!L${afterStart}:L${afterEnd})`,
    `=STDEV.S('原始数据'!L${afterStart}:L${afterEnd})`,
    `=AVERAGE('原始数据'!O${afterStart}:O${afterEnd})`,
    `=I${row}-B${row}`,
    `=AVERAGE('原始数据'!Q${beforeStart}:Q${beforeEnd})`,
    `=K${row}-'说明'!$B$8`,
    `=AVERAGE('原始数据'!Q${afterStart}:Q${afterEnd})`,
    `=M${row}-'说明'!$B$8`,
    `=AVERAGE('原始数据'!S${beforeStart}:S${beforeEnd})`,
    `=O${row}-'说明'!$B$9`,
    `=AVERAGE('原始数据'!S${afterStart}:S${afterEnd})`,
    `=Q${row}-'说明'!$B$9`,
    `=AVERAGE('原始数据'!J${beforeStart}:J${beforeEnd})`,
    `=AVERAGE('原始数据'!J${afterStart}:J${afterEnd})`,
    `=B${row}/C${row}`,
    `=I${row}/G${row}`,
    `=ABS(F${row})`,
    `=ABS(J${row})`,
    `=ABS(L${row})`,
    `=ABS(N${row})`,
    `=ABS(P${row})`,
    `=ABS(R${row})`,
    `=ABS(S${row})`,
    `=ABS(T${row})`,
  ]);
}
summarySheet.getRange("B2:AD982").formulas = summaryFormulas;
summarySheet.freezePanes.freezeRows(1);
summarySheet.freezePanes.freezeColumns(2);
summarySheet.getRange("A2:A982").format.numberFormat = "#,##0";
summarySheet.getRange("B2:T982").format.numberFormat = "0.000";
summarySheet.getRange("U2:V982").format.numberFormat = "0.000000";
summarySheet.getRange("W2:AD982").format.numberFormat = "0.000";
setWidths(summarySheet, {
  A: 14, B: 16, C: 20, D: 20, E: 20, F: 18, G: 20, H: 20, I: 20, J: 18,
  K: 18, L: 18, M: 18, N: 18, O: 18, P: 18, Q: 18, R: 18, S: 20, T: 20,
  U: 16, V: 16, W: 18, X: 18, Y: 18, Z: 18, AA: 18, AB: 18, AC: 18, AD: 18,
});
const summaryTable = summarySheet.tables.add("A1:AD982", true, "FrequencySummaryTable");
summaryTable.style = "TableStyleMedium2";
summaryTable.showBandedRows = true;
summarySheet.getRange("X2:X982").conditionalFormats.add("cellIs", {
  operator: "greaterThan",
  formula: 5,
  format: { fill: colors.red, font: { color: colors.redText, bold: true } },
});
summarySheet.getRange("Z2:Z982").conditionalFormats.add("cellIs", {
  operator: "greaterThan",
  formula: 5,
  format: { fill: colors.red, font: { color: colors.redText, bold: true } },
});
summarySheet.getRange("AB2:AB982").conditionalFormats.add("cellIs", {
  operator: "greaterThan",
  formula: 5,
  format: { fill: colors.red, font: { color: colors.redText, bold: true } },
});
summarySheet.getRange("AD2:AD982").conditionalFormats.add("cellIs", {
  operator: "greaterThan",
  formula: 1000,
  format: { fill: colors.red, font: { color: colors.redText, bold: true } },
});

// 补偿结果
compensationSheet.showGridLines = false;
styleTitle(compensationSheet, "A1:H1", "补偿前后误差指标与9点校正表");
compensationSheet.getRange("A3:E3").values = [["指标", "补偿前", "补偿后", "改善率", "验收"]];
styleHeader(compensationSheet.getRange("A3:E3"));
compensationSheet.getRange("A4:A8").values = [
  ["最大|峰值误差| (mV)"],
  ["最大|Vpp误差| (mV)"],
  ["最大|RMS误差| (mV)"],
  ["最大|频率误差| (Hz)"],
  ["最大ADC Overrun"],
];
compensationSheet.getRange("B4:C8").formulas = [
  ["=MAX('频点汇总'!W2:W982)", "=MAX('频点汇总'!X2:X982)"],
  ["=MAX('频点汇总'!Y2:Y982)", "=MAX('频点汇总'!Z2:Z982)"],
  ["=MAX('频点汇总'!AA2:AA982)", "=MAX('频点汇总'!AB2:AB982)"],
  ["=MAX(MAX('原始数据'!J2:J2944),-MIN('原始数据'!J2:J2944))", "=MAX(MAX('原始数据'!J2945:J5887),-MIN('原始数据'!J2945:J5887))"],
  ["=MAX('原始数据'!Y2:Y2944)", "=MAX('原始数据'!Y2945:Y5887)"],
];
compensationSheet.getRange("D4:D8").formulas = [
  ["=IF(B4=0,0,(B4-C4)/B4)"],
  ["=IF(B5=0,0,(B5-C5)/B5)"],
  ["=IF(B6=0,0,(B6-C6)/B6)"],
  ["=IF(B7=0,0,(B7-C7)/B7)"],
  ["=IF(B8=0,0,(B8-C8)/B8)"],
];
compensationSheet.getRange("E4:E8").formulas = [
  ["=IF(C4<=5,\"通过\",\"不通过\")"],
  ["=IF(C5<=5,\"通过\",\"不通过\")"],
  ["=IF(C6<=5,\"通过\",\"不通过\")"],
  ["=IF(C7<=1000,\"通过\",\"不通过\")"],
  ["=IF(C8=0,\"通过\",\"检查\")"],
];
compensationSheet.getRange("B4:C8").format.numberFormat = "0.000";
compensationSheet.getRange("D4:D8").format.numberFormat = "0.0%";
compensationSheet.getRange("E4:E8").format = {
  fill: colors.green,
  font: { bold: true, color: colors.greenText },
  horizontalAlignment: "center",
};
compensationSheet.getRange("A10:C10").values = [["节点", "频率 (Hz)", "补偿增益"]];
styleHeader(compensationSheet.getRange("A10:C10"));
const nodes = analysis.compensation.nodes;
compensationSheet.getRange(`A11:C${10 + nodes.length}`).values = nodes.map((node, index) => [
  index + 1,
  node.frequency_hz,
  node.correction_gain,
]);
compensationSheet.getRange(`B11:B${10 + nodes.length}`).format.numberFormat = "#,##0";
compensationSheet.getRange(`C11:C${10 + nodes.length}`).format.numberFormat = "0.0000000";
const nodeTable = compensationSheet.tables.add(`A10:C${10 + nodes.length}`, true, "CompensationNodeTable");
nodeTable.style = "TableStyleMedium4";
compensationSheet.getRange("E10:H10").merge();
compensationSheet.getRange("E10:H10").values = [["补偿结论"]];
styleHeader(compensationSheet.getRange("E10:H10"));
compensationSheet.getRange("E11:H17").merge();
compensationSheet.getRange("E11:H17").values = [[
  "补偿后981个频点全部通过。峰值最大误差由0.9 mV降至0.3 mV；Vpp最大误差由1.9 mV降至约0.57 mV；RMS最大误差降至约0.19 mV。频率最大误差保持6 Hz，2943条复测均无ADC溢出。",
]];
compensationSheet.getRange("E11:H17").format = {
  fill: colors.green,
  font: { color: colors.greenText, size: 11 },
  wrapText: true,
  verticalAlignment: "center",
  horizontalAlignment: "left",
  borders: { preset: "outside", style: "medium", color: "#70AD47" },
};
setWidths(compensationSheet, { A: 26, B: 16, C: 16, D: 14, E: 18, F: 18, G: 18, H: 18 });
compensationSheet.freezePanes.freezeRows(3);

// 误差曲线：四张原生折线图均直接引用公式驱动的频点汇总表。
chartSheet.showGridLines = false;
styleTitle(chartSheet, "A1:R1", "10 kHz–500 kHz 误差曲线与频响补偿增益");

function addLineChart({ positionStart, positionEnd, title, yTitle, series }) {
  const chart = chartSheet.charts.add("line", {
    chartType: "line",
    title,
    hasLegend: true,
  });
  for (const item of series) {
    const chartSeries = chart.series.add(item.name);
    chartSeries.categoryFormula = "'频点汇总'!$A$2:$A$982";
    chartSeries.formula = `'频点汇总'!$${item.column}$2:$${item.column}$982`;
    chartSeries.fill = item.color;
  }
  chart.title = title;
  chart.titleTextStyle.fontSize = 12;
  chart.hasLegend = true;
  chart.xAxis = { axisType: "textAxis", textStyle: { fontSize: 9 } };
  chart.yAxis = { numberFormatCode: "0.00", textStyle: { fontSize: 9 } };
  chart.xAxis.title.text = "频率 (Hz)";
  chart.yAxis.title.text = yTitle;
  chart.setPosition(positionStart, positionEnd);
  return chart;
}

addLineChart({
  positionStart: "A3", positionEnd: "I19", title: "峰值误差：补偿前后对比",
  yTitle: "误差 (mV)",
  series: [
    { name: "补偿前", column: "F", color: "#A5A5A5" },
    { name: "补偿后", column: "J", color: "#2F75B5" },
  ],
});
addLineChart({
  positionStart: "J3", positionEnd: "R19", title: "Vpp误差：补偿前后对比",
  yTitle: "误差 (mV)",
  series: [
    { name: "补偿前", column: "L", color: "#A5A5A5" },
    { name: "补偿后", column: "N", color: "#70AD47" },
  ],
});
addLineChart({
  positionStart: "A21", positionEnd: "I37", title: "RMS误差：补偿前后对比",
  yTitle: "误差 (mV)",
  series: [
    { name: "补偿前", column: "P", color: "#A5A5A5" },
    { name: "补偿后", column: "R", color: "#ED7D31" },
  ],
});
addLineChart({
  positionStart: "J21", positionEnd: "R37", title: "低通链路所需补偿增益",
  yTitle: "增益 (V/V)",
  series: [{ name: "补偿增益", column: "U", color: "#7030A0" }],
});

// 紧凑核查：关键公式区、公式错误以及每个工作表的视觉渲染。
const inspections = {};
inspections.summary = (await workbook.inspect({
  kind: "table",
  range: "补偿结果!A3:E8",
  include: "values,formulas",
  tableMaxRows: 10,
  tableMaxCols: 8,
})).ndjson;
inspections.nodes = (await workbook.inspect({
  kind: "table",
  range: `补偿结果!A10:C${10 + nodes.length}`,
  include: "values,formulas",
  tableMaxRows: 15,
  tableMaxCols: 5,
})).ndjson;
inspections.errors = (await workbook.inspect({
  kind: "match",
  searchTerm: "#REF!|#DIV/0!|#VALUE!|#NAME\\?|#N/A",
  options: { useRegex: true, maxResults: 300 },
  summary: "final formula error scan",
})).ndjson;
await fs.writeFile(
  path.join(currentDir, "workbook_inspection.txt"),
  `${inspections.summary}\n${inspections.nodes}\n${inspections.errors}\n`,
  "utf8",
);

const previews = [
  ["说明", "A1:H16", "preview_说明.png"],
  ["原始数据", "A1:Y22", "preview_原始数据.png"],
  ["频点汇总", "A1:N22", "preview_频点汇总_1.png"],
  ["频点汇总", "U1:AD22", "preview_频点汇总_2.png"],
  ["补偿结果", "A1:H19", "preview_补偿结果.png"],
  ["误差曲线", "A1:R38", "preview_误差曲线.png"],
];
for (const [sheetName, range, fileName] of previews) {
  const image = await workbook.render({ sheetName, range, scale: 1.2, format: "png" });
  await fs.writeFile(
    path.join(currentDir, fileName),
    new Uint8Array(await image.arrayBuffer()),
  );
}

const output = await SpreadsheetFile.exportXlsx(workbook);
await output.save(outputPath);
console.log(JSON.stringify({ outputPath, previews: previews.map((item) => item[2]), inspections }, null, 2));
