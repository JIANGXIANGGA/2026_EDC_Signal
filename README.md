# STM32G474 信号项目渐进式重写

当前分支 `codex/rebuild-from-cubemx` 是用于重新学习和逐步实现的起点，不是完整产品分支。完整旧版保存在 `main`，基线提交为 `e0d1b90`。

## 当前边界

已经保留：

- `G474.ioc` 及 CubeMX 生成的时钟、GPIO、DMA、SPI、I2C、TIM、DAC 和 NVIC 配置。
- `Modules/DDS/dds.c/.h` 完整 DDS 软件模块。
- STM32 HAL、CMSIS、CMake、启动文件和正确区分主 SRAM/CCMRAM 的链接脚本。

尚未实现：

- ADC121S101 Driver。
- DAC Ping-Pong 输出 Driver。
- Service、FFT、显示、触摸、LVGL Port、UI 和 Application。
- `main.c` 只初始化 CubeMX 外设，不启动定时器或 DMA，因此当前不会采样、输出波形或刷新屏幕。

## CubeMX 硬件基线

| 功能 | 配置 |
| --- | --- |
| MCU | STM32G474VET6，168 MHz，HSE 24 MHz |
| ADC121S101 | SPI2，PB12~PB15，16 bit，Mode 0，硬件 NSS |
| SPI2 SCLK | 10.5 MHz，满足 ADC121S101 的 10~20 MHz 规格 |
| ADC 采样节拍 | TIM7，100 kHz，更新事件 DMA |
| DAC 输出 | DAC1 CH1，PA4，TIM6 TRGO 100 kHz |
| LCD | SPI4，PE2/PE6，PE3 DC，PE4 CS，PE5 RST |
| 触摸 | I2C3 PC8/PC9，PC6 EXTI，PC7 RST |

CubeMX 生成文件只允许在 `USER CODE BEGIN/END` 区域添加自有代码。Driver、Service 和 Application 代码放在独立模块中，不直接写进生成文件。

## 构建

```powershell
cmake --preset Debug
cmake --build --preset Debug
```

每个里程碑必须独立编译、烧录和验收，通过后单独提交。完整步骤记录在：

```text
D:\01_Projects\Obsidian\电赛信号题\第三阶段-进阶\STM32G474信号项目渐进式重写流程.md
```
