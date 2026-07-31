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
#include "adc.h"
#include "dac.h"
#include "dma.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "signal_app.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* ADC2 使用 60 MHz 内核时钟、连续转换，理论采样率为 4 MSPS。 */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* 旧 CubeMX 生成调用保留在自动区，但运行时不再初始化 ADC1/PB0。 */
#define MX_ADC1_Init() ((void)0)
#define MX_TIM7_Init() ((void)0)
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

static HAL_StatusTypeDef Signal_ADC_PLLClock_Config(void)
{
  RCC_OscInitTypeDef oscillator = {0};
  RCC_ClkInitTypeDef clock = {0};

  /* 临时切换到 HSI，才能在不影响 PLLR 系统时钟的前提下修改 PLLP。 */
  oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  oscillator.HSIState = RCC_HSI_ON;
  oscillator.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  oscillator.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&oscillator) != HAL_OK)
  {
    return HAL_ERROR;
  }

  clock.ClockType = RCC_CLOCKTYPE_SYSCLK;
  clock.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  if (HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_4) != HAL_OK)
  {
    return HAL_ERROR;
  }

  oscillator = (RCC_OscInitTypeDef){0};
  oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  oscillator.HSEState = RCC_HSE_ON;
  oscillator.PLL.PLLState = RCC_PLL_ON;
  oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  oscillator.PLL.PLLM = RCC_PLLM_DIV6;
  oscillator.PLL.PLLN = 75;
  oscillator.PLL.PLLP = RCC_PLLP_DIV5;
  oscillator.PLL.PLLQ = RCC_PLLQ_DIV2;
  oscillator.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&oscillator) != HAL_OK)
  {
    return HAL_ERROR;
  }

  clock.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                    RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clock.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clock.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clock.APB1CLKDivider = RCC_HCLK_DIV1;
  clock.APB2CLKDivider = RCC_HCLK_DIV1;
  return HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_4);
}

static HAL_StatusTypeDef Signal_ADC2_Continuous_Config(void)
{
  ADC_ChannelConfTypeDef channel_config = {0};

  if (HAL_ADC_DeInit(&hadc2) != HAL_OK)
  {
    return HAL_ERROR;
  }

  hadc2.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV1;
  hadc2.Init.Resolution = ADC_RESOLUTION_12B;
  hadc2.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc2.Init.GainCompensation = 0U;
  hadc2.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc2.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc2.Init.LowPowerAutoWait = DISABLE;
  hadc2.Init.ContinuousConvMode = ENABLE;
  hadc2.Init.NbrOfConversion = 1U;
  hadc2.Init.DiscontinuousConvMode = DISABLE;
  hadc2.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc2.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc2.Init.DMAContinuousRequests = ENABLE;
  hadc2.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  hadc2.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc2) != HAL_OK)
  {
    return HAL_ERROR;
  }

  channel_config.Channel = ADC_CHANNEL_4;
  channel_config.Rank = ADC_REGULAR_RANK_1;
  channel_config.SamplingTime = ADC_SAMPLETIME_2CYCLES_5;
  channel_config.SingleDiff = ADC_SINGLE_ENDED;
  channel_config.OffsetNumber = ADC_OFFSET_NONE;
  channel_config.Offset = 0U;
  return HAL_ADC_ConfigChannel(&hadc2, &channel_config);
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
  if (Signal_ADC_PLLClock_Config() != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_TIM6_Init();
  MX_TIM7_Init();
  MX_DAC1_Init();
  MX_ADC1_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  /*
   * 保持运行固件与 G474.ioc 一致。即使尚未重新生成 CubeMX 代码，
   * 也在启动 ADC 前将 TIM7 更新率修正到严格满足 500 Hz 栅格要求。
   */
  /* PA7/ADC2_IN4 以 ADC 自由运行模式连续采样，不使用 TIM7 触发。 */
  HAL_NVIC_DisableIRQ(DMA1_Channel1_IRQn);
  MX_ADC2_Init();
  if (Signal_ADC2_Continuous_Config() != HAL_OK)
  {
    Error_Handler();
  }

  /*
   * PA7 实机标定：输入高电平 124 mV、低电平 14 mV，即 110 mVpp；
   * 34 帧串口统计的 ADC 峰峰值中位数为 137 code。
   * 因此输入端电压换算系数为 110 mV / 137 code = 0.802920 mV/code。
   * 该系数为线性比例，不假定输入固定为 110 mVpp，可覆盖 50～250 mVpp。
   */
  const signal_measurement_calibration_t measurement_calibration = {
    /* UNI-T固定为50 Ω负载标称；基础系数按50 Ω、250 mVpp三点实测恢复。 */
    .input_mv_per_code = 0.802920f,
    .peak_to_peak_gain = 1.0f,
    .rms_gain = 1.0f,
    .spectrum_gain = 1.0f,
    /*
     * 当前完整模拟链路的实机幅频标定，增益为标准幅值/ADC实测幅值。
     * 标定数据来自 50 kHz～500 kHz、500 Hz 步进、每点 3 帧的全频段扫频。
     */
    .response_point_count = 9U,
    .response = {
      {50000U, 1.0337148f},
      {81000U, 1.0446103f},
      {137000U, 1.0826270f},
      {193000U, 1.1393780f},
      {240000U, 1.2000521f},
      {299000U, 1.2946957f},
      {332000U, 1.3597650f},
      {411500U, 1.5317982f},
      {500000U, 1.7671864f},
    },
  };
  const signal_app_config_t signal_app_config = {
    .hmi_uart = &huart1,
    .measurement_calibration = &measurement_calibration,
  };

  if (Signal_App_Init(&signal_app_config) != HAL_OK)
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
    Signal_App_Process();
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
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV6;
  RCC_OscInitStruct.PLL.PLLN = 85;
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
