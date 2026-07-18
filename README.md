# 2026_EDC_Signal

基于 STM32G474VETx 的信号采集、处理与显示工程。项目使用 STM32 HAL、C11、CMake、VS Code 与 STM32CubeMX，集成 LVGL、ST7789 显示、CST816T 触摸、ADC121S101 外部 ADC、DAC 输出、DDS 与 CMSIS-DSP FFT 处理。

## 开发环境

- MCU: STM32G474VETx
- IDE: VS Code + STM32CubeMX
- 构建系统: CMake + Ninja
- 编译器: arm-none-eabi-gcc
- 固件库: STM32 HAL
- C 标准: C11
- UI: LVGL

> 不使用 LL 库。CubeMX 生成文件只在 `USER CODE BEGIN/END` 区域内修改。

## 目录结构

```text
.
├── App/                  # Application 层，系统应用逻辑
├── Services/             # Service 层，FFT、信号处理等服务
├── Modules/              # Driver/Module 层，ADC、DAC、DDS、显示、触摸驱动
├── Core/                 # STM32CubeMX 生成的 HAL 初始化与中断文件
├── Drivers/              # STM32 HAL、CMSIS、CMSIS-DSP 依赖
├── lvgl/                 # LVGL 核心源码
├── lv_port/              # LVGL 显示与输入移植层
├── ui/                   # LVGL 用户界面
├── cmake/                # CMake 工具链与 STM32CubeMX 构建文件
├── G474.ioc              # STM32CubeMX 工程配置
├── lv_conf.h             # LVGL 配置
└── CMakeLists.txt        # 顶层 CMake 工程
```

## 构建

确认 `arm-none-eabi-gcc` 与 `ninja` 已在环境变量中，或使用 STM32 VS Code/Cube 工具链环境。

```powershell
cmake --preset Debug
cmake --build --preset Debug
```

构建产物位于：

```text
build/Debug/G474.elf
build/Debug/G474.hex
build/Debug/G474.bin
```

`build/` 已加入 `.gitignore`，不会提交到仓库。

## 主要功能模块

- `Modules/ADC/adc121s101.*`: ADC121S101 外部 ADC 采样驱动，16 bit 传输后取低 12 bit。
- `Modules/DAC/dac_output.*`: DAC 输出驱动。
- `Modules/DDS/dds.*`: DDS 波形生成。
- `Modules/Display/st7789_bus.*`: ST7789 显示总线。
- `Modules/Touch/cst816t.*`: CST816T 触摸驱动。
- `Services/fft_service.*`: CMSIS-DSP FFT 服务。
- `Services/signal_service.*`: 信号采集与处理服务。
- `lv_port/`: LVGL 显示刷新和触摸输入移植。
- `App/dds_app.*`: 应用层控制逻辑。

## 开发约定

- 优先使用 DMA，避免 Polling。
- 中断只做事件通知，不做耗时计算。
- FFT、滤波、LVGL 更新放在主循环执行。
- 采样和显示链路优先使用 Ping-Pong Buffer。
- 新模块按 Driver/Service/Application 分层组织。
- 一个模块一个 `.c/.h`，函数职责单一，注释使用中文。

## Git 说明

仓库只提交工程源码、配置与必要依赖。以下内容不提交：

- `build/`
- `Tools/`
- `.codex/`
- `.agents/`
- LVGL 示例、测试、文档和未启用的第三方扩展库
- CMSIS 示例、测试、PythonWrapper、预编译库

