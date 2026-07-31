#ifndef VOFA_TELEMETRY_SERVICE_H
#define VOFA_TELEMETRY_SERVICE_H

#include <stdint.h>

#include "stm32g4xx_hal.h"

typedef struct {
    uint8_t initialized;
    HAL_StatusTypeDef last_hal_status;
    uint32_t publish_count;
    uint32_t format_error_count;
    uint32_t transmit_error_count;
    uint32_t last_measurement_count;
} vofa_telemetry_status_t;

HAL_StatusTypeDef VOFA_Telemetry_Service_Init(void);
HAL_StatusTypeDef VOFA_Telemetry_Service_Process(void);
const vofa_telemetry_status_t *VOFA_Telemetry_Service_GetStatus(void);

#endif
