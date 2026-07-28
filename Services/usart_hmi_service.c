#include "usart_hmi_service.h"

#include <stdio.h>
#include <string.h>

#define USART_HMI_TERMINATOR_BYTE 0xFFU
#define USART_HMI_TERMINATOR_SIZE 3U
#define USART_HMI_COMMAND_MAX_SIZE 96U

typedef struct {
    usart_hmi_send_bytes_fn_t send_bytes;
    void *user_context;
    uint8_t async_tx;
    volatile uint8_t tx_busy;
    uint8_t tx_buffer[USART_HMI_COMMAND_MAX_SIZE + USART_HMI_TERMINATOR_SIZE];
    uint8_t rx_ring[USART_HMI_RX_RING_SIZE];
    volatile uint16_t rx_head;
    volatile uint16_t rx_tail;
    uint8_t frame[USART_HMI_MAX_PAYLOAD_SIZE];
    uint16_t frame_length;
    uint8_t pending_ff_count;
    uint8_t frame_overflow;
    usart_hmi_event_t event_queue[USART_HMI_EVENT_QUEUE_SIZE];
    uint8_t event_head;
    uint8_t event_tail;
    usart_hmi_status_t status;
} usart_hmi_context_t;

static usart_hmi_context_t g_usart_hmi;

static uint16_t usart_hmi_ring_next(uint16_t index)
{
    return (uint16_t)((index + 1U) % USART_HMI_RX_RING_SIZE);
}

static uint8_t usart_hmi_event_next(uint8_t index)
{
    return (uint8_t)((index + 1U) % USART_HMI_EVENT_QUEUE_SIZE);
}

static uint8_t usart_hmi_frame_append(uint8_t byte)
{
    if (g_usart_hmi.frame_length >= USART_HMI_MAX_PAYLOAD_SIZE) {
        g_usart_hmi.frame_overflow = 1U;
        return 0U;
    }

    g_usart_hmi.frame[g_usart_hmi.frame_length] = byte;
    g_usart_hmi.frame_length++;
    return 1U;
}

static void usart_hmi_queue_event(const usart_hmi_event_t *event)
{
    const uint8_t next = usart_hmi_event_next(g_usart_hmi.event_head);

    if (next == g_usart_hmi.event_tail) {
        g_usart_hmi.status.event_queue_overflows++;
        return;
    }

    g_usart_hmi.event_queue[g_usart_hmi.event_head] = *event;
    g_usart_hmi.event_head = next;
}

static uint8_t usart_hmi_is_return_status(uint8_t code)
{
    return (((code <= 0x24U) && (code != 0x65U) && (code != 0x66U) &&
             (code != 0x67U) && (code != 0x68U)) ||
            (code == 0xFDU) || (code == 0xFEU)) ?
               1U :
               0U;
}

