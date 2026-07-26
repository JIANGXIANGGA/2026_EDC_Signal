# STM32G474 信号项目渐进式重写

当前分支 `codex/rebuild-from-cubemx` 是用于重新学习和逐步实现的起点，不是完整产品分支。完整旧版保存在 `main`，基线提交为 `e0d1b90`。

## 当前边界

已经保留：

- `G474.ioc` 及 CubeMX 生成的时钟、GPIO、DMA、SPI、I2C、TIM、DAC 和 NVIC 配置。
- AD9910 的 SPI DMA Driver、异步 Service、最小 Application 和手动数字斜坡控制。
- STM32 HAL、CMSIS、CMake、启动文件和正确区分主 SRAM/CCMRAM 的链接脚本。

已接入当前 `main.c`：

- ADC121S101 采集和内部 DAC Ping-Pong 输出，由 `App/signal_app.c` 统一调度。

尚未接入当前 `main.c`：

- FFT、串口屏 UI 和完整信号处理 Application。
- 当前 `main.c` 已启动 AD9910 最小示例、TIM7/SPI2 ADC 采集和 TIM6/DAC1 DDS 输出数据流。

## CubeMX 硬件基线

| 功能 | 配置 |
| --- | --- |
| MCU | STM32G474VET6，170 MHz，HSE 24 MHz |
| ADC121S101 | SPI2，PB12~PB15，16 bit，Mode 0，硬件 NSS |
| SPI2 SCLK | 10.625 MHz，满足 ADC121S101 的 10~20 MHz 规格 |
| ADC 采样节拍 | TIM7，100 kHz，更新事件 DMA |
| DAC 输出 | DAC1 CH1，PA4，TIM6 TRGO 100 kHz |
| AD9910 | SPI4 10.625 MHz，PE2 SCLK，PE6 SDIO，PE3 IO_UPDATE，PE4 CSB；PE7/PE8/PE9 为 PF0/PF1/PF2，PC6/PC7/PC8 为 DRC/DPH/DRO |
| AD9910 供电 | 模块独立 DC 5 V / 1 A，与 STM32G474 共地；控制信号为 3.3 V |
| 已释放 GPIO | PC9（PC6/PC7/PC8 已用于 DRC/DPH/DRO） |

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

## AD9910 代码内信号发生器

当前 `main.c` 默认通过 `App/signal_app.c` 启动 `App/ad9910_ram_waveform_app.c`，使用 AD9910 内部 RAM 回放自定义幅度波形，不使用 STM32 DAC 输出口。RAM 回放参数和波形表在 `App/ad9910_ram_waveform_presets.c` 中修改。

如果需要恢复原来的 AD9910 单音/扫频演示，将 `App/signal_app.c` 中的 `SIGNAL_APP_USE_AD9910_RAM_PLAYBACK` 改为 `0U`。

原 `App/ad9910_demo_app.c` 无需串口屏，上电后自动加载代码内预设并运行。支持：

- 固定频率输出。
- 单次线性扫频，到达终点后转为固定终止频率。
- 连续往返扫频，自动执行上扫、终点停留、返回、起点停留。
- 频率、幅度百分比、相位、扫描时间、返回时间和端点停留时间。
- AD9910 Profile0~7 编程与 GPIO 选择。

### 修改上电预设

编辑 `App/ad9910_demo_presets.c`：

```c
#define AD9910_DEMO_BOOT_PRESET AD9910_DEMO_PRESET_USER

static const ad9910_demo_config_t g_ad9910_user_config = {
    .mode = AD9910_DEMO_MODE_CONTINUOUS_SWEEP,
    .start_frequency_hz = 100000U,
    .stop_frequency_hz = 10000000U,
    .sweep_time_ms = 1000U,
    .return_time_ms = 500U,
    .start_hold_ms = 100U,
    .stop_hold_ms = 100U,
    .target_steps = 10000U,
    .amplitude_percent = 100U,
    .phase_degrees = 0U,
};
```

可选择的预设：

```text
AD9910_DEMO_PRESET_FIXED_1KHZ
AD9910_DEMO_PRESET_FIXED_1MHZ
AD9910_DEMO_PRESET_AUDIO_SWEEP
AD9910_DEMO_PRESET_RF_SWEEP
AD9910_DEMO_PRESET_SINGLE_RF_SWEEP
AD9910_DEMO_PRESET_USER
```

扫频规划器根据时间和 `target_steps` 自动计算 `positive_step_hz`、`negative_step_hz`、`positive_rate` 和 `negative_rate`。默认 USER 预设计算结果为：

```text
990 Hz/步，10000 步
上扫 rate = 25000，实际 1000 ms
返回 rate = 12500，实际 500 ms
```

`DRO` 中断只累计端点事件；Application 主循环使用 `DPH` 实现停留并切换 `DRC` 方向。模块的 `PWR/RST/TEN/OSK` 接 GND，5 V 独立供电并与 STM32 共地。

调试器观察 `g_ad9910_demo_status`：`app_error=AD9910_DEMO_ERROR_NONE`，且 `run_state` 进入 `FIXED_READY`、`SWEEP_UP/DOWN` 或 `SINGLE_COMPLETE` 时，Application 正常运行。
