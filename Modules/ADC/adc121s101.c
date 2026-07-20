#include "adc121s101.h"

#include <stddef.h>

#define ADC121S101_DMA_HALF_COUNT 2U
#define ADC121S101_INVALID_HALF   0xFFU
#define ADC121S101_COPY_ATTEMPTS  2U

static SPI_HandleTypeDef *g_adc_spi;       /* ADC121S101 使用的 SPI 外设句柄。 */
static TIM_HandleTypeDef *g_sample_timer;  /* 产生采样节拍的定时器句柄。 */
static DMA_HandleTypeDef *g_trigger_dma;   /* 将虚拟发送数据写入 SPI 的定时器 DMA 句柄。 */

/* 两个半区在内存中连续，SPI RX DMA 将其视为一个 2048 点循环缓冲区。 */
static uint16_t g_dma_buffer[ADC121S101_DMA_HALF_COUNT]
                            [ADC121S101_BLOCK_SIZE]; /* SPI RX DMA Ping-Pong 接收缓冲区。 */
static uint16_t g_tx_dummy; /* 每次采样时通过 SPI 发送的占位字。 */

/* 以下元数据仅在 DMA 回调与主循环之间共享，中断中不处理采样数据。 */
static volatile uint32_t g_buffer_sequence[ADC121S101_DMA_HALF_COUNT]; /* 各半区最近写入完成的序号。 */
static volatile uint32_t g_latest_sequence;    /* DMA 最近发布的完整块序号。 */
static volatile uint32_t g_delivered_sequence; /* 主循环最近成功消费的块序号。 */
static volatile uint32_t g_completed_block_count; /* DMA 已完成的采样块总数。 */
static volatile uint32_t g_dropped_block_count;   /* 新块覆盖未消费块的累计次数。 */
static volatile uint32_t g_copy_retry_count;      /* 复制期间 DMA 回卷的累计次数。 */
static volatile uint32_t g_error_count;           /* SPI 或 DMA 错误累计次数。 */
static volatile uint32_t g_recovery_count;        /* 主循环成功恢复采样的累计次数。 */
static volatile uint8_t g_latest_half;        /* 最近完成的 DMA 半区编号。 */
static volatile uint8_t g_active_half;        /* DMA 当前正在写入的半区编号。 */
static volatile uint8_t g_running;            /* 连续采样是否正在运行。 */
static volatile uint8_t g_recovery_pending;   /* 是否需要主循环恢复采样。 */

/** @brief 在临界区中重置 Ping-Pong 缓冲区的发布与消费序号。 */
static void adc121s101_reset_stream_tracking(void)
{
    uint32_t primask = __get_PRIMASK(); /* 进入临界区前的中断屏蔽状态。 */

    __disable_irq();
    g_buffer_sequence[0] = 0U;
    g_buffer_sequence[1] = 0U;
    g_latest_sequence = g_completed_block_count;
    g_delivered_sequence = g_completed_block_count;
    g_latest_half = ADC121S101_INVALID_HALF;
    g_active_half = 0U;
    __set_PRIMASK(primask);
}

/** @brief 在 DMA 回调中发布刚完成的半区并记录下一活动半区。 */
static void adc121s101_publish_half(uint8_t completed_half,
                                    uint8_t next_active_half)
{
    uint32_t sequence; /* 本次发布块的新序号。 */

    if ((g_running == 0U) ||
        (completed_half >= ADC121S101_DMA_HALF_COUNT) ||
        (next_active_half >= ADC121S101_DMA_HALF_COUNT)) {
        return;
    }

    if (g_latest_sequence != g_delivered_sequence) {
        g_dropped_block_count++;
    }

    sequence = g_completed_block_count + 1U;
    g_completed_block_count = sequence;
    g_active_half = next_active_half;
    g_buffer_sequence[completed_half] = sequence;
    __DMB();
    g_latest_half = completed_half;
    g_latest_sequence = sequence;
}

/** @brief 在中断上下文停止新触发并向主循环发布恢复请求。 */
static void adc121s101_notify_error(void)
{
    if ((g_sample_timer == NULL) || (g_adc_spi == NULL) ||
        (g_running == 0U)) {
        return;
    }

    /* 中断中只停止新的触发并发布错误，HAL Abort 留给主循环执行。 */
    __HAL_TIM_DISABLE_DMA(g_sample_timer, TIM_DMA_UPDATE);
    __HAL_TIM_DISABLE(g_sample_timer);
    CLEAR_BIT(g_adc_spi->Instance->CR2,
              SPI_CR2_TXDMAEN | SPI_CR2_RXDMAEN);
    g_running = 0U;
    g_recovery_pending = 1U;
    g_error_count++;
}

