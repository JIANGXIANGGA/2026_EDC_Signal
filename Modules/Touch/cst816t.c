#include "cst816t.h"

#define CST816T_I2C_ADDRESS       (0x15U << 1)
#define CST816T_REG_GESTURE_ID    0x01U
#define CST816T_FINGER_COUNT_MASK 0x0FU
#define CST816T_POSITION_HIGH_MASK 0x0FU
#define CST816T_DEFAULT_PERIOD_MS 20U

/** @brief 按 CST816T 时序执行硬件复位。 */
static void cst816t_reset(cst816t_t *touch)
{
    HAL_GPIO_WritePin(touch->config.reset_port,
                      touch->config.reset_pin,
                      GPIO_PIN_RESET);
    HAL_Delay(10U);
    HAL_GPIO_WritePin(touch->config.reset_port,
                      touch->config.reset_pin,
                      GPIO_PIN_SET);
    HAL_Delay(50U);
}

/** @brief 对原始触摸坐标执行轴交换、限幅和镜像变换。 */
static void cst816t_transform_point(cst816t_t *touch,
                                    uint16_t raw_x,
                                    uint16_t raw_y)
{
    uint16_t x = raw_x; /* 经过轴交换和镜像后的 X 坐标工作值。 */
    uint16_t y = raw_y; /* 经过轴交换和镜像后的 Y 坐标工作值。 */
    uint16_t width = touch->config.horizontal_resolution;   /* 坐标映射区域宽度。 */
    uint16_t height = touch->config.vertical_resolution;    /* 坐标映射区域高度。 */

    if(touch->config.swap_xy) {
        uint16_t temporary = x; /* 交换 X/Y 坐标时使用的临时值。 */
        x = y;
        y = temporary;
    }

    if(x >= width) {
        x = width - 1U;
    }
    if(y >= height) {
        y = height - 1U;
    }

    if(touch->config.mirror_x) {
        x = (width - 1U) - x;
    }
    if(touch->config.mirror_y) {
        y = (height - 1U) - y;
    }

    touch->x = (int32_t)x;
    touch->y = (int32_t)y;
}

/** @brief 解析 I2C 接收帧并更新触摸坐标与按下状态。 */
static void cst816t_parse_data(cst816t_t *touch)
{
    uint16_t raw_x; /* 从 I2C 接收帧解析出的原始 X 坐标。 */
    uint16_t raw_y; /* 从 I2C 接收帧解析出的原始 Y 坐标。 */

    if((touch->rx_data[1] & CST816T_FINGER_COUNT_MASK) == 0U) {
        touch->pressed = false;
        return;
    }

    raw_x = ((uint16_t)(touch->rx_data[2] & CST816T_POSITION_HIGH_MASK) << 8) |
            touch->rx_data[3];
    raw_y = ((uint16_t)(touch->rx_data[4] & CST816T_POSITION_HIGH_MASK) << 8) |
            touch->rx_data[5];
    cst816t_transform_point(touch, raw_x, raw_y);
    touch->pressed = true;
}

/** @brief 发起一次手势及坐标寄存器的 I2C DMA 读取。 */
static void cst816t_start_read(cst816t_t *touch)
{
    HAL_StatusTypeDef status; /* 发起 I2C DMA 读取的 HAL 返回状态。 */

    touch->read_requested = false;
    touch->transfer_busy = true;
    status = HAL_I2C_Mem_Read_DMA(touch->config.i2c,
                                  CST816T_I2C_ADDRESS,
                                  CST816T_REG_GESTURE_ID,
                                  I2C_MEMADD_SIZE_8BIT,
                                  touch->rx_data,
                                  sizeof(touch->rx_data));
    if(status != HAL_OK) {
        touch->transfer_busy = false;
        touch->pressed = false;
    }
}

/** @brief 初始化 CST816T 实例、坐标配置和异步读取状态。 */
HAL_StatusTypeDef CST816T_Init(cst816t_t *touch,
                               const cst816t_config_t *config)
{
    if((touch == NULL) || (config == NULL) || (config->i2c == NULL) ||
       (config->reset_port == NULL) || (config->i2c->hdmarx == NULL) ||
       (config->horizontal_resolution == 0U) ||
       (config->vertical_resolution == 0U)) {
        return HAL_ERROR;
    }

    touch->config = *config;
    if(touch->config.refresh_period_ms == 0U) {
        touch->config.refresh_period_ms = CST816T_DEFAULT_PERIOD_MS;
    }
    touch->transfer_busy = false;
    touch->transfer_complete = false;
    touch->read_requested = true;
    touch->last_read_tick = HAL_GetTick();
    touch->x = 0;
    touch->y = 0;
    touch->pressed = false;
    touch->initialized = false;

    cst816t_reset(touch);
    touch->initialized = true;
    return HAL_OK;
}

/** @brief 在主循环中解析完成帧并按周期调度下一次读取。 */
void CST816T_Process(cst816t_t *touch)
{
    uint32_t now; /* 当前系统毫秒计数，用于周期读取调度。 */

    if((touch == NULL) || !touch->initialized) {
        return;
    }

    if(touch->transfer_complete) {
        uint32_t primask = __get_PRIMASK(); /* 进入临界区前的中断屏蔽状态。 */

        __disable_irq();
        touch->transfer_complete = false;
        __set_PRIMASK(primask);
        cst816t_parse_data(touch);
    }

    now = HAL_GetTick();
    if((now - touch->last_read_tick) >= touch->config.refresh_period_ms) {
        touch->last_read_tick = now;
        touch->read_requested = true;
    }

    if(touch->read_requested && !touch->transfer_busy) {
        cst816t_start_read(touch);
    }
}

/** @brief 将触摸外部中断转换为主循环读取请求。 */
void CST816T_NotifyInterrupt(cst816t_t *touch)
{
    if((touch != NULL) && touch->initialized) {
        touch->read_requested = true;
    }
}

/** @brief 读取最近的屏幕坐标并返回当前按下状态。 */
bool CST816T_GetPoint(const cst816t_t *touch,
                      int32_t *x,
                      int32_t *y)
{
    if((touch == NULL) || !touch->initialized) {
        return false;
    }

    if(x != NULL) {
        *x = touch->x;
    }
    if(y != NULL) {
        *y = touch->y;
    }
    return touch->pressed;
}

/** @brief 处理匹配 I2C 的 DMA 接收完成事件。 */
void CST816T_MemRxCpltCallback(cst816t_t *touch,
                               I2C_HandleTypeDef *i2c)
{
    if((touch != NULL) && touch->initialized &&
       (i2c == touch->config.i2c)) {
        touch->transfer_busy = false;
        touch->transfer_complete = true;
    }
}

/** @brief 处理匹配 I2C 的传输错误并释放忙状态。 */
void CST816T_ErrorCallback(cst816t_t *touch,
                           I2C_HandleTypeDef *i2c)
{
    if((touch != NULL) && touch->initialized &&
       (i2c == touch->config.i2c)) {
        touch->transfer_busy = false;
        touch->transfer_complete = false;
        touch->pressed = false;
    }
}
