# STM32G474 周期信号测量分析装置

本项目面向 2026 年电赛 G 题，使用 STM32G474VET6、STM32 HAL 和 C11，测量外部
信号发生器输出的周期信号。固件只负责采集、分析和显示，不再包含板载信号源
控制代码。

当前版本为 2026-08-01 第三问实测版本，已移除固定值自检和 ADC/DAC 环回路径；保留独立
VOFA+ 遥测链路，用于实机查看题目物理量和 ADC/FFT 原始诊断数据。

## 当前功能

- 片内 ADC2：PA7/ADC2_IN4 高速通道、TIM7 TRGO、DMA Ping-Pong 连续采样。
- 8192 点实数 FFT（内部复用 4096 点 CMSIS-DSP CFFT），提供频谱、谱峰及精确频点估计接口。
- Hann 窗、相干增益补偿、三点频率插值和 Hann 加权最小二乘幅相估计。
- 10 kHz～500 kHz 谱峰搜索和任意正整数阶谐波族筛选，有效信号输出 2～3 条谱线。
- 峰峰值、真有效值、基频、各频率分量幅值及频谱图显示。
- TJC 串口屏 USART1 DMA 通信，可选择显示 1 个或 3 个完整周期。
- VOFA+ USART2 TX DMA 遥测，直接输出 Upp、U、基频及各分量频率/峰值幅值。

## 软件架构

```text
Application
└── signal_app                    初始化、主循环调度、错误聚合
        │
Service │
├── signal_acquisition_service    DMA 数据交接与分析任务编排
├── waveform_analyzer_service     8192 点实数频谱分析
├── signal_measurement_service    电压标定、真有效值与谱线测量
├── signal_reconstruction_service 带内谐波时域重构与峰峰值计算
├── usart_hmi_service             TJC 帧解析与命令编码
└── vofa_telemetry_service        题目物理量与原始诊断遥测
        │
Driver  │
├── adc_internal                  ADC2_IN4 + TIM7 TRGO + ADC DMA
├── tjc_uart_driver               USART1 Receive-to-Idle DMA
└── vofa_uart_driver              USART2/PD5 TX DMA
```

依赖方向为 `Application -> Service -> Driver -> HAL`。CubeMX 生成文件只在
`USER CODE BEGIN/END` 区域接入业务代码。

测量页协议由 `signal_hmi_app` 管理，仅包含测量值发布、时域波形和频谱绘制。

## 小于 500 Hz 的频率栅格

FFT 的频率栅格间隔为：

```text
Δf = Fs / N = 3,953,488 / 8192 ≈ 482.604 Hz
```

采样服务根据 TIM7 的时钟、预分频和自动重装值计算实际采样率，FFT 与频率插值共用
同一采样率来源，避免 CubeMX 定时器参数变化后频率轴仍沿用旧常量。分析器初始化时还会检查：

```c
sample_rate_hz <= WAVEFORM_ANALYZER_FFT_SIZE *
                  WAVEFORM_ANALYZER_MAX_BIN_RESOLUTION_HZ
```

如果后续在 CubeMX 中提高采样率或减小 FFT 点数，导致 `Δf > 500 Hz`，初始化会直接
返回错误，避免生成参数不达标的固件。Hann 三点插值用于降低非整周期采样的频率
误差；量程两端允许一个 FFT 栅格的估计偏差，避免实际信号源频偏使 10 kHz 或
500 kHz 有效分量被误删。插值频率上的 Hann 加权最小二乘负责最终幅值和相位。TIM7 的实际触发率为
`170 MHz / 43 = 3,953,488.37 Hz`，运行时取整为 `3,953,488 Hz` 传入分析器。

## 片内 ADC2 高速采集参数

| 项目 | 当前配置 |
| --- | --- |
| ADC 引脚 | PA7 / ADC2_IN4 高速通道，单端 12 bit |
| ADC 时钟 | PLL VCO 340 MHz / 5 = 68 MHz，异步 DIV1 |
| 采样时间 | 2.5 周期；12 bit 单次转换总计约 15 ADC 周期 |
| 采样触发 | TIM7 TRGO Update，PSC=0，ARR=42 |
| 实际触发率 | 170 MHz / 43 = 3.95348837 MSPS |
| FFT 采样率 | 由 TIM7 配置计算，当前为 3.953488 MSPS |
| DMA 缓冲区 | 2 x 8192 点，主 SRAM |
| DMA | DMA2 Channel 2，Halfword，Circular，Very High |
| 单块时间 | 约 2.072 ms |
| FFT 分辨率 | 约 482.604 Hz |

当前采集链路为：

