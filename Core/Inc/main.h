/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32g4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define AD9910_SCLK_Pin GPIO_PIN_2
#define AD9910_SCLK_GPIO_Port GPIOE
#define AD9910_IO_UPDATE_Pin GPIO_PIN_3
#define AD9910_IO_UPDATE_GPIO_Port GPIOE
#define AD9910_CSB_Pin GPIO_PIN_4
#define AD9910_CSB_GPIO_Port GPIOE
#define AD9910_SDIO_Pin GPIO_PIN_6
#define AD9910_SDIO_GPIO_Port GPIOE
#define WHILE_TIME_Pin GPIO_PIN_3
#define WHILE_TIME_GPIO_Port GPIOA
#define AD9910_PROFILE0_Pin GPIO_PIN_7
#define AD9910_PROFILE0_GPIO_Port GPIOE
#define AD9910_PROFILE1_Pin GPIO_PIN_8
#define AD9910_PROFILE1_GPIO_Port GPIOE
#define AD9910_PROFILE2_Pin GPIO_PIN_9
#define AD9910_PROFILE2_GPIO_Port GPIOE
#define ADC121_CS_Pin GPIO_PIN_12
#define ADC121_CS_GPIO_Port GPIOB
#define ADC121_SCK_Pin GPIO_PIN_13
#define ADC121_SCK_GPIO_Port GPIOB
#define ADC121_MISO_Pin GPIO_PIN_14
#define ADC121_MISO_GPIO_Port GPIOB
#define ADC121_MOSI_Pin GPIO_PIN_15
#define ADC121_MOSI_GPIO_Port GPIOB
#define AD9910_DRCTL_Pin GPIO_PIN_6
#define AD9910_DRCTL_GPIO_Port GPIOC
#define AD9910_DRHOLD_Pin GPIO_PIN_7
#define AD9910_DRHOLD_GPIO_Port GPIOC
#define AD9910_DROVER_Pin GPIO_PIN_8
#define AD9910_DROVER_GPIO_Port GPIOC

/* USER CODE BEGIN Private defines */
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
