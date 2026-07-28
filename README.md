# STM32G474 信号采集与 AD9910 控制固件

本项目基于 STM32G474VET6、STM32 HAL 和 C11，实现：

- ADC121S101 定时 SPI DMA 采集与 Ping-Pong Buffer。
- 4096 点 CMSIS-DSP RFFT、频率/幅值/THD/波形类型分析。
- AD9910 单音、RAM 波形和数字斜坡异步控制。
- 可选 ADC 到 DAC 的同采样率 Ping-Pong 回环。
- 面向后续 TJC 串口屏的命令入口。

## 软件架构

```text
Application
├── signal_app                    总调度、错误聚合
├── ad9910_signal_generator_app   用户参数、模式与预设状态机
├── ad9910_auto_test_app          可选上电 RAM 波形测试
└── tjc_ad9910_interface          TJC 命令适配入口
        │
Service │
├── signal_acquisition_service    采集、FFT、可选回环编排
├── waveform_analyzer_service     4096 点频谱分析
├── adc_dac_loopback_service      ADC/DAC 缓冲区交接
├── ad9910_service                AD9910 异步寄存器状态机
└── ad9910_sweep_planner          数字斜坡参数规划
        │
Driver  │
├── adc121s101                    TIM7 触发的 SPI2 RX DMA
├── dac_output                    TIM6 触发的 DAC DMA
├── ad9910                        SPI4 TX DMA 与寄存器编码
└── ad9910_board                  AD9910 板级 GPIO 映射
```

依赖方向保持为 `Application -> Service -> Driver -> HAL`。CubeMX 生成文件只在
`USER CODE BEGIN/END` 区域接入 Application，业务代码不写入生成文件。

旧 AD9910 独立演示已移动到 `Examples/AD9910/`，不参与当前固件构建。

## 主循环与中断

`Core/Src/main.c` 初始化 CubeMX 外设后调用：

```c
Signal_App_Init(&signal_app_config);

while (1) {
    Signal_App_Process();
}
```

主循环负责 AD9910 状态机、DMA 错误恢复、FFT、波形分析和应用状态更新。
DMA/EXTI 中断只更新完成标志、缓冲区索引和计数器，不执行 FFT 或波形计算。

## ADC121S101 采集

| 项目 | 当前配置 |
| --- | --- |
| SPI | SPI2，主机，Mode 0，16 bit，硬件 NSS Pulse |
| SPI SCLK | 10.625 MHz |
| 采样触发 | TIM7 Update DMA，PSC=9，ARR=15 |
| 实际采样率 | 170 MHz / 10 / 16 = 1.0625 MSPS |
| DMA 缓冲区 | 2 x 4096 点，主 SRAM |
| 有效数据 | `sample = rx & 0x0FFFU` |
| FFT 分辨率 | 1,062,500 / 4096 = 259.4 Hz |

ADC 采集没有直接使用 `HAL_SPI_TransmitReceive_DMA()`。该 HAL API 会按 SPI
时钟连续发送整块数据，无法让每个 16 bit 帧由 TIM7 均匀触发。当前实现使用：

1. TIM7 Update DMA 每个采样周期向 SPI2 DR 写入一个 16 bit Dummy Word。
2. SPI2 RX DMA 循环接收 8192 点，并在半满/全满事件切换 Ping-Pong 半区。
3. 主循环复制最新完整半区并提取低 12 bit，再执行 FFT。

这是定时采样的专用实现，不是轮询。

## AD9910 控制

AD9910 当前为单线只写接口，没有接入 SDO，因此 Driver 使用
`HAL_SPI_Transmit_DMA()`。Service 通过异步状态机依次写寄存器并在主循环产生
IO UPDATE，避免在中断中处理 GPIO 时序。

Application 支持：

- 单音模式：默认 1 MHz、100%、0 度。
- RAM 波形模式：8 个可编辑预设，支持正弦、方波、三角波、上升/下降锯齿和复合波。
- 数字斜坡：固定、单次扫频和连续往返扫频的底层 Service 能力。
- 异步命令合并：写寄存器期间到达的新配置会保留到下一服务周期，不覆盖 DMA 缓冲区。

单音和 RAM 回放默认同时编译：上电先请求单音，自动测试等待 1 秒后将 RAM
preset 5 配置为 10 kHz 复合波并启动回放。产品固件可分别关闭两种输出能力。

## 可选 DAC 回环

启用回环后，DAC1 CH1 使用 TIM6 + DMA 输出两个 4096 点半区。DAC 采样率由
采集 Service 自动设置为 TIM7 的实际更新率，禁止在没有重采样器时使用不同的
ADC/DAC 时钟。主循环先提交 DAC 数据，再执行 FFT，以降低实时回环延迟。