1. TIM7 约每 252.94 ns 产生一次 TRGO Update，硬件触发 ADC2 规则组转换。
2. ADC2 DMA 循环接收 16384 点，在半满/全满回调中只发布 8192 点半区事件。
3. 主循环复制最新 8192 点；若复制恰好跨越 DMA 半区边界，立即改复制刚完成的新半区，
   再执行单遍时域统计、Hann 加窗实数 FFT、精确频点幅相估计和测量计算。

ADC 在首次启动前执行单端校准；初始化还会验证 `Fs <= 4 MSPS` 且 ADC 时钟至少覆盖
每次 15 周期转换。ADC/DMA 错误只在回调中设置恢复标志，由主循环停止并重新启动采集。

## 测量与标定

`waveform_analyzer_service` 输出 ADC 码值域结果，
`signal_measurement_service` 负责转换为物理量：

- 仅在 10 kHz～500 kHz 搜索谱峰。
- 从最多 6 个候选峰中筛选整数倍关系最一致的 2～3 条谱线，谐波阶次不限于 2、3。
- 匹配规则为 `n = round(fi / f0)` 且 `|fi - n*f0| <= 1 kHz`，`n*f0` 不得超过 500 kHz。
- 仅检出一条孤立谱线时仍保留诊断结果，但 `signal_valid` 为 0。
- 峰峰值由频响补偿后的 2～3 个带内分量按零初相重构一个完整周期，再取 `max - min`；
  第三问中不低于 1 MHz 的干扰不会进入 Upp。
- 真有效值由带内正弦分量的峰值幅值计算：`sqrt(sum(Ui_peak^2) / 2)`，不包含带外干扰功率。
- 谱线频率先由 Hann 三点插值得到，再以模型
  `x[n] = dc + a*cos(wn) + b*sin(wn)` 做 Hann 加权最小二乘；峰值幅值为
  `sqrt(a*a + b*b)`。
- 电压结果对连续 9 帧去掉一个最大值和最小值后求平均；信号频率族改变时自动清空历史，
  稳态聚合时间约 360 ms。
- 谱线幅值最后执行电压标定和分段线性频响校正。
- 频响补偿后峰值幅值小于 5 mV 的候选谱线按零处理，抑制高频干扰产生的微弱杂散；
  该阈值等于题目分量幅值绝对误差限。
- 主循环每 40 ms 尝试分析最新完整块，全部耗时计算均不在中断内执行。

## 第三问高频干扰抑制

第三问输入为 `u = ub + uJ`，其中 `ub` 的所有频率分量位于 10 kHz～500 kHz，
`uJ` 为 200 mVpp、频率不低于 1 MHz 的单频干扰。当前数字处理数据流为：

```text
ADC DMA 原始采样
  -> Hann 窗 8192 点 FFT
  -> 仅在 10 kHz～500 kHz 搜索候选峰
  -> 选择 2～3 个整数倍谐波分量
  -> 电压标定与分段频响补偿
  -> 带内分量计算 Upp、真有效值和各分量幅值
  -> 带内谐波重构 1/3 周期时域曲线
```

数值结果、定性频谱和时域曲线使用同一组带内有效分量。时域图不再直接绘制原始 ADC
采样，因此高频干扰不会表现为曲线毛刺。重构使用题目规定的零初相模型，不引入直流偏移；
ADC 采样起点造成的统一时间平移不影响 Upp、真有效值或频率结果。

2026-08-01 实机回归覆盖 12 组不同的 2/3 分量组合、50～250 mVpp、10 kHz 和
500 kHz 边界，以及 1.0～2.0 MHz、200 mVpp 干扰，结果 12/12 PASS。最大 Upp
误差 3.2 mV、最大真有效值误差 0.716 mV、最大分量幅值误差 0.917 mV、最大频率
误差 5 Hz、最大稳定测量时间 1.375 s，ADC DMA 溢出为 0。完整逐分量结果位于
`outputs/20260801_question3/question3_interference_results_final.json`。

在同一组 `ub=250 mVpp`、250 kHz 基波和 500 kHz 二次谐波条件下，又将
200 mVpp 干扰按 1 MHz 步进从 1 MHz 扫到 20 MHz，共 20/20 组通过。最大 Upp
误差 2.5 mV、最大真有效值误差 0.713 mV、最大分量幅值误差 0.740 mV、最大频率
误差 3 Hz、最长稳定时间 1.344 s，DMA 溢出为 0。完整记录位于
`outputs/20260801_question3_1_20mhz/`，该轮无需修改算法或重新烧录。

