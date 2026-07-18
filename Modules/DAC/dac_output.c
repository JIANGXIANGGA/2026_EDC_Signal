#include "dac_output.h"

#include <stddef.h>

#define DAC_OUTPUT_HALF_COUNT 2U
#define DAC_OUTPUT_SAFE_CODE  2048U

typedef enum {
    DAC_BUFFER_ACTIVE = 0,
    DAC_BUFFER_AVAILABLE,
    DAC_BUFFER_UPDATING,
    DAC_BUFFER_READY
} DAC_BufferState;

static DAC_HandleTypeDef *g_output_dac;
static uint16_t g_dac_buffer[DAC_OUTPUT_HALF_COUNT][DAC_OUTPUT_BLOCK_SIZE];
static volatile DAC_BufferState g_buffer_state[DAC_OUTPUT_HALF_COUNT];
static volatile uint32_t g_completed_block_count;
static volatile uint32_t g_underrun_count;
static volatile uint32_t g_error_count;
static volatile uint32_t g_recovery_count;
static volatile uint8_t g_running;
static volatile uint8_t g_recovery_pending;

static void dac_output_fill_safe_code(void)
{
    uint32_t half_index;
    uint32_t sample_index;

    for (half_index = 0U;
         half_index < DAC_OUTPUT_HALF_COUNT;
         half_index++) {
        for (sample_index = 0U;
             sample_index < DAC_OUTPUT_BLOCK_SIZE;
             sample_index++) {
            g_dac_buffer[half_index][sample_index] = DAC_OUTPUT_SAFE_CODE;
        }
    }
}

static void dac_output_request_recovery(uint8_t is_error)
{
    if ((g_output_dac == NULL) || (g_running == 0U)) {
        return;
    }

    /* 立即停止新的 DMA 请求，耗时恢复工作留给主循环。 */
    CLEAR_BIT(g_output_dac->Instance->CR, DAC_CR_DMAEN1);
    g_running = 0U;
    g_recovery_pending = 1U;
    if (is_error != 0U) {
        g_error_count++;
    } else {
        g_underrun_count++;
    }
}

static void dac_output_advance(uint8_t completed_half,
                               uint8_t next_active_half)
{
    if ((g_running == 0U) ||
        (completed_half >= DAC_OUTPUT_HALF_COUNT) ||
        (next_active_half >= DAC_OUTPUT_HALF_COUNT)) {
        return;
    }

    g_completed_block_count++;
    g_buffer_state[completed_half] = DAC_BUFFER_AVAILABLE;

    if (g_buffer_state[next_active_half] != DAC_BUFFER_READY) {
        dac_output_request_recovery(0U);
        return;
    }
    g_buffer_state[next_active_half] = DAC_BUFFER_ACTIVE;
}

HAL_StatusTypeDef DAC_Output_Init(DAC_HandleTypeDef *hdac)
{
    if ((hdac == NULL) || (hdac->DMA_Handle1 == NULL)) {
        return HAL_ERROR;
    }

    g_output_dac = hdac;
    g_buffer_state[0] = DAC_BUFFER_READY;
    g_buffer_state[1] = DAC_BUFFER_READY;
    g_completed_block_count = 0U;
    g_underrun_count = 0U;
    g_error_count = 0U;
    g_recovery_count = 0U;
    g_running = 0U;
    g_recovery_pending = 0U;
    dac_output_fill_safe_code();
    return HAL_OK;
}

HAL_StatusTypeDef DAC_Output_Start(void)
{
    HAL_StatusTypeDef status;

    if ((g_output_dac == NULL) || (g_running != 0U)) {
        return HAL_ERROR;
    }

    g_buffer_state[0] = DAC_BUFFER_ACTIVE;
    g_buffer_state[1] = DAC_BUFFER_READY;
    status = HAL_DAC_Start_DMA(g_output_dac,
                               DAC_CHANNEL_1,
                               (uint32_t *)&g_dac_buffer[0][0],
                               DAC_OUTPUT_HALF_COUNT * DAC_OUTPUT_BLOCK_SIZE,
                               DAC_ALIGN_12B_R);
    if (status == HAL_OK) {
        g_recovery_pending = 0U;
        g_running = 1U;
    }
    return status;
}

HAL_StatusTypeDef DAC_Output_Stop(void)
{
    HAL_StatusTypeDef status;

    if (g_output_dac == NULL) {
        return HAL_ERROR;
    }

    status = HAL_DAC_Stop_DMA(g_output_dac, DAC_CHANNEL_1);
    g_running = 0U;
    return status;
}

HAL_StatusTypeDef DAC_Output_Process(void)
{
    HAL_StatusTypeDef status;

    if (g_recovery_pending == 0U) {
        return HAL_OK;
    }

    if (g_output_dac == NULL) {
        return HAL_ERROR;
    }

    (void)HAL_DAC_Stop_DMA(g_output_dac, DAC_CHANNEL_1);
    dac_output_fill_safe_code();
    status = DAC_Output_Start();
    if (status == HAL_OK) {
        g_recovery_count++;
    } else {
        g_recovery_pending = 1U;
    }
    return status;
}

uint8_t DAC_Output_AcquireBuffer(uint16_t **buffer, uint8_t *buffer_index)
{
    uint32_t primask;
    uint8_t index;

    if ((buffer == NULL) || (buffer_index == NULL) ||
        (g_running == 0U)) {
        return 0U;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    for (index = 0U; index < DAC_OUTPUT_HALF_COUNT; index++) {
        if (g_buffer_state[index] == DAC_BUFFER_AVAILABLE) {
            g_buffer_state[index] = DAC_BUFFER_UPDATING;
            *buffer = g_dac_buffer[index];
            *buffer_index = index;
            __set_PRIMASK(primask);
            return 1U;
        }
    }

    __set_PRIMASK(primask);
    return 0U;
}

uint8_t DAC_Output_CommitBuffer(uint8_t buffer_index)
{
    uint32_t primask;
    uint8_t committed = 0U;

    if (buffer_index >= DAC_OUTPUT_HALF_COUNT) {
        return 0U;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    if ((g_running != 0U) &&
        (g_buffer_state[buffer_index] == DAC_BUFFER_UPDATING)) {
        g_buffer_state[buffer_index] = DAC_BUFFER_READY;
        committed = 1U;
    }
    __set_PRIMASK(primask);
    return committed;
}

void DAC_Output_HalfCpltCallback(DAC_HandleTypeDef *hdac)
{
    if (hdac == g_output_dac) {
        dac_output_advance(0U, 1U);
    }
}

void DAC_Output_CpltCallback(DAC_HandleTypeDef *hdac)
{
    if (hdac == g_output_dac) {
        dac_output_advance(1U, 0U);
    }
}

void DAC_Output_ErrorCallback(DAC_HandleTypeDef *hdac)
{
    if (hdac == g_output_dac) {
        dac_output_request_recovery(1U);
    }
}

void DAC_Output_GetStatus(DAC_Output_Status *status)
{
    uint32_t primask;

    if (status == NULL) {
        return;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    status->completed_block_count = g_completed_block_count;
    status->underrun_count = g_underrun_count;
    status->error_count = g_error_count;
    status->recovery_count = g_recovery_count;
    status->running = g_running;
    status->recovery_pending = g_recovery_pending;
    __set_PRIMASK(primask);
}
