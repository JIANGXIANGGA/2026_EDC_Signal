#ifndef TJC_AD9910_INTERFACE_H
#define TJC_AD9910_INTERFACE_H

#include "ad9910_signal_generator_app.h"

/*
 * TJC 串口屏后续只负责把页面控件值解析成 command。
 * 本接口先不绑定 UART，避免串口协议和 AD9910 业务状态机耦合。
 * UART 中断只发布接收事件，解析和 Dispatch 必须放在主循环。
 */
HAL_StatusTypeDef TJC_AD9910_Interface_Dispatch(
    const ad9910_siggen_command_t *command);

#endif