`main.c` 当前按 PA7 实测的 `110 mVpp / 137 code = 0.802920 mV/code` 做单点标定。
该参数只是线性换算比例，不会把输入固定为 110 mVpp，适用于题目规定的 50～250 mVpp。
实机仍必须用标准信号源标定以下参数：

```c
signal_measurement_calibration_t calibration = {
    .input_mv_per_code = /* 模拟前端输入端每码对应的 mV */,
    .peak_to_peak_gain = 1.0f,
    .rms_gain = 1.0f,
    .spectrum_gain = 1.0f,
    .response_point_count = /* 频响标定点数 */,
    .response = {
        {10000U, 1.0f}, /* correction_gain = 标准幅值 / 实测幅值 */
        /* 最多 6 个频点，按频率严格递增 */
    },
};
```

未完成 10 kHz～500 kHz 多点标定前，编译通过不代表电压误差已经达到 ±5 mV。

## TJC 串口屏协议

USART1 使用 115200 bit/s、8N1、DMA 收发。测量页 ID 为 0，页面需建立
`va0`～`va13` 隐藏数值变量：

| 变量 | 含义 | 单位 |
| --- | --- | --- |
| `page0.va0` | 信号有效标志 | 0/1 |
| `page0.va1` | 峰峰值 | mV 定点数（×10） |
| `page0.va2` | 真有效值 | mV 定点数（×10） |
| `page0.va3` | 基频 | Hz |
| `page0.va4` | 有效谱线数 | 条 |
| `page0.va5/6` | 谱线 1 频率/幅值 | Hz / mV 定点数（×10） |
| `page0.va8/7` | 谱线 2 频率/幅值 | Hz / mV 定点数（×10） |
| `page0.va9/10` | 谱线 3 频率/幅值 | Hz / mV 定点数（×10） |
| `page0.va11` | 当前时域周期数 | 1/3 |
| `page0.va12/13` | 谱线 2/3 的谐波阶次（基波固定为 1，不单独显示） | 正整数 |

电压类显示使用虚拟浮点数组件 `x0`～`x4`，其 `vvs` 均设置为 `1`。
例如 STM32 写入 `va1.val=1234` 时，`x0` 显示为 `123.4 mV`；这样显示单位为
mV，同时保留 0.1 mV 分辨率。

`tm0` 定时事件使用以下代码：

```text
covx va0.val,t0.txt,0,0
x0.val=va1.val
x1.val=va2.val
covx va3.val,t3.txt,0,0
covx va4.val,t4.txt,0,0
covx va5.val,t5.txt,0,0
x2.val=va6.val
x3.val=va7.val
covx va8.val,t7.txt,0,0
covx va9.val,t9.txt,0,0
x4.val=va10.val
covx va11.val,t11.txt,0,0
covx va12.val,t24.txt,0,0
covx va13.val,t25.txt,0,0
```

最新 `22.h` 的绘图和键控组件映射为：

| 组件 | ID | STM32 功能 |
| --- | ---: | --- |
| `w0` | 18 | 1/3 周期时域波形 |
| `w1` | 35 | 正频率电压频谱 |
| `bSnap` | 19 | 立即触发一轮绘图更新 |
| `b0` | 36 | 选择显示 1 个完整周期 |
| `b1` | 37 | 选择显示 3 个完整周期 |

`b0`、`b1` 必须在 TJC 工程中开启“弹起事件发送组件 ID”。时域波形根据带内有效分量
重构 1/3 个完整周期并自动缩放；频谱按有效分量的频率位置和电压幅值相对比例绘制。
固件自动交替刷新 `w0` 和 `w1`；按下 `bSnap` 可立即触发新一轮更新，但不是正常显示所必需。
频谱横轴从 0 Hz 起，根据当前最高有效分量自动扩展并保留右侧余量，谱线使用 5 像素宽度，
避免低频、最高频谱线被控件边框遮挡。
透明传输流程为：

```text
sendme
cle <组件ID>,0
addt <组件ID>,0,512
等待 FE FF FF FF
发送 512 字节裸数据
等待 FD FF FF FF
```

## VOFA+ 串口调试协议

USART2 TX 使用 PD5、460800 bit/s、8N1 和 DMA2 Channel 1，每 100 ms 输出两行。
第一行直接对应题目物理量：

```text
Upp_mV:253.5,U_mV:65.5,f_base_Hz:100001,component_count:2,signal_valid:1,f1_Hz:100001,U1_peak_mV:82.1,n1:1,f2_Hz:500000,U2_peak_mV:42.9,n2:5,f3_Hz:0,U3_peak_mV:0.0,n3:0
```

字段含义：

