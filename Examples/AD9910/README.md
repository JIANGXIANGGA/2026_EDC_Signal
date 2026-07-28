# AD9910 参考应用

本目录保存不参与当前固件构建的 AD9910 独立示例：

- `ad9910_demo_app.*`：固定频率与数字斜坡扫频状态机。
- `ad9910_ram_waveform_app.*`：旧版 RAM 波形回放示例。

当前产品入口是 `App/signal_app.c`，通过
`App/ad9910_signal_generator_app.c` 驱动 AD9910。参考示例需要单独接入
Application 调度后才能运行，不能与当前入口同时初始化 `AD9910_Service`。