## 构建

需要 CMake、Ninja 和 `arm-none-eabi-gcc`。使用 STM32Cube VSCode 扩展时通常会
自动提供工具链环境；独立 PowerShell 中需先把 ARM GNU Toolchain 的 `bin`
目录加入 `PATH`。

```powershell
cmake --preset Debug
cmake --build --preset Debug

cmake --preset Release
cmake --build --preset Release
```

功能选项：

| CMake 选项 | 默认值 | 作用 |
| --- | --- | --- |
| `SIGNAL_ENABLE_ADC_DAC_LOOPBACK` | `OFF` | 编译并启用 ADC 到 DAC 回环 |
| `SIGNAL_ENABLE_AD9910_AUTO_TEST` | `ON` | 启用 AD9910 上电 RAM 波形测试 |
| `SIGNAL_ENABLE_AD9910_SINGLE_TONE` | `ON` | 启用 AD9910 单音模式 |
| `SIGNAL_ENABLE_AD9910_RAM_PLAYBACK` | `ON` | 启用 AD9910 RAM 波形回放模式 |

两个模式选项分别映射为代码宏
`AD9910_SIGGEN_ENABLE_SINGLE_TONE` 和
`AD9910_SIGGEN_ENABLE_RAM_PLAYBACK`：

- 两者开启：上电请求默认单音，允许切换 RAM 回放。
- 仅单音开启：RAM 模式和 RAM 配置 API 返回 `HAL_ERROR`，RAM 自动测试自动失效。
- 仅 RAM 开启：上电直接请求默认 RAM preset 0，单音模式和配置 API 返回 `HAL_ERROR`。
- 两者关闭：CMake 配置阶段报错，避免生成无输出能力的固件。

示例：

```powershell
cmake --preset Release `
  -DSIGNAL_ENABLE_AD9910_SINGLE_TONE=ON `
  -DSIGNAL_ENABLE_AD9910_RAM_PLAYBACK=OFF
cmake --build --preset Release
```

构建会生成 `G474.elf`、`G474.hex`、`G474.bin` 和 `G474.map`。

## 内存占用

当前 GNU Arm 15.2.1 构建结果：

| 配置 | RAM | CCMRAM | FLASH |
| --- | ---: | ---: | ---: |
| Debug，双模式开启 | 53,088 / 98,304 B | 32,768 / 32,768 B | 88,040 / 524,288 B |
| Debug，仅单音 | 53,080 / 98,304 B | 32,768 / 32,768 B | 87,156 / 524,288 B |
| Debug，仅 RAM | 53,088 / 98,304 B | 32,768 / 32,768 B | 88,040 / 524,288 B |
| Release，双模式开启 | 53,104 / 98,304 B | 32,768 / 32,768 B | 71,800 / 524,288 B |
| Debug，DAC 回环开启 | 69,512 / 98,304 B | 32,768 / 32,768 B | 90,852 / 524,288 B |

CCMRAM 专用于两个 4096 点浮点 FFT 工作缓冲区，当前占满 32 KB。DMA 缓冲区
全部位于主 SRAM，因为 STM32G474 DMA 不能访问 CCMRAM。新增 CCMRAM 对象会在
链接阶段直接报溢出，不会静默侵占主栈。

## 调试状态

可通过以下只读状态入口观察运行情况：

```c
const signal_app_status_t *Signal_App_GetStatus(void);
const ad9910_siggen_status_t *AD9910_SignalGenerator_GetStatus(void);
const signal_acquisition_status_t *Signal_Acquisition_Service_GetStatus(void);
const waveform_analyzer_result_t *Waveform_Analyzer_GetResult(void);
```

硬件验收时重点检查：

- `adc_error_count == 0`
- `adc_overrun_count == 0`
- 开启回环时 `dac_underrun_count == 0`
- 开启回环时 `dac_loopback_dropped_block_count == 0`
- `signal_generator.app_error == AD9910_SIGGEN_ERROR_NONE`

Debug 使用 `-O0`，不能代表 1.0625 MSPS 下的最终实时性能。吞吐验收应使用
Release，并结合上述 overrun/drop 计数和示波器结果判断。

## 当前边界

- TJC 文件目前只有命令分发接口，尚未实现串口帧解析。
- 本次重构已通过编译和静态检查，尚未代替板级烧录、示波器和频谱仪验收。
- `ad9910_signal_generator_app` 仍使用少量 AD9910 编码常量生成 RAM Polar Word；
  后续如需支持第二种 DDS，应把波形编码抽成独立 Service 接口。
