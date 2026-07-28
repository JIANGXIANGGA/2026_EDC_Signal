#ifndef AD9910_AUTO_TEST_APP_H
#define AD9910_AUTO_TEST_APP_H

#include "stm32g4xx_hal.h"

typedef enum {
    AD9910_AUTO_TEST_STATE_WAIT_BOOT = 0,
    AD9910_AUTO_TEST_STATE_WAIT_RAM_ACTIVE,
    AD9910_AUTO_TEST_STATE_RUNNING,
    AD9910_AUTO_TEST_STATE_ERROR
} ad9910_auto_test_state_t;

typedef struct {
    ad9910_auto_test_state_t state;
    HAL_StatusTypeDef last_hal_status;
} ad9910_auto_test_status_t;

void AD9910_AutoTest_App_Init(void);
void AD9910_AutoTest_App_Process(void);
const ad9910_auto_test_status_t *AD9910_AutoTest_App_GetStatus(void);

#endif
