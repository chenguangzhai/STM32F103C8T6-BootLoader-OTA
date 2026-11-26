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
#include "dma.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "board.h"
#include "Boot_Loader.h"
/* w25q_test.h 已删除（自检代码不再保留），原因见 main() 里的说明。
   ⚠ 2026-11-24：本行原先直接写出了「2 号用户代码段的开始标记」原文，CubeMX 用字符串
   查找定位用户代码段，会先命中这句注释，于是把 Includes 尾部到真正的结束标记之间整段
   当成用户代码重新插入 —— 生成出两个 int main()、工程编译不过。
   **注释里永远不要出现标记原文（段名 + BEGIN/END 的组合）。** */
/* 本轮新增：Boot_Loader_Init() 内部会设片内 Flash 写入起点 0x08005000。
   放在这里仅为说明依赖；函数声明在 Boot_Loader.h，无需额外 include 才能调用。 */
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
  MX_USART1_UART_Init();
  /* 2026-11-24：CAN 初始化已从 Boot 移除。Boot 只做「读标志 -> 从 W25Q 搬 1MB 到片内
     -> 校验 -> 跳转」，不关心字节来自 CAN 还是串口；CAN 收包在 APP 侧完成。
     保留这段说明是为了让后来者知道：这里没有 MX_CAN_Init 是刻意为之，不是漏了。 */
  MX_SPI1_Init();
  /* USER CODE BEGIN 2 */
  Uart_Init();
	/* W25Q64 自检代码（w25q_test.c/.h）已随发布版从仓库移除：它只是开发阶段
	   确认接线/ID 的辅助手段，功能验证早已完成；W25q 绑定由 Boot_Loader_Init()
	   显式完成，不依赖自检副作用。需要重新排查硬件时从 git 历史恢复该文件。 */

	/* 本轮新增：初始化片内 Flash 落盘链路（设写入起点 = App 区 0x08005000）。
	   必须在 Uart_Init() 之后 —— 落盘进度日志走 UART_LOG，先开串口才打得出来。
	   副作用：不改 W25Q 自检行为；串口随后收到的字节会被写进片内 Flash 而非丢弃。 */
	Boot_Loader_Init();

	/* 上电 3 秒窗口。窗口期内串口收到数据 -> 返回并留在 OTA 收 bin；
	   一直没数据且 App 有效 -> 直接跳到 0x08005000，本函数不返回。
	   自检代码已移除，本函数直接跟在 Boot_Loader_Init() 之后。 */
	(void)Boot_Loader_BootWindow();

	uint16_t len = 0;
	uint32_t sum = 0;		/* 11156 字节没事，但换成 1MB 的固件时 uint16_t 会悄悄回绕 */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    len = Boot_Loader_Getlen();
    sum += len;
    if(len > 0)
    {
		  Uart_Printf(UART_LOG, "receive: %d  sum: %lu\r\n", len, sum);
    }
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

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
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
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
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
#ifdef USE_FULL_ASSERT
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
