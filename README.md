# STM32G474 周期信号测量分析装置

本项目面向 2026 年电赛 G 题，使用 STM32G474VET6、STM32 HAL 和 C11，测量外部
信号发生器输出的周期信号。固件不再生成测试信号，AD9910 Driver、Service、
Application 和自测代码均不参与当前构建。

## 当前功能

- 片内 ADC1：PB0/ADC1_IN15、TIM7 TRGO、DMA Ping-Pong 连续采样。
- 4096 点 CMSIS-DSP CFFT，提供 `FFT_Process()`、`FFT_Magnitude[]`、`FFT_Peak*` 接口。
- Hann 窗、相干增益补偿和 Hann 三点频率/幅值插值。
- 10 kHz～500 kHz 谱峰搜索和任意正整数阶谐波族筛选，有效信号输出 2～3 条谱线。
- 峰峰值、真有效值、基频、各频率分量幅值及频谱图显示。
- TJC 串口屏 USART1 DMA 通信，可选择显示 1 个或 3 个完整周期。
- 可选 ADC 到 DAC 的同采样率 Ping-Pong 回环，默认关闭。

## 软件架构

```text
Application
└── signal_app                    初始化、主循环调度、错误聚合
        │
Service │
├── signal_acquisition_service    DMA 数据交接与分析任务编排
├── waveform_analyzer_service     4096 点频谱分析
├── signal_measurement_service    电压标定、真有效值与谱线测量
├── usart_hmi_service             TJC 帧解析与命令编码
└── adc_dac_loopback_service      可选 ADC/DAC 回环
        │
Driver  │
├── adc_internal                  ADC1_IN15 + TIM7 TRGO + ADC DMA
├── tjc_uart_driver               USART1 Receive-to-Idle DMA
└── dac_output                    可选 TIM6 + DAC DMA
```

依赖方向为 `Application -> Service -> Driver -> HAL`。CubeMX 生成文件只在
`USER CODE BEGIN/END` 区域接入业务代码。

测量页协议由 `signal_hmi_app` 管理；当前
`TJC_SCREEN_ENABLE_GENERATOR_CONTROL=0`，信号源控制代码不会参与编译。

## 500 Hz 频率分辨率

FFT 的频率栅格间隔为：

```text
Δf = Fs / N = 2,048,000 / 4096 = 500 Hz
```

频率轴使用与 `G474_FFT_Test` 一致的标称采样率 2.048 MHz，因此频率栅格固定为
500 Hz。分析器初始化
时还会检查：

```c
sample_rate_hz <= WAVEFORM_ANALYZER_FFT_SIZE *
                  WAVEFORM_ANALYZER_MAX_BIN_RESOLUTION_HZ
```

如果后续在 CubeMX 中提高采样率或减小 FFT 点数，导致 `Δf > 500 Hz`，初始化会直接
返回错误，避免生成参数不达标的固件。Hann 三点插值用于降低非整周期采样的栅栏
误差。TIM7 的实际触发率为 `170 MHz / 83 = 2,048,192.77 Hz`，与标称值相差约
94 ppm；频率轴暂按参考工程策略使用标称值，后续可用标准源标定。边界整数 bin
的三点插值允许超出标称范围最多半个 bin，避免将真实 10 kHz 谱线误删。

## 片内 ADC1 采集参数

| 项目 | 当前配置 |
| --- | --- |
| ADC 引脚 | PB0 / ADC1_IN15，单端 12 bit |
| ADC 时钟 | 170 MHz / 4 = 42.5 MHz |
| 采样时间 | 2.5 周期；12 bit 单次转换总计约 15 ADC 周期 |
| 采样触发 | TIM7 TRGO Update，PSC=0，ARR=82 |
| 实际触发率 | 170 MHz / 83 = 2.04819277 MSPS |
| FFT 标称采样率 | 2.048 MSPS |
| DMA 缓冲区 | 2 x 4096 点，主 SRAM |
| DMA | DMA1 Channel 1，Halfword，Circular，Very High |
| 单块时间 | 约 2.000 ms |
| FFT 分辨率 | 500 Hz |

当前采集链路为：

1. TIM7 约每 488.19 ns 产生一次 TRGO Update，硬件触发 ADC1 规则组转换。
2. ADC1 DMA 循环接收 8192 点，在半满/全满回调中只发布半区事件。
3. 主循环复制最新 4096 点，再执行时域统计、Hann 加窗 CFFT 和测量计算。

ADC 在首次启动前执行单端校准；ADC/DMA 错误只在回调中设置恢复标志，由主循环停止
并重新启动采集。

## 测量与标定

`waveform_analyzer_service` 输出 ADC 码值域结果，
`signal_measurement_service` 负责转换为物理量：

- 仅在 10 kHz～500 kHz 搜索谱峰。
- 从最多 6 个候选峰中筛选整数倍关系最一致的 2～3 条谱线，谐波阶次不限于 2、3。
- 匹配规则为 `n = round(fi / f0)` 且 `|fi - n*f0| <= 1 kHz`，`n*f0` 不得超过 500 kHz。
- 仅检出一条孤立谱线时仍保留诊断结果，但 `signal_valid` 为 0。
- 峰峰值为整帧原始采样的 `max(samples) - min(samples)`。
- 真有效值为整帧去直流后的时域 RMS：`sqrt(sum((sample - mean)^2) / N)`。
- 谱线幅值执行 `2/N` 缩放、Hann 相干增益补偿、插值补偿、电压标定和频响校正。
- 主循环每 20 ms 尝试分析最新完整块，全部耗时计算均不在中断内执行。