| 字段 | 含义 | 单位 |
| --- | --- | --- |
| `Upp_mV` | 输入周期信号峰峰值 Upp | mV |
| `U_mV` | 去直流后的真有效值 U | mV |
| `f_base_Hz` | 有效谐波族的基频 | Hz |
| `fi_Hz` | 第 i 个有效频率分量 | Hz |
| `Ui_peak_mV` | 第 i 个正弦分量峰值幅值 Ui | mV（非 RMS、非峰峰值） |
| `ni` | 第 i 个分量相对基频的谐波阶次 | 正整数 |

第二行保留 `adc_min/max`、原始 FFT 候选峰、码值幅值、九帧聚合离散度、分析耗时和
`adc_overrun` 等字段。固件输出的是实测值；每组 UNI-T 任意波的理论真实值由公式、
频率和输出幅度单独计算，再与第一行结果做误差比较。

## 构建

```powershell
cmake --preset Debug
cmake --build --preset Debug

cmake --preset Release
cmake --build --preset Release
```

构建生成 `G474.elf`、`G474.hex`、`G474.bin` 和 `G474.map`。当前构建占用：

| 构建 | RAM | CCMRAM | FLASH |
| --- | ---: | ---: | ---: |
| Debug | 76,440 / 98,304 B | 32,768 / 32,768 B | 118,048 / 524,288 B |
| Release | 76,440 / 98,304 B | 32,768 / 32,768 B | 83,780 / 524,288 B |

CCMRAM 专用于 8192 点实数 FFT 缓冲区（偶/奇样本打包为 4096 点复数序列，共
8192 个 `float`）；Hann 窗由递推振荡器实时生成，不占用窗口数组。单边幅值谱和
DMA 缓冲区位于主 SRAM。

## 硬件边界与验收重点

- 3.953 MSPS 对 500 kHz 每周期约采集 7.91 点，显著提高高频端采样相位覆盖。
- PA7 是单极性 ADC 输入，只允许 `0～VDDA`。不能把以 0 V 为中心的双极性正弦直接
  接入；联调时应把信号发生器 DC Offset 设置为约 1.65 V，或使用交流耦合加 1.65 V
  偏置的模拟前端。软件会自动去除该直流偏置。
- PA7 处严禁低于 VSSA 或高于 VDDA，否则会触发钳位并可能损坏 MCU。
- `fJ >= 1 MHz` 可能在采样后混叠进 0～500 kHz，数字算法不能无条件恢复。
  BNC 输入后必须设置模拟低通/抗混叠滤波器，并通过实测确定截止频率和阻带衰减。
- 输入范围只有 50～250 mVpp，应配置低噪声、足够带宽的增益与偏置前端，并保留
  50 Ω 端接方案，否则 ADC 量化噪声和信号源幅值定义会直接影响 ±5 mV 指标。
- 题目每项要求 2 秒内完成；当前约 2.072 ms 数据块、40 ms 分析调度和约 360 ms
  九帧稳健聚合仍有充足裕量。2026-07-30 Release 板测连续 59 帧的分析耗时中位数为
  `17.816 ms`、最大值为 `17.827 ms`，且 `adc_overrun_count` 始终为 0。
- `G474.ioc` 已配置 PA7/ADC2_IN4、TIM7 TRGO 与 DMA2 Channel 2。当前机器没有
  CubeMX 可执行程序，后续调整外设后应重新生成并复查 USER CODE 区域接入点。
- 当前生成文件仍保留旧 ADC1 初始化调用；ADC2 完整接入暂存在 `adc.c/.h` 的 USER CODE
  区，运行时以 ADC2 为唯一采集源。下次 CubeMX 重新生成后会由 `G474.ioc` 正式生成
  ADC2、PA7 和 DMA2 Channel 2 配置，业务 Driver 接口无需变化。
- CubeMX 正式生成 `hadc2`、`hdma_adc2`、`MX_ADC2_Init()` 和 DMA2 中断后，必须删除或条件
  关闭 `adc.c/.h` USER CODE 区中的同名临时兼容实现，否则会出现重复定义；同时移除
  `main.c` USER CODE 区用于屏蔽 `MX_ADC1_Init()` 的宏以及旧 DMA1 中断禁用语句。

## 总结

当前 PA7/ADC2_IN4 + 3.953488 MSPS + 8192 点方案的频率栅格约为 482.604 Hz；
幅值采用精确频点加权最小二乘和九帧稳健聚合。达到 ±5 mV 的最后关键仍是完成
1.65 V 偏置、模拟抗混叠前端和 10 kHz～500 kHz 全频段实机标定。
