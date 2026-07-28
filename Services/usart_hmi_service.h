#ifndef USART_HMI_SERVICE_H
#define USART_HMI_SERVICE_H

#include <stdint.h>

#include "stm32g4xx_hal.h"

#ifndef USART_HMI_RX_RING_SIZE
#define USART_HMI_RX_RING_SIZE 256U
#endif

#ifndef USART_HMI_MAX_PAYLOAD_SIZE
#define USART_HMI_MAX_PAYLOAD_SIZE 96U
#endif

#ifndef USART_HMI_EVENT_QUEUE_SIZE
#define USART_HMI_EVENT_QUEUE_SIZE 8U
#endif

typedef HAL_StatusTypeDef (*usart_hmi_send_bytes_fn_t)(
    const uint8_t *data,
    uint16_t length,
    void *user_context);

typedef enum {
    USART_HMI_EVENT_NONE = 0,
    USART_HMI_EVENT_TOUCH,
    USART_HMI_EVENT_CURRENT_PAGE,
    USART_HMI_EVENT_TOUCH_COORDINATE,
    USART_HMI_EVENT_STRING,
    USART_HMI_EVENT_NUMBER,
    USART_HMI_EVENT_RETURN_STATUS,
    USART_HMI_EVENT_STARTUP,
    USART_HMI_EVENT_SLEEP,
    USART_HMI_EVENT_WAKEUP,
    USART_HMI_EVENT_TRANSPARENT_READY,
    USART_HMI_EVENT_TRANSPARENT_FINISHED,
    USART_HMI_EVENT_UNKNOWN
} usart_hmi_event_type_t;

typedef enum {
    USART_HMI_TOUCH_RELEASE = 0,
    USART_HMI_TOUCH_PRESS = 1
} usart_hmi_touch_state_t;

typedef struct {
    usart_hmi_send_bytes_fn_t send_bytes;
    void *user_context;
    uint8_t async_tx;
} usart_hmi_service_config_t;

typedef struct {
    usart_hmi_event_type_t type;
    uint8_t raw_code;
    union {
        struct {
            uint8_t page_id;
            uint8_t component_id;
            usart_hmi_touch_state_t state;
        } touch;

        struct {
            uint8_t page_id;
        } current_page;

        struct {
            uint16_t x;
            uint16_t y;
            usart_hmi_touch_state_t state;
            uint8_t is_sleep_event;
        } coordinate;

        struct {
            char text[USART_HMI_MAX_PAYLOAD_SIZE];
        } string;

        struct {
            uint32_t value;
        } number;

        struct {
            uint8_t code;
        } return_status;

        struct {
            uint8_t payload[USART_HMI_MAX_PAYLOAD_SIZE];
            uint16_t length;
        } unknown;
    } data;
} usart_hmi_event_t;

typedef struct {
    uint32_t rx_bytes;
    uint32_t rx_frames;
    uint32_t rx_ring_overflows;
    uint32_t frame_overflows;
    uint32_t event_queue_overflows;
    uint32_t tx_commands;
    uint32_t tx_errors;
    HAL_StatusTypeDef last_tx_status;
} usart_hmi_status_t;

HAL_StatusTypeDef Usart_HMI_Service_Init(
    const usart_hmi_service_config_t *config);
void Usart_HMI_Service_Process(void);
void Usart_HMI_Service_NotifyTxComplete(void);
HAL_StatusTypeDef Usart_HMI_Service_PushRxBytes(const uint8_t *data,
                                                 uint16_t length);
uint8_t Usart_HMI_Service_ReadEvent(usart_hmi_event_t *event);
const usart_hmi_status_t *Usart_HMI_Service_GetStatus(void);

HAL_StatusTypeDef Usart_HMI_Service_SendCommand(const char *command);
HAL_StatusTypeDef Usart_HMI_Service_SetNumber(const char *object_name,
                                               int32_t value);
HAL_StatusTypeDef Usart_HMI_Service_SetText(const char *object_name,
                                             const char *text);
HAL_StatusTypeDef Usart_HMI_Service_SetVisible(const char *object_name,
                                                uint8_t visible);
HAL_StatusTypeDef Usart_HMI_Service_Page(const char *page_name);
HAL_StatusTypeDef Usart_HMI_Service_GetProperty(const char *object_name,
                                                 const char *property_name);

#endif
