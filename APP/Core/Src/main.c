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
#include "can.h"
#include "dma.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "board.h"
#include "Boot_Loader.h"
#include "w25q_test.h"      /* W25Q64 上电自检，结果从 UART_LOG 打印 */
#include "ota_flag.h"       /* 确认制：App 启动成功后擦更新标志 */
#include "ota_backup.h"     /* 回滚：确认前把当前固件备份到 W25Q 0x200000 */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* ==================== 回滚测试桩（编译期开关） ====================
 * 0 = 正常固件（发布用，默认）
 * 1 = 按键测试：上电按住 KEY2(PA1) 才制造 HardFault（一固件两路径，但要按对时机）
 * 2 = 无条件测试：上电必崩，不依赖按键 —— 用于实测回滚分支，避免按键时序问题
 *
 * 两者都插在确认逻辑之前：标志保持 APPLIED、备份不被覆盖，卡在 HardFault_Handler
 * 的 while(1) 不擦标志 → 手动复位后 Boot 读 APPLIED，从备份区恢复旧固件。
 * 测试完把本宏改回 0 重新编译发布。
 *
 * 注意：KEY2=PA1，gpio.c 已配成输入浮空；LED 占用 PA0，不要用 KEY1。 */
#define OTA_TEST_CRASH      0

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
  MX_CAN_Init();
  MX_SPI1_Init();
  /* USER CODE BEGIN 2 */
  Uart_Init();

	/* 本轮新增：APP 侧串口收 bin -> W25Q64(0x100000)。
	   必须在 Uart_Init() 之后（日志走 UART_LOG），且在 MX_SPI1_Init() 之后（绑定 SPI1）。
	   返回非 0 表示 W25Q64 初始化失败（多半是 ID 读不到 / 片选没生效），打日志但不停机，
	   后续 Feed 会因未初始化直接报错，便于从日志定位。 */
	{
		uint8_t st = Boot_Loader_Init();

		Uart_Printf(UART_LOG, "[app] w25q init %s\r\n", (st == 0U) ? "OK" : "FAIL");
	}

#if (OTA_TEST_CRASH != 0)
	/* 【回滚测试桩】制造 HardFault，模拟"新固件跑飞"。
	   放在确认逻辑之前：此时标志仍是 APPLIED、备份还没被覆盖，
	   App 卡死在 HardFault_Handler 不会擦标志 → 下次复位 Boot 走回滚分支。
	   非对齐 word 写（0x00000001）在 Cortex-M3 上必然 HardFault，比跳 0 地址可靠。
	   mode 1 = 按住 KEY2(PA1) 才崩（浮空输入按下拉地为低）
	   mode 2 = 无条件崩（避免按键时序问题，实测回滚用） */
	{
		uint8_t crash = 0U;

#if (OTA_TEST_CRASH == 1)
		crash = (HAL_GPIO_ReadPin(KEY2_GPIO_Port, KEY2_Pin) == GPIO_PIN_RESET) ? 1U : 0U;
#elif (OTA_TEST_CRASH == 2)
		crash = 1U;
#endif
		if (crash != 0U)
		{
			Uart_Printf(UART_LOG, "[TEST] inject HardFault (mode=%d)\r\n",
						(int)OTA_TEST_CRASH);
			volatile uint32_t *bad = (volatile uint32_t *)0x00000001U;
			*bad = 0xDEADBEEFU;		/* 非对齐访问 -> HardFault，停在这里 */
		}
	}
#endif

	/* 【确认 + 备份】App 初始化全部完成后：
	   1) 若标志是 APPLIED（说明我是刚装上的新固件、还没确认过）：
	      先把当前片内固件（就是我自己，此刻已证明能跑起来）备份到 W25Q 0x200000，
	      供下次升级失败时回滚；再擦标志表示确认。
	   2) 若标志是其它状态或空：只做原有逻辑，不重复备份。
	   放这里（不是 main 第一行）：时钟/串口/W25Q 都起来才算"真的活过来了"，
	   更早 HardFault 的固件不会备份也不会擦标志，Boot 会按 APPLIED 走回滚。

	   APP 起始地址常量 0x08005000 与 Boot 侧 flash_app.h 的 APP_ADDR 一致；
	   两个工程独立，不 include 对方头文件。 */
	{
		OtaUpdateFlag_t flag;

		if ((OtaFlag_Read(&flag) == OTA_FLAG_OK) && (flag.state == OTA_FLAG_APPLIED))
		{
			/* 备份当前固件：长度用标志里的（Boot 搬运时写的就是这份固件的长度） */
			int8_t br = OtaBackup_SaveFromFlash(0x08005000UL, flag.length);

			Uart_Printf(UART_LOG, "[app] backup current fw %s (%luB)\r\n",
						(br == OTA_BAK_OK) ? "OK" : "FAIL (no rollback next time)",
						(unsigned long)flag.length);
		}

		/* 确认：擦标志（无论上面备份成功与否，当前固件已经跑起来了） */
		if (OtaFlag_Read(&flag) == OTA_FLAG_OK)
		{
			int8_t r = OtaFlag_Erase();

			Uart_Printf(UART_LOG, "[app] update confirmed, flag %s\r\n",
						(r == OTA_FLAG_OK) ? "erased" : "erase FAIL (will reapply)");
		}
	}

	uint16_t len = 0;
	uint32_t sum = 0;
	static uint32_t s_led_t;		/* LED 闪烁基准，非阻塞 */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */
    /* USER CODE BEGIN 3 */
		/* 关键：LED 用非阻塞定时，不能用 HAL_Delay(500)。
		   HAL_Delay 会让主循环 1 秒才取一次串口数据，而 1KB 环缓冲在 115200 下
		   约 87ms 就被灌满 —— 必溢出丢字节，bin 写进 W25Q 就是残缺的。 */
		if (Timer_Expired(&s_led_t, 500U))
		{
			Led_Fan_Toggle();
		}

		/* 每圈都取串口数据喂给 W25Q 落盘模块；Getlen 内部处理满扇区擦写与 idle 校验 */
		len = Boot_Loader_Getlen();
		sum += len;
		if (len > 0)
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
