/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dac.h"
#include "dma.h"
#include "i2c.h"
#include "spi.h"
#include "tim.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "adc121s101.h"
#include "dac_output.h"
#include "dds_app.h"
#include "lv_port_display.h"
#include "lv_port_indev.h"
#include "lvgl.h"
#include "ui.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* HAL 回调仅转发给对应模块，避免在中断中执行整块数据处理。 */
/** @brief 将 SPI DMA 前半区完成事件转发给 ADC Driver。 */
void HAL_SPI_TxRxHalfCpltCallback(SPI_HandleTypeDef *hspi)
{
  ADC121S101_TxRxHalfCpltCallback(hspi);
}

/** @brief 将 SPI DMA 全缓冲区完成事件转发给 ADC Driver。 */
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
  ADC121S101_TxRxCpltCallback(hspi);
}

/** @brief 将 SPI DMA 发送完成事件转发给 LVGL 显示端口。 */
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
  lv_port_display_tx_cplt_callback(hspi);
}

/** @brief 将 SPI 错误事件分别转发给 ADC 和显示模块。 */
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
  ADC121S101_ErrorCallback(hspi);
  lv_port_display_error_callback(hspi);
}

/** @brief 将 I2C DMA 接收完成事件转发给触摸输入端口。 */
void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
  lv_port_indev_mem_rx_cplt_callback(hi2c);
}

/** @brief 将 I2C 错误事件转发给触摸输入端口。 */
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
  lv_port_indev_error_callback(hi2c);
}

/** @brief 将触摸 GPIO 外部中断转换为异步读取请求。 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == T_INT_Pin)
  {
    lv_port_indev_notify_interrupt();
  }
}

/** @brief 将 DAC DMA 前半区完成事件转发给 DAC Driver。 */
void HAL_DAC_ConvHalfCpltCallbackCh1(DAC_HandleTypeDef *hdac)
{
  DAC_Output_HalfCpltCallback(hdac);
}

/** @brief 将 DAC DMA 全缓冲区完成事件转发给 DAC Driver。 */
void HAL_DAC_ConvCpltCallbackCh1(DAC_HandleTypeDef *hdac)
{
  DAC_Output_CpltCallback(hdac);
}

/** @brief 将 DAC DMA 错误事件转发给 DAC Driver。 */
void HAL_DAC_ErrorCallbackCh1(DAC_HandleTypeDef *hdac)
{
  DAC_Output_ErrorCallback(hdac);
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_TIM6_Init();
  MX_TIM7_Init();
  MX_DAC1_Init();
  MX_SPI2_Init();
  MX_I2C3_Init();
  MX_SPI4_Init();
  /* USER CODE BEGIN 2 */
  /* 显示面板、SPI 总线和方向参数的启动配置。 */
  const lv_port_display_config_t display_config = {
    .bus = {
      .spi = &hspi4,
      .cs_port = LCD_CS_GPIO_Port,
      .cs_pin = LCD_CS_Pin,
      .dc_port = LCD_DC_GPIO_Port,
      .dc_pin = LCD_DC_Pin,
      .reset_port = LCD_RST_GPIO_Port,
      .reset_pin = LCD_RST_Pin,
      /* 背光已直接接 3.3 V，不再占用 MCU GPIO。 */
      .backlight_port = NULL,
      .backlight_pin = 0U,
    },
    .horizontal_resolution = LCD_HOR_RES,
    .vertical_resolution = LCD_VER_RES,
    .x_gap = LCD_X_GAP,
    .y_gap = LCD_Y_GAP,
    .rotation = LV_DISPLAY_ROTATION_0,
    .flags = LV_LCD_FLAG_NONE,
    .invert_colors = false,
  };
  /* 触摸控制器、坐标映射和刷新周期的启动配置。 */
  const cst816t_config_t touch_config = {
    .i2c = &hi2c3,
    .reset_port = T_RST_GPIO_Port,
    .reset_pin = T_RST_Pin,
    .horizontal_resolution = LCD_HOR_RES,
    .vertical_resolution = LCD_VER_RES,
    .swap_xy = false,
    .mirror_x = false,
    .mirror_y = true,
    .refresh_period_ms = 20U,
  };
  /* UI 控件动作到 Application 层请求函数的回调映射。 */
  const ui_signal_callbacks_t ui_callbacks = {
    .on_sample_rate_down = dds_app_request_sample_rate_down,
    .on_sample_rate_up = dds_app_request_sample_rate_up,
    .on_dds_freq_down = dds_app_request_freq_down,
    .on_dds_freq_up = dds_app_request_freq_up,
    .on_dds_amplitude_down = dds_app_request_amplitude_down,
    .on_dds_amplitude_up = dds_app_request_amplitude_up,
    .on_dds_waveform_next = dds_app_request_next_waveform,
  };
  ui_signal_state_t ui_state; /* 主循环传递给 LVGL 的信号状态快照。 */
  lv_display_t *display;      /* 初始化完成后的 LVGL 显示对象。 */

  if (ADC121S101_Init(&hspi2, &htim7) != HAL_OK)
  {
    Error_Handler();
  }

  if (DAC_Output_Init(&hdac1) != HAL_OK)
  {
    Error_Handler();
  }

  if (DAC_Output_Start() != HAL_OK)
  {
    Error_Handler();
  }

  if (dds_app_init(&htim7, &htim6) != HAL_OK)
  {
    Error_Handler();
  }

  lv_init();
  display = lv_port_display_init(&display_config);

  if (lv_port_indev_init(&touch_config, display) == NULL)
  {
    Error_Handler();
  }
  
  ui_init(&ui_callbacks);
  dds_app_fill_ui_state(&ui_state);
  ui_update_signal_state(&ui_state);

  /* TIM7 通过更新 DMA 驱动 ADC，TIM6 仅作为 DAC 硬件 TRGO。 */
  if (ADC121S101_Start() != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_TIM_Base_Start(&htim6) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    lv_port_indev_process();
    if (dds_app_process())
    {
      dds_app_fill_ui_state(&ui_state);
      ui_update_signal_state(&ui_state);
    }

    (void)lv_timer_handler();
    HAL_Delay(1U);
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV2;
  RCC_OscInitStruct.PLL.PLLN = 28;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