/** @brief 将 TIM 更新 DMA 错误转换为 ADC 采样流错误事件。 */
static void adc121s101_trigger_dma_error_callback(DMA_HandleTypeDef *hdma)
{
    if (hdma == g_trigger_dma) {
        adc121s101_notify_error();
    }
}

/** @brief 绑定 SPI、采样定时器与触发 DMA，并清零双缓冲状态。 */
HAL_StatusTypeDef ADC121S101_Init(SPI_HandleTypeDef *hspi,
                                  TIM_HandleTypeDef *sample_timer)
{
    DMA_HandleTypeDef *trigger_dma; /* TIM 更新事件对应的 DMA 句柄。 */
    uint32_t half_index;            /* 初始化 Ping-Pong 半区的索引。 */
    uint32_t sample_index;          /* 初始化半区内采样点的索引。 */

    if ((hspi == NULL) || (sample_timer == NULL) ||
        (hspi->hdmarx == NULL) ||
        (sample_timer->hdma[TIM_DMA_ID_UPDATE] == NULL)) {
        return HAL_ERROR;
    }

    trigger_dma = sample_timer->hdma[TIM_DMA_ID_UPDATE];
    /* TIM7 更新 DMA 直接写 SPI2 DR，以硬件节拍替代逐点定时器中断。 */
    hspi->hdmatx = trigger_dma;
    trigger_dma->Parent = hspi;

    g_adc_spi = hspi;
    g_sample_timer = sample_timer;
    g_trigger_dma = trigger_dma;
    g_tx_dummy = 0U;
    g_completed_block_count = 0U;
    g_dropped_block_count = 0U;
    g_copy_retry_count = 0U;
    g_error_count = 0U;
    g_recovery_count = 0U;
    g_running = 0U;
    g_recovery_pending = 0U;
    adc121s101_reset_stream_tracking();

    for (half_index = 0U;
         half_index < ADC121S101_DMA_HALF_COUNT;
         half_index++) {
        for (sample_index = 0U;
             sample_index < ADC121S101_BLOCK_SIZE;
             sample_index++) {
            g_dma_buffer[half_index][sample_index] = 0U;
        }
    }

    return HAL_OK;
}

/** @brief 启动由定时器更新 DMA 驱动的 SPI 连续采样。 */
HAL_StatusTypeDef ADC121S101_Start(void)
{
    HAL_StatusTypeDef status; /* HAL 启动操作的返回状态。 */

    if ((g_adc_spi == NULL) || (g_sample_timer == NULL) ||
        (g_trigger_dma == NULL) || (g_running != 0U)) {
        return HAL_ERROR;
    }

    adc121s101_reset_stream_tracking();

    /* 先准备 SPI RX/TX DMA，再开放 TIM7 更新请求，避免丢失首个样点。 */
    status = HAL_SPI_TransmitReceive_DMA(
        g_adc_spi,
        (uint8_t *)&g_tx_dummy,
        (uint8_t *)&g_dma_buffer[0][0],
        (uint16_t)(ADC121S101_DMA_HALF_COUNT * ADC121S101_BLOCK_SIZE));
    if (status != HAL_OK) {
        return status;
    }

    /* TX DMA 由 TIM7 请求驱动，只需要传输错误中断，不需要空的 HT/TC 中断。 */
    g_trigger_dma->XferErrorCallback =
        adc121s101_trigger_dma_error_callback;
    __HAL_DMA_DISABLE_IT(g_trigger_dma, DMA_IT_HT | DMA_IT_TC);
    __HAL_DMA_ENABLE_IT(g_trigger_dma, DMA_IT_TE);

    __HAL_TIM_ENABLE_DMA(g_sample_timer, TIM_DMA_UPDATE);
    status = HAL_TIM_Base_Start(g_sample_timer);
    if (status != HAL_OK) {
        __HAL_TIM_DISABLE_DMA(g_sample_timer, TIM_DMA_UPDATE);
        (void)HAL_SPI_Abort(g_adc_spi);
        return status;
    }

    g_recovery_pending = 0U;
    g_running = 1U;
    return HAL_OK;
}

/** @brief 停止采样定时器并中止 SPI DMA 传输。 */
HAL_StatusTypeDef ADC121S101_Stop(void)
{
    HAL_StatusTypeDef timer_status; /* 停止采样定时器的返回状态。 */
    HAL_StatusTypeDef spi_status;   /* 中止 SPI DMA 的返回状态。 */

    if ((g_adc_spi == NULL) || (g_sample_timer == NULL)) {
        return HAL_ERROR;
    }

    __HAL_TIM_DISABLE_DMA(g_sample_timer, TIM_DMA_UPDATE);
    timer_status = HAL_TIM_Base_Stop(g_sample_timer);
    spi_status = HAL_SPI_Abort(g_adc_spi);
    g_running = 0U;
    adc121s101_reset_stream_tracking();

    if ((timer_status != HAL_OK) && (timer_status != HAL_ERROR)) {
        return timer_status;
    }
    return spi_status;
}