static uint16_t usart_hmi_read_be16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t usart_hmi_read_le32(const uint8_t *data)
{
    return ((uint32_t)data[0]) | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void usart_hmi_decode_frame(const uint8_t *payload, uint16_t length)
{
    usart_hmi_event_t event;
    uint16_t copy_length;

    if (length == 0U) {
        return;
    }

    memset(&event, 0, sizeof(event));
    event.raw_code = payload[0];

    switch (payload[0]) {
    case 0x65U:
        if (length >= 4U) {
            event.type = USART_HMI_EVENT_TOUCH;
            event.data.touch.page_id = payload[1];
            event.data.touch.component_id = payload[2];
            event.data.touch.state =
                (payload[3] == 0U) ? USART_HMI_TOUCH_RELEASE :
                                      USART_HMI_TOUCH_PRESS;
        }
        break;

    case 0x66U:
        if (length >= 2U) {
            event.type = USART_HMI_EVENT_CURRENT_PAGE;
            event.data.current_page.page_id = payload[1];
        }
        break;

    case 0x67U:
    case 0x68U:
        if (length >= 6U) {
            event.type = USART_HMI_EVENT_TOUCH_COORDINATE;
            event.data.coordinate.x = usart_hmi_read_be16(&payload[1]);
            event.data.coordinate.y = usart_hmi_read_be16(&payload[3]);
            event.data.coordinate.state =
                (payload[5] == 0U) ? USART_HMI_TOUCH_RELEASE :
                                      USART_HMI_TOUCH_PRESS;
            event.data.coordinate.is_sleep_event =
                (payload[0] == 0x68U) ? 1U : 0U;
        }
        break;

    case 0x70U:
        event.type = USART_HMI_EVENT_STRING;
        copy_length = (uint16_t)(length - 1U);
        if (copy_length >= USART_HMI_MAX_PAYLOAD_SIZE) {
            copy_length = USART_HMI_MAX_PAYLOAD_SIZE - 1U;
        }
        memcpy(event.data.string.text, &payload[1], copy_length);
        event.data.string.text[copy_length] = '\0';
        break;

    case 0x71U:
        if (length >= 5U) {
            event.type = USART_HMI_EVENT_NUMBER;
            event.data.number.value = usart_hmi_read_le32(&payload[1]);
        }
        break;

    case 0x86U:
        event.type = USART_HMI_EVENT_SLEEP;
        break;

    case 0x87U:
        event.type = USART_HMI_EVENT_WAKEUP;
        break;

    case 0x88U:
        event.type = USART_HMI_EVENT_STARTUP;
        break;

    case 0xFDU:
        event.type = USART_HMI_EVENT_TRANSPARENT_FINISHED;
        break;

    case 0xFEU:
        event.type = USART_HMI_EVENT_TRANSPARENT_READY;
        break;

    default:
        if ((length == 1U) && (usart_hmi_is_return_status(payload[0]) != 0U)) {
            event.type = USART_HMI_EVENT_RETURN_STATUS;
            event.data.return_status.code = payload[0];
        } else {
            event.type = USART_HMI_EVENT_UNKNOWN;
            copy_length = length;
            if (copy_length > USART_HMI_MAX_PAYLOAD_SIZE) {
                copy_length = USART_HMI_MAX_PAYLOAD_SIZE;
            }
            memcpy(event.data.unknown.payload, payload, copy_length);
            event.data.unknown.length = copy_length;
        }
        break;
    }

    if (event.type != USART_HMI_EVENT_NONE) {
        usart_hmi_queue_event(&event);
    }
}

static void usart_hmi_finish_frame(void)
{
    if (g_usart_hmi.frame_overflow != 0U) {
        g_usart_hmi.status.frame_overflows++;
    } else {
        g_usart_hmi.status.rx_frames++;
        usart_hmi_decode_frame(g_usart_hmi.frame,
                               g_usart_hmi.frame_length);
    }

    g_usart_hmi.frame_length = 0U;
    g_usart_hmi.pending_ff_count = 0U;
    g_usart_hmi.frame_overflow = 0U;
}

static void usart_hmi_parser_push_byte(uint8_t byte)
{
    if (byte == USART_HMI_TERMINATOR_BYTE) {
        g_usart_hmi.pending_ff_count++;
        if (g_usart_hmi.pending_ff_count >= USART_HMI_TERMINATOR_SIZE) {
            usart_hmi_finish_frame();
        }
        return;
    }

    while (g_usart_hmi.pending_ff_count > 0U) {
        (void)usart_hmi_frame_append(USART_HMI_TERMINATOR_BYTE);
        g_usart_hmi.pending_ff_count--;
    }

    (void)usart_hmi_frame_append(byte);
}

static uint8_t usart_hmi_rx_pop(uint8_t *byte)
{
    if (g_usart_hmi.rx_tail == g_usart_hmi.rx_head) {
        return 0U;
    }

    *byte = g_usart_hmi.rx_ring[g_usart_hmi.rx_tail];
    g_usart_hmi.rx_tail = usart_hmi_ring_next(g_usart_hmi.rx_tail);
    return 1U;
}

static HAL_StatusTypeDef usart_hmi_send_buffer(const uint8_t *data,
                                                uint16_t length)
{
    HAL_StatusTypeDef status;
    uint16_t total_length;

    if ((g_usart_hmi.send_bytes == NULL) || (data == NULL) ||
        (length == 0U) || (length > USART_HMI_COMMAND_MAX_SIZE)) {
        g_usart_hmi.status.last_tx_status = HAL_ERROR;
        g_usart_hmi.status.tx_errors++;
        return HAL_ERROR;
    }

    if ((g_usart_hmi.async_tx != 0U) && (g_usart_hmi.tx_busy != 0U)) {
        g_usart_hmi.status.last_tx_status = HAL_BUSY;
        return HAL_BUSY;
    }

    memcpy(g_usart_hmi.tx_buffer, data, length);
    total_length = length;
    for (uint8_t index = 0U; index < USART_HMI_TERMINATOR_SIZE; ++index) {
        g_usart_hmi.tx_buffer[total_length] = USART_HMI_TERMINATOR_BYTE;
        total_length++;
    }

    g_usart_hmi.tx_busy = g_usart_hmi.async_tx;
    status = g_usart_hmi.send_bytes(g_usart_hmi.tx_buffer,
                                    total_length,
                                    g_usart_hmi.user_context);
    if ((status != HAL_OK) || (g_usart_hmi.async_tx == 0U)) {
        g_usart_hmi.tx_busy = 0U;
    }

    g_usart_hmi.status.last_tx_status = status;
    if (status == HAL_OK) {
        g_usart_hmi.status.tx_commands++;
    } else {
        g_usart_hmi.status.tx_errors++;
    }

    return status;
}

HAL_StatusTypeDef Usart_HMI_Service_Init(
    const usart_hmi_service_config_t *config)
{
    if ((config == NULL) || (config->send_bytes == NULL)) {
        return HAL_ERROR;
    }

    memset(&g_usart_hmi, 0, sizeof(g_usart_hmi));
    g_usart_hmi.send_bytes = config->send_bytes;
    g_usart_hmi.user_context = config->user_context;
    g_usart_hmi.async_tx = config->async_tx;
    g_usart_hmi.status.last_tx_status = HAL_OK;
    return HAL_OK;
}

void Usart_HMI_Service_Process(void)
{
    uint8_t byte;

    while (usart_hmi_rx_pop(&byte) != 0U) {
        usart_hmi_parser_push_byte(byte);
    }
}

void Usart_HMI_Service_NotifyTxComplete(void)
{
    g_usart_hmi.tx_busy = 0U;
}

HAL_StatusTypeDef Usart_HMI_Service_PushRxBytes(const uint8_t *data,
                                                 uint16_t length)
{
    HAL_StatusTypeDef status = HAL_OK;

    if ((data == NULL) && (length != 0U)) {
        return HAL_ERROR;
    }

    for (uint16_t index = 0U; index < length; ++index) {
        const uint16_t next = usart_hmi_ring_next(g_usart_hmi.rx_head);
        if (next == g_usart_hmi.rx_tail) {
            g_usart_hmi.status.rx_ring_overflows++;
            status = HAL_BUSY;
            continue;
        }

        g_usart_hmi.rx_ring[g_usart_hmi.rx_head] = data[index];
        g_usart_hmi.rx_head = next;
        g_usart_hmi.status.rx_bytes++;
    }

    return status;
}

uint8_t Usart_HMI_Service_ReadEvent(usart_hmi_event_t *event)
{
    if ((event == NULL) || (g_usart_hmi.event_tail ==
                            g_usart_hmi.event_head)) {
        return 0U;
    }

    *event = g_usart_hmi.event_queue[g_usart_hmi.event_tail];
    g_usart_hmi.event_tail = usart_hmi_event_next(g_usart_hmi.event_tail);
    return 1U;
}

const usart_hmi_status_t *Usart_HMI_Service_GetStatus(void)
{
    return &g_usart_hmi.status;
}

HAL_StatusTypeDef Usart_HMI_Service_SendCommand(const char *command)
{
    size_t length;

    if (command == NULL) {
        return HAL_ERROR;
    }

    length = strlen(command);
    if ((length == 0U) || (length > USART_HMI_COMMAND_MAX_SIZE)) {
        return HAL_ERROR;
    }

    return usart_hmi_send_buffer((const uint8_t *)command, (uint16_t)length);
}

HAL_StatusTypeDef Usart_HMI_Service_SetNumber(const char *object_name,
                                               int32_t value)
{
    char command[USART_HMI_COMMAND_MAX_SIZE];
    int written;

    if (object_name == NULL) {
        return HAL_ERROR;
    }

    written = snprintf(command, sizeof(command), "%s.val=%ld",
                       object_name, (long)value);
    if ((written <= 0) || ((size_t)written >= sizeof(command))) {
        return HAL_ERROR;
    }

    return Usart_HMI_Service_SendCommand(command);
}

HAL_StatusTypeDef Usart_HMI_Service_SetText(const char *object_name,
                                             const char *text)
{
    char command[USART_HMI_COMMAND_MAX_SIZE];
    int written;

    if ((object_name == NULL) || (text == NULL) ||
        (strchr(text, '"') != NULL)) {
        return HAL_ERROR;
    }

    written = snprintf(command, sizeof(command), "%s.txt=\"%s\"",
                       object_name, text);
    if ((written <= 0) || ((size_t)written >= sizeof(command))) {
        return HAL_ERROR;
    }

    return Usart_HMI_Service_SendCommand(command);
}

HAL_StatusTypeDef Usart_HMI_Service_SetVisible(const char *object_name,
                                                uint8_t visible)
{
    char command[USART_HMI_COMMAND_MAX_SIZE];
    int written;

    if (object_name == NULL) {
        return HAL_ERROR;
    }

    written = snprintf(command, sizeof(command), "vis %s,%u",
                       object_name, (visible == 0U) ? 0U : 1U);
    if ((written <= 0) || ((size_t)written >= sizeof(command))) {
        return HAL_ERROR;
    }

    return Usart_HMI_Service_SendCommand(command);
}

HAL_StatusTypeDef Usart_HMI_Service_Page(const char *page_name)
{
    char command[USART_HMI_COMMAND_MAX_SIZE];
    int written;

    if (page_name == NULL) {
        return HAL_ERROR;
    }

    written = snprintf(command, sizeof(command), "page %s", page_name);
    if ((written <= 0) || ((size_t)written >= sizeof(command))) {
        return HAL_ERROR;
    }

    return Usart_HMI_Service_SendCommand(command);
}

HAL_StatusTypeDef Usart_HMI_Service_GetProperty(const char *object_name,
                                                 const char *property_name)
{
    char command[USART_HMI_COMMAND_MAX_SIZE];
    int written;

    if ((object_name == NULL) || (property_name == NULL)) {
        return HAL_ERROR;
    }

    written = snprintf(command, sizeof(command), "get %s.%s",
                       object_name, property_name);
    if ((written <= 0) || ((size_t)written >= sizeof(command))) {
        return HAL_ERROR;
    }

    return Usart_HMI_Service_SendCommand(command);
}