默认标定只用于联调，按 3.3 V ADC 参考电压和 1 倍模拟前端增益换算。实机必须用
标准信号源标定以下参数：

```c
signal_measurement_calibration_t calibration = {
    .input_mv_per_code = /* 模拟前端输入端每码对应的 mV */,
    .peak_to_peak_gain = 1.0f,
    .rms_gain = 1.0f,
    .spectrum_gain = 1.0f,
    .response_point_count = /* 频响标定点数 */,
};
```

未完成 10 kHz～500 kHz 多点标定前，编译通过不代表电压误差已经达到 ±5 mV。

## TJC 串口屏协议

USART1 使用 115200 bit/s、8N1、DMA 收发。测量页 ID 为 0，页面需建立
`va0`～`va11` 隐藏数值变量：

| 变量 | 含义 | 单位 |
| --- | --- | --- |
| `page0.va0` | 信号有效标志 | 0/1 |
| `page0.va1` | 峰峰值 | 0.1 mV |
| `page0.va2` | 真有效值 | 0.1 mV |
| `page0.va3` | 基频 | Hz |
| `page0.va4` | 有效谱线数 | 条 |
| `page0.va5/6` | 谱线 1 频率/幅值 | Hz / 0.1 mV |
| `page0.va8/7` | 谱线 2 频率/幅值 | Hz / 0.1 mV |
| `page0.va9/10` | 谱线 3 频率/幅值 | Hz / 0.1 mV |
| `page0.va11` | 当前时域周期数 | 1/3 |
| `page0.va12/13` | 谱线 2/3 的谐波阶次（基波固定为 1，不单独显示） | 正整数 |

最新 `22.h` 的绘图和键控组件映射为：

| 组件 | ID | STM32 功能 |
| --- | ---: | --- |
| `w0` | 20 | 1/3 周期时域波形 |
| `w1` | 40 | 正频率电压频谱 |
| `bSnap` | 21 | 立即触发一轮绘图更新 |
| `b0` | 41 | 选择显示 1 个完整周期 |
| `b1` | 42 | 选择显示 3 个完整周期 |

`b0`、`b1` 必须在 TJC 工程中开启“弹起事件发送组件 ID”。时域波形执行
上升沿对齐、重采样和留边自动缩放；频谱按有效分量的频率位置和电压幅值相对比例绘制。
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

## 构建

```powershell
cmake --preset Debug
cmake --build --preset Debug

cmake --preset Release
cmake --build --preset Release
```

| CMake 选项 | 默认值 | 作用 |
| --- | --- | --- |
| `SIGNAL_ENABLE_ADC_DAC_LOOPBACK` | `OFF` | 编译并启用 ADC 到 DAC 回环 |

构建生成 `G474.elf`、`G474.hex`、`G474.bin` 和 `G474.map`。当前构建占用：

| 构建 | RAM | CCMRAM | FLASH |
| --- | ---: | ---: | ---: |
| Debug | 56,096 / 98,304 B | 32,768 / 32,768 B | 106,092 / 524,288 B |
| Release | 56,112 / 98,304 B | 32,768 / 32,768 B | 84,272 / 524,288 B |

CCMRAM 专用于 4096 点复数 FFT 缓冲区（8192 个 `float`）；Hann 窗、单边幅值谱和
DMA 缓冲区位于主 SRAM。

## 硬件边界与验收重点

- 2.048 MSPS 对 500 kHz 每周期约采集 4.10 点，已经脱离原 1 MSPS 的奈奎斯特端点问题。
- PB0 是单极性 ADC 输入，只允许 `0～VDDA`。不能把以 0 V 为中心的双极性正弦直接
  接入；联调时应把信号发生器 DC Offset 设置为约 1.65 V，或使用交流耦合加 1.65 V
  偏置的模拟前端。软件会自动去除该直流偏置。
- PB0 处严禁低于 VSSA 或高于 VDDA，否则会触发钳位并可能损坏 MCU。
- `fJ >= 1 MHz` 可能在采样后混叠进 0～500 kHz，数字算法不能无条件恢复。
  BNC 输入后必须设置模拟低通/抗混叠滤波器，并通过实测确定截止频率和阻带衰减。
- 输入范围只有 50～250 mVpp，应配置低噪声、足够带宽的增益与偏置前端，并保留
  50 Ω 端接方案，否则 ADC 量化噪声和信号源幅值定义会直接影响 ±5 mV 指标。
- 题目每项要求 2 秒内完成；当前约 2.000 ms 数据块和 20 ms 分析调度有充足的软件
  时间裕量，最终仍需用 Release 固件检查 `adc_overrun_count == 0`。
- `G474.ioc` 已移除 SPI2、SPI4、ADC121S101 和 AD9910 引脚配置，并加入
  PB0/ADC1_IN15、TIM7 TRGO 与 DMA1 Channel 1。当前机器没有 CubeMX 可执行程序，
  所以旧生成文件中的闲置 SPI 初始化仍保留；下次用 CubeMX 重新生成后会自动清理。
- CMake 会检测 `Core/Inc/adc.h` 和 `Core/Src/adc.c`：当前缺少 CubeMX ADC 生成文件时
  Driver 自行配置 ADC/MSP/IRQ；以后 CubeMX 生成这两个文件后会自动切换到生成句柄，
  避免重复定义 ADC MSP 和 DMA1 Channel 1 中断。

## 总结

当前 PB0/ADC1_IN15 + 标称 2.048 MSPS + 4096 点方案的频率栅格为 500 Hz。项目下一阶段的关键是完成 1.65 V
偏置、模拟抗混叠前端和全频段电压标定。
