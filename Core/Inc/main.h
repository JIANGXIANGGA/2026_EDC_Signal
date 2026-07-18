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
#define LCD_SCK_Pin GPIO_PIN_2
#define LCD_SCK_GPIO_Port GPIOE
#define LCD_DC_Pin GPIO_PIN_3
#define LCD_DC_GPIO_Port GPIOE
#define LCD_CS_Pin GPIO_PIN_4
#define LCD_CS_GPIO_Port GPIOE
#define LCD_RST_Pin GPIO_PIN_5
#define LCD_RST_GPIO_Port GPIOE
#define LCD_MOSI_Pin GPIO_PIN_6
#define LCD_MOSI_GPIO_Port GPIOE
#define ADC121_CS_Pin GPIO_PIN_12
#define ADC121_CS_GPIO_Port GPIOB
#define ADC121_SCK_Pin GPIO_PIN_13
#define ADC121_SCK_GPIO_Port GPIOB
#define ADC121_MISO_Pin GPIO_PIN_14
#define ADC121_MISO_GPIO_Port GPIOB
#define ADC121_MOSI_Pin GPIO_PIN_15
#define ADC121_MOSI_GPIO_Port GPIOB
#define T_INT_Pin GPIO_PIN_6
#define T_INT_GPIO_Port GPIOC
#define T_INT_EXTI_IRQn EXTI9_5_IRQn
#define T_RST_Pin GPIO_PIN_7
#define T_RST_GPIO_Port GPIOC
#define T_SCK_Pin GPIO_PIN_8
#define T_SCK_GPIO_Port GPIOC
#define T_SDA_Pin GPIO_PIN_9
#define T_SDA_GPIO_Port GPIOC

/* USER CODE BEGIN Private defines */

/* 2.0 英寸 ST7789/CST816T 模组；其他面板只需调整此处与 gap。 */
#define LCD_HOR_RES               240U
#define LCD_VER_RES               284U
#define LCD_X_GAP                 0U
#define LCD_Y_GAP                 0U

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