/** @brief 在主循环中处理中断发布的采样流恢复请求。 */
HAL_StatusTypeDef ADC121S101_Process(void)
{
    HAL_StatusTypeDef status; /* 当前恢复步骤的 HAL 返回状态。 */

    if (g_recovery_pending == 0U) {
        return HAL_OK;
    }

    if (g_adc_spi == NULL) {
        return HAL_ERROR;
    }

    status = HAL_SPI_Abort(g_adc_spi);
    if (status != HAL_OK) {
        return status;
    }

    status = ADC121S101_Start();
    if (status == HAL_OK) {
        g_recovery_count++;
    } else {
        g_recovery_pending = 1U;
    }
    return status;
}

/** @brief 处理 SPI RX DMA 前半区完成事件并发布半区 0。 */
void ADC121S101_TxRxHalfCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == g_adc_spi) {
        adc121s101_publish_half(0U, 1U);
    }
}

/** @brief 处理 SPI RX DMA 全缓冲区完成事件并发布半区 1。 */
void ADC121S101_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == g_adc_spi) {
        adc121s101_publish_half(1U, 0U);
    }
}

/** @brief 处理 SPI 错误事件并通知主循环恢复采样。 */
void ADC121S101_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == g_adc_spi) {
        adc121s101_notify_error();
    }
}

/** @brief 稳定复制最新采样块并提取每个 SPI 字的低 12 位。 */
uint8_t ADC121S101_CopyLatestBlock(uint16_t *destination,
                                   uint32_t capacity)
{
    uint32_t attempt; /* 为避开 DMA 回卷而执行的复制尝试编号。 */

    if ((destination == NULL) ||
        (capacity < ADC121S101_BLOCK_SIZE) ||
        (g_running == 0U)) {
        return 0U;
    }

    for (attempt = 0U; attempt < ADC121S101_COPY_ATTEMPTS; attempt++) {
        uint32_t primask;      /* 进入临界区前的中断屏蔽状态。 */
        uint32_t sequence;     /* 本次准备复制的采样块序号。 */
        uint32_t sample_index; /* 复制块内采样点的索引。 */
        uint8_t half_index;    /* 本次准备复制的 DMA 半区编号。 */
        uint8_t stable;        /* 复制前后目标半区是否保持稳定。 */

        primask = __get_PRIMASK();
        __disable_irq();
        sequence = g_latest_sequence;
        half_index = g_latest_half;
        if ((half_index >= ADC121S101_DMA_HALF_COUNT) ||
            (sequence == g_delivered_sequence) ||
            (half_index == g_active_half)) {
            __set_PRIMASK(primask);
            return 0U;
        }
        __set_PRIMASK(primask);

        for (sample_index = 0U;
             sample_index < ADC121S101_BLOCK_SIZE;
             sample_index++) {
            destination[sample_index] =
                g_dma_buffer[half_index][sample_index] & 0x0FFFU;
        }
        __DMB();

        primask = __get_PRIMASK();
        __disable_irq();
        stable = ((g_buffer_sequence[half_index] == sequence) &&
                  (g_latest_sequence == sequence) &&
                  (g_active_half != half_index)) ? 1U : 0U;
        if (stable != 0U) {
            g_delivered_sequence = sequence;
        } else {
            g_copy_retry_count++;
        }
        __set_PRIMASK(primask);

        if (stable != 0U) {
            return 1U;
        }
    }

    return 0U;
}

/** @brief 将最新块标记为已消费，用于丢弃重配置前的旧数据。 */
void ADC121S101_DiscardPendingBlock(void)
{
    uint32_t primask = __get_PRIMASK(); /* 进入临界区前的中断屏蔽状态。 */

    __disable_irq();
    g_delivered_sequence = g_latest_sequence;
    __set_PRIMASK(primask);
}

/** @brief 在临界区中复制 ADC 连续采样的运行统计。 */
void ADC121S101_GetStatus(ADC121S101_Status *status)
{
    uint32_t primask; /* 进入临界区前的中断屏蔽状态。 */

    if (status == NULL) {
        return;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    status->completed_block_count = g_completed_block_count;
    status->dropped_block_count = g_dropped_block_count;
    status->copy_retry_count = g_copy_retry_count;
    status->error_count = g_error_count;
    status->recovery_count = g_recovery_count;
    status->running = g_running;
    status->recovery_pending = g_recovery_pending;
    __set_PRIMASK(primask);
}
