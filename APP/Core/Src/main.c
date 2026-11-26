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
#include "ota_flag.h"       /* 确认制：App 启动成功后擦更新标志 */
#include "ota_backup.h"     /* 回滚：确认前把当前固件备份到 W25Q 0x200000 */
#include "can_drv.h"        /* CAN：Normal 收 0x601 应答 / 发 0x581 查询；阶段 1+2 握手与分帧接收 */
#include "ota_w25q.h"       /* 阶段 2：CAN 分片直接喂 OtaW25q_Feed（不经过串口那条路径） */
#include "crc32.h"          /* 去重：Crc32_Flash 算片内当前固件 CRC，与 REPLY 的 CRC 比对 */
#include <string.h>         /* memcpy：把 DATA 帧载荷拼进块缓冲 */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* ==================== CAN OTA 会话层：阶段 1 查询握手 ====================
 * 为什么复用 0x581/0x601 而不是新增 ID：下位机主动发起，C8T6 发 -> 0x581（原本定义为
 * 下位机响应方向），上位机回 -> 0x601（原本定义为上位机请求方向）。两侧过滤器天然匹配
 * （C8T6 收 0x601、ZET6 收 0x581），一行驱动都不用改。can_drv.h 里 REQ/RSP 的注释语义
 * 因此反转，消息类型靠帧内 opcode 区分，后续步骤沿用这套约定扩展。
 *
 * 帧格式（8 字节满帧）：
 *   C8T6 -> 0x581  B0=0x10(QUERY) B1=版本 B2~B7=保留
 *   ZET6 -> 0x601  B0=0x11(REPLY) B1=有/无(1/0) B2B3=长度(uint16小端) B4~B7=CRC低32位
 *
 * 阶段 2 新增三个 opcode（帧内 B0）：
 *   0x12 UPDATE_REQ  C8T6->ZET6：B1=版本 B2B3=长度(uint16) B4~B7=CRC32(回显 REPLY 收到的值)
 *                   回显长度+CRC 是为了让上位机确认"下位机要的正是我这份"，避免槽 A 在
 *                   查询与请求之间被新传覆盖时发错版本。
 *   0x13 DATA        ZET6->C8T6：B1B2=seq(uint16小端) B3~B7=5 字节数据（满帧 8 字节）
 *   0x14 ACK         C8T6->ZET6：B1B2=已正确收到的最后 seq，B3=0 正常 / 非 0 请求重传
 *
 * 为什么每帧只装 5 字节：CAN 帧上限 8 字节，B0 给 opcode、B1B2 给 seq，
 *   剩下的 B3~B7 恰好 5 字节数据。分帧数 = ceil(len/5)，ACK 每块回一次。
 *
 * 块级 ACK 的时序意义（方案 §7）：ZET6 连发一块后停下等 ACK，C8T6 利用这段总线
 *   静默期完成 W25Q 扇区擦写（最坏 400ms）；擦完才回 ACK 放行下一块。
 *   所以 ZET6 等 ACK 超时必须 >= 400ms + 传输余量。 */
#define OTA_CAN_OP_QUERY     0x10U
#define OTA_CAN_OP_REPLY     0x11U
#define OTA_CAN_OP_UPDATE_REQ 0x12U
#define OTA_CAN_OP_DATA      0x13U
#define OTA_CAN_OP_ACK       0x14U
#define OTA_CAN_PROTO_VER    0x01U
#define OTA_CAN_QUERY_TIMEOUT_MS  100U   /* 等邮箱腾空/发完；对端不在线时避免阻塞主循环 */
#define OTA_KEY_DEBOUNCE_MS  50U         /* 按键防抖时间，滤掉机械抖动与长按时长差异 */

/* 分帧参数。5B/帧：21204B ≈ 4241 帧，125k 下约 3.8s（不含擦写空窗）。 */
#define OTA_CAN_DATA_BYTES   5U
#define OTA_CAN_BLOCK_FRAMES 16U         /* 每块 16 帧=80B，攒够一块再擦写+回 ACK */
#define OTA_CAN_RCV_TIMEOUT_MS 1500U     /* 接收态等下一帧的超时；>400ms 擦写 + 传输余量 */

/* 会话状态：IDLE(等按键) -> WAIT_REPLY(已发查询) -> RECEIVING(已发更新请求，收 bin) */
#define OTA_CAN_ST_IDLE      0U
#define OTA_CAN_ST_WAIT_REPLY 1U
#define OTA_CAN_ST_RECEIVING 2U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* KEY1(PA0) 查询按键的边沿+防抖状态。PA0 是浮空输入、按下为低，
 * 用「电平变化重置计时 + 稳定 50ms 才确认」滤掉机械抖动；
 * s_key1_fired 保证按住不放只发一帧，松开后才允许下一次查询。 */
static uint8_t  s_key1_prev  = 1U;   /* 上一次电平，初值=高(未按下) */
static uint8_t  s_key1_fired = 0U;   /* 本次按下是否已发过查询帧 */
static uint32_t s_key1_t;

/* KEY2(PA1) 回滚按键的边沿+防抖状态，与 KEY1 同款。
 * 为什么不复用 KEY1 的变量：两个键各自独立，查询和回滚可能先后触发，
 * 共用一个 fired 锁会导致第二次按键被吞。 */
static uint8_t  s_key2_prev  = 1U;
static uint8_t  s_key2_fired = 0U;
static uint32_t s_key2_t;

/* ===== 阶段 2：CAN 接收会话状态 ===== */
static uint8_t  s_ota_st = OTA_CAN_ST_IDLE;   /* 当前会话状态 */
static uint32_t s_ota_len;                    /* REPLY 声明的固件长度（uint16 解析后） */
static uint32_t s_ota_crc;                    /* REPLY 声明的固件 CRC32 */
static uint16_t s_ota_expect_seq;             /* 期望收到的下一帧 seq */
static uint16_t s_ota_expect_frame;           /* 期望的总帧数 = ceil(len/5) */
static uint16_t s_ota_block_cnt;              /* 本块已收帧数，满 OTA_CAN_BLOCK_FRAMES 回一次 ACK */
static uint32_t s_ota_fed;                    /* 已 Feed 给 OtaW25q 的字节数（含扇区内未落盘的尾巴） */
static uint32_t s_ota_last_frame_ms;          /* 最近一帧到达时刻，收包超时用 */
static uint8_t  s_ota_write_err;              /* 落盘失败锁：出错后只重传、不再写 Flash */
/* 块缓冲：收满一块才一次性 Feed。为什么不能每帧直接 Feed：
 *   OtaW25q_Feed 攒满 4KB 会在调用内部擦扇区（最坏 400ms）。若每帧都调，擦写可能
 *   落在块的中间，此时 ZET6 还在连发后续帧，C8T6 卡在 SPI 上、8 级接收队列必溢出。
 *   攒成一块再 Feed，擦写就只发生在"收满一块、回 ACK 之前"——此时 ZET6 正停着等 ACK，
 *   总线天然静默，正是方案 §7 要的那个空窗。 */
static uint8_t  s_ota_block[OTA_CAN_BLOCK_FRAMES * OTA_CAN_DATA_BYTES];

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* App 区最大长度：0x08005000 ~ 0x08010000 = 44KB。REPLY 的长度是 uint16，
 * 可能超过这个上限；不去重直接拒收，避免 Crc32_Flash 读到 Flash 末尾之外。 */
#define OTA_CAN_APP_MAX   45056UL

/**
 * 回一块的 ACK。B1B2=已正确收到的最后 seq，B3=0 正常。
 * 为什么单独封装：ACK 在三个位置都要发（正常块满、收齐、出错重传），
 * 统一出口能保证帧格式一致，也方便以后加 ACK 的 CRC 保护。
 */
static void OtaCan_SendAck(uint16_t last_seq, uint8_t err)
{
	uint8_t ack[8];

	ack[0] = OTA_CAN_OP_ACK;
	ack[1] = (uint8_t)(last_seq & 0xFFU);
	ack[2] = (uint8_t)((last_seq >> 8) & 0xFFU);
	ack[3] = err;
	ack[4] = 0U; ack[5] = 0U; ack[6] = 0U; ack[7] = 0U;

	(void)CanDrv_Send(CANDRV_ID_RSP, ack, (uint8_t)sizeof(ack), OTA_CAN_QUERY_TIMEOUT_MS);
}

/**
 * 去重：槽 A 那份 bin 是否已经就是当前在跑的固件。
 * 判据 = REPLY 里的长度+CRC 与片内 App 区 [0x08005000, +len) 读回的 CRC 一致。
 * 这里用 Crc32_Flash 是对的（片内 XIP，直接解引用）；W25Q 那种 SPI 外挂才不能用。
 * 长度非法（0 或超 App 区）时返回 0 不拦，但外层会先拦掉非法长度。
 */
static uint8_t OtaCan_IsCurrentFw(uint32_t len, uint32_t crc)
{
	uint32_t now;

	if((len == 0U) || (len > OTA_CAN_APP_MAX))
	{
		return 0U;
	}
	now = Crc32_Flash(0x08005000UL, len);
	return (now == crc) ? 1U : 0U;
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

	/* 【阶段 1b】CAN 改为 Normal 真实接收（2026-11-25 改动）
	   背景：上位机 ZET6 已接入总线，每 1s 发一帧 0x601（8 字节测试帧），本侧要从引脚收。
	   为什么必须退出回环：F103 的 Loopback 模式把 TX 在内部环回给 RX，同时**断开 RX 引脚**，
	   所以回环状态下无论对端怎么发都收不到 —— 这不是过滤器或中断的问题。
	   真正要同时满足的是两道闸：
	     1) .ioc 的 CAN.Mode = NORMAL（已改）
	     2) CAN_STB(PB7) 拉低（gpio.c 已改；置高时收发器 Standby，RX 不出信号）
	   参数仍由 CubeMX 生成，CanDrv_Init() 只做「读回校验 + 配过滤器 + Start + 激活中断」。
	   错误码：-2 状态不对 / -6 位时序不符预期 / -7 CAN1 RX0 中断未使能。
	   回环分支与自测保留：把 .ioc 改回 LOOPBACK 时仍能跑 1a 那套验收。 */
	{
		int8_t cr = CanDrv_Init();

		if (cr == CANDRV_OK)
		{
			Uart_Printf(UART_LOG, "[can] init OK, %lu bps, sample %u.%u%%, %s\r\n",
						(unsigned long)CanDrv_ActualBaudrate(),
						(unsigned int)(CanDrv_ActualSamplePointPermille() / 10U),
						(unsigned int)(CanDrv_ActualSamplePointPermille() % 10U),
						(CanDrv_GetMode() == CANDRV_MODE_LOOPBACK) ? "loopback" : "normal");

			if (CanDrv_GetMode() == CANDRV_MODE_LOOPBACK)
			{
				int8_t sr = CanDrv_SelfTest();

				Uart_Printf(UART_LOG, "[can] loopback self-test %s (tx=%lu ovf=%lu err=%lu bof=%lu)\r\n",
							(sr == CANDRV_OK) ? "PASS" : "FAIL",
							(unsigned long)CanDrv_TxOkCount(),
							(unsigned long)CanDrv_RxOverflowCount(),
							(unsigned long)CanDrv_ErrorCount(),
							(unsigned long)CanDrv_BusOffCount());
			}
			else
			{
				/* Normal 模式没有对端时硬测只会得到误导性的 FAIL，所以不跑自测；
				   改为在主循环里轮询接收队列并打印（见 while 中那段）。
				   本侧过滤器只放行 0x601 —— 也就是上位机的请求帧。
				   2026-11-25 21:2x：Mode 已回到 NORMAL，但这里保留按 SILM 位动态取名的写法
				   （CanDrv_GetMode() 只看 LBKM 位，区分不了 SILENT/NORMAL），
				   这样以后再做单侧 SILENT 判别时，串口能一眼看出烧的是哪一版。 */
				Uart_Printf(UART_LOG, "[can] %s mode: 接收就绪，等待上位机 0x%03X\r\n",
							((hcan.Init.Mode & CAN_BTR_SILM) != 0U) ? "silent" : "normal",
							(unsigned int)CANDRV_ID_REQ);
			}
		}
		else
		{
			Uart_Printf(UART_LOG, "[can] init FAIL code=%d\r\n", (int)cr);
		}
	}

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
			// Led_Fan(1);
		}

		/* ---------- 串口喂数（阶段 2 起受会话状态控制） ----------
		   2026-11-26 用户确认：进入 CAN 接收态后只屏蔽"串口->W25Q 喂数"这一条路径，
		   日志 TX（Uart_Printf）照常，方便观察下载过程。
		   为什么必须屏蔽：串口和 CAN 若同时喂 OtaW25q，两个数据源会交错写进同一个
		   固件区，校验必然失败、且很难判断是哪路写坏了。
		   IDLE/WAIT_REPLY 态照常收串口；RECEIVING 态停喂，直到收齐或失败回到 IDLE。 */
		if(s_ota_st == OTA_CAN_ST_RECEIVING)
		{
			len = 0U;                 /* 不调用 Boot_Loader_Getlen()，串口字节留在环缓冲里不消费 */
		}
		else
		{
			len = Boot_Loader_Getlen();
		}
		sum += len;
		if (len > 0)
		{
			Uart_Printf(UART_LOG, "receive: %d  sum: %lu\r\n", len, sum);
		}

		/* ---------- KEY1：按键查询"上位机有没有新程序"（阶段 1） ----------
		   PA0 浮空输入，按下为低。为什么要防抖+边沿锁存而不是直接读电平：
		   1) 机械按键抖动会在几毫秒内反复跳变，不滤会发出一串查询帧；
		   2) 按住不放时电平持续为低，靠 s_key1_fired 保证只发一帧，松开才能再次触发。
		   发帧走 0x581（下位机->上位机方向），100ms 超时：正常一帧约 0.9ms，
		   超时只可能是对端掉线/没 ACK，此时打 FAIL 而不阻塞主循环。 */
		{
			uint8_t key1_now = (HAL_GPIO_ReadPin(KEY1_GPIO_Port, KEY1_Pin) == GPIO_PIN_RESET) ? 0U : 1U;

			if(key1_now != s_key1_prev)
			{
				s_key1_prev = key1_now;
				s_key1_fired = 0U;              /* 电平变化：重新计防抖，允许新的一次按下 */
				Timer_Reset(&s_key1_t);
			}
			else if((key1_now == 0U) && (s_key1_fired == 0U) &&
					(s_ota_st == OTA_CAN_ST_IDLE) &&    /* 下载中不接受新查询，防止状态机被打断 */
					(Timer_Timeout(&s_key1_t, OTA_KEY_DEBOUNCE_MS) != 0U))
			{
				uint8_t q[8];
				int8_t  sr;

				q[0] = OTA_CAN_OP_QUERY;
				q[1] = OTA_CAN_PROTO_VER;
				q[2] = 0U; q[3] = 0U; q[4] = 0U; q[5] = 0U; q[6] = 0U; q[7] = 0U;

				sr = CanDrv_Send(CANDRV_ID_RSP, q, (uint8_t)sizeof(q), OTA_CAN_QUERY_TIMEOUT_MS);
				s_key1_fired = 1U;

				/* 只有真的发出去才推进状态；发送 FAIL 时留在 IDLE，按键松开后可重试 */
				if(sr == CANDRV_OK)
				{
					s_ota_st = OTA_CAN_ST_WAIT_REPLY;
				}

				Uart_Printf(UART_LOG, "[can] query %s (id=0x%03lX)\r\n",
							(sr == CANDRV_OK) ? "sent" : "FAIL",
							(unsigned long)CANDRV_ID_RSP);
			}
		}

		/* ---------- KEY2(PA1)：人工回滚到备份固件 ----------
		   PA1 浮空输入，按下为低。为什么写标志而不在 App 里直接搬：
		   F103 单 bank，App 正在 0x08005000 取指时擦写同一 bank 必 HardFault。
		   复用 Boot 已实机验证的回滚链路 —— 这里把标志写成 APPLIED 再复位，
		   Boot 读到 APPLIED 就从 0x201000 备份区恢复、校验、擦标志、跳 App。

		   写标志前先校验备份头+数据 CRC：备份坏就不写、不复位。否则 Boot 回滚失败
		   停在 OTA 等串口，用户还以为按键没生效；fail-safe 优先于"一定回滚"。

		   ⚠ 语义：备份区存的是"最近一次确认过的固件"（App 启动时备份当时的片内）。
		   若刚升级完就按键2，备份区=当前这份新固件，回滚是把同一份重搬一遍 ——
		   它验证回滚链路通不通，不是退回更早版本。要退更早版本得改备份时机。 */
		{
			uint8_t key2_now = (HAL_GPIO_ReadPin(KEY2_GPIO_Port, KEY2_Pin) == GPIO_PIN_RESET) ? 0U : 1U;

			if(key2_now != s_key2_prev)
			{
				s_key2_prev = key2_now;
				s_key2_fired = 0U;
				Timer_Reset(&s_key2_t);
			}
			else if((key2_now == 0U) && (s_key2_fired == 0U) &&
					(s_ota_st == OTA_CAN_ST_IDLE) &&    /* 下载中不接受回滚，避免两路抢 W25Q */
					(Timer_Timeout(&s_key2_t, OTA_KEY_DEBOUNCE_MS) != 0U))
			{
				OtaBackupHeader_t hdr;
				int8_t hs = OtaBackup_ReadHeader(&hdr);

				s_key2_fired = 1U;   /* 无论备份好坏都锁存，按住不放不重复触发 */

				if(hs != OTA_BAK_OK)
				{
					Uart_Printf(UART_LOG, "[app] KEY2 rollback: no valid backup (%d), ignored\r\n", (int)hs);
				}
				else if(OtaBackup_VerifyData(&hdr) != 1)
				{
					Uart_Printf(UART_LOG, "[app] KEY2 rollback: backup CRC BAD, ignored\r\n");
				}
				else
				{
					/* length/crc 填备份头的：Boot 的 ApplyRollback 只用备份头自己的值，
					   这里填对是为了让标志自检自洽，日志也能对上备份固件。 */
					int8_t ws = OtaFlag_Write(hdr.length, hdr.crc32, OTA_FLAG_APPLIED);

					Uart_Printf(UART_LOG, "[app] KEY2 rollback: flag APPLIED %s, reset...\r\n",
								(ws == OTA_FLAG_OK) ? "OK" : "FAIL");
					if(ws == OTA_FLAG_OK)
					{
						HAL_NVIC_SystemReset();   /* 不返回：Boot 接管回滚 */
					}
				}
			}
		}

		/* ---------- CAN 接收状态机（阶段 2） ----------
		   过滤器只放行 0x601（上位机方向），每圈取干净：队列深度只有 8。
		   三条分支对应会话三态：
		     REPLY   (WAIT_REPLY)  -> 去重 -> 发 UPDATE_REQ -> 进 RECEIVING
		     DATA    (RECEIVING)   -> 攒块 -> 满块写 W25Q -> 回 ACK -> 收齐 Finish
		     ACK/其它               -> 忽略（本侧不收 ACK）
		   注意：所有写 Flash 都集中在 DATA 分支，且只在"回 ACK 之前"做完，
		   这样 ZET6 永远是在 C8T6 空闲时才被放行下一块。 */
		{
			can_drv_frame_t f;

			while (CanDrv_RxPop(&f) == CANDRV_OK)
			{
				/* REPLY：B1=有/无，B2B3=长度，B4~B7=CRC。只有发出过查询才接受，
				   否则滞留的旧 REPLY 会把空闲中的板子拽进下载态。 */
				if((f.id == CANDRV_ID_REQ) && (f.dlc == 8U) &&
				   (f.data[0] == OTA_CAN_OP_REPLY) && (s_ota_st == OTA_CAN_ST_WAIT_REPLY))
				{
					uint32_t len = (uint32_t)f.data[2] | ((uint32_t)f.data[3] << 8);
					uint32_t crc = (uint32_t)f.data[4] | ((uint32_t)f.data[5] << 8) |
								   ((uint32_t)f.data[6] << 16) | ((uint32_t)f.data[7] << 24);
					uint8_t  req[8];
					int8_t   sr;

					Uart_Printf(UART_LOG, "[can] reply %s len=%luB crc=0x%08lX\r\n",
								(f.data[1] != 0U) ? "YES" : "NO",
								(unsigned long)len, (unsigned long)crc);

					s_ota_st = OTA_CAN_ST_IDLE;   /* 无论 YES/NO，先离开 WAIT_REPLY */

					if(f.data[1] == 0U)
					{
						continue;    /* 上位机说没有：保持 IDLE */
					}
					/* 长度闸门：0 或超出 App 区都不接收，防止 CRC 越界读/写爆 W25Q */
					if((len == 0U) || (len > OTA_CAN_APP_MAX))
					{
						Uart_Printf(UART_LOG, "[ota] reply 长度非法 %luB，忽略\r\n", (unsigned long)len);
						continue;
					}
					/* 去重：槽里这份若就是当前在跑的固件，不必下载 */
					if(OtaCan_IsCurrentFw(len, crc) != 0U)
					{
						Uart_Printf(UART_LOG, "[ota] 已是当前固件，跳过下载\r\n");
						continue;
					}

					/* 回显长度+CRC 请求更新，让上位机确认发的是它报过的那份 */
					req[0] = OTA_CAN_OP_UPDATE_REQ;
					req[1] = OTA_CAN_PROTO_VER;
					req[2] = (uint8_t)(len & 0xFFU);
					req[3] = (uint8_t)((len >> 8) & 0xFFU);
					req[4] = (uint8_t)(crc & 0xFFU);
					req[5] = (uint8_t)((crc >> 8) & 0xFFU);
					req[6] = (uint8_t)((crc >> 16) & 0xFFU);
					req[7] = (uint8_t)((crc >> 24) & 0xFFU);
					sr = CanDrv_Send(CANDRV_ID_RSP, req, (uint8_t)sizeof(req), OTA_CAN_QUERY_TIMEOUT_MS);
					if(sr != CANDRV_OK)
					{
						Uart_Printf(UART_LOG, "[ota] UPDATE_REQ 发送 FAIL=%d\r\n", (int)sr);
						continue;
					}

					/* 干净交接：重置 OtaW25q 的写入起点与计数，丢弃串口路径残留状态。
					   注意 Boot_Loader_Init 内部调 OtaW25q_Init，会把 s_total/s_fill 清零。 */
					(void)Boot_Loader_Init();

					s_ota_len         = len;
					s_ota_crc         = crc;
					s_ota_expect_seq  = 0U;
					s_ota_expect_frame = (uint16_t)((len + (uint32_t)OTA_CAN_DATA_BYTES - 1U) /
													  (uint32_t)OTA_CAN_DATA_BYTES);
					s_ota_block_cnt   = 0U;
					s_ota_fed         = 0U;
					s_ota_last_frame_ms = Board_Millis();
					s_ota_write_err   = OTAW_OK;
					s_ota_st          = OTA_CAN_ST_RECEIVING;

					Uart_Printf(UART_LOG, "[ota] 开始 CAN 接收 %luB / %u 帧\r\n",
								(unsigned long)len, (unsigned int)s_ota_expect_frame);
				}
				/* DATA：B1B2=seq，B3~B7=5 字节数据。只在 RECEIVING 态处理。 */
				else if((f.id == CANDRV_ID_REQ) && (f.dlc == 8U) &&
						(f.data[0] == OTA_CAN_OP_DATA) && (s_ota_st == OTA_CAN_ST_RECEIVING))
				{
					uint16_t seq = (uint16_t)((uint16_t)f.data[1] | ((uint16_t)f.data[2] << 8));
					uint32_t off = (uint32_t)s_ota_block_cnt * OTA_CAN_DATA_BYTES;
					uint8_t  i;

					s_ota_last_frame_ms = Board_Millis();

					/* seq 不连续 = 丢帧。回一个带 err 的 ACK 让上位机立即停，别等超时。
					   C8T6 不实现重传（当前流程靠上位机重跑一轮），只保证不把错数据写进 Flash。 */
					if(seq != s_ota_expect_seq)
					{
						Uart_Printf(UART_LOG, "[ota] seq 错：期望 %u 收到 %u，放弃\r\n",
									(unsigned int)s_ota_expect_seq, (unsigned int)seq);
						OtaCan_SendAck(s_ota_expect_seq, 1U);
						s_ota_st = OTA_CAN_ST_IDLE;
						continue;
					}
					/* 防御：块缓冲固定 16*5=80B，块满必须已回 ACK 并清零，不该越界 */
					if((off + OTA_CAN_DATA_BYTES) > sizeof(s_ota_block))
					{
						Uart_Printf(UART_LOG, "[ota] 块缓冲越界，放弃\r\n");
						OtaCan_SendAck(s_ota_expect_seq, 1U);
						s_ota_st = OTA_CAN_ST_IDLE;
						continue;
					}
					for(i = 0U; i < OTA_CAN_DATA_BYTES; i++)
					{
						s_ota_block[off + i] = f.data[3U + i];
					}
					s_ota_expect_seq++;
					s_ota_block_cnt++;

					/* 收齐与块满互斥处理：收齐优先。
					   为什么必须互斥：最后一帧可能同时让 block_cnt 达到 16 且 expect_seq 达到总数，
					   若两个分支各回一次 ACK，上位机会把第二个误当成"下一块已确认"。
					   收齐时 ACK 放在 Finish 之后发，它才真正代表"落盘+校验+写标志全成功"。 */
					if(s_ota_expect_seq >= s_ota_expect_frame)
					{
						int8_t fr;

						/* 把最后不足一块的尾巴 Feed 掉，按声明长度裁掉末帧填充字节。
						   ⚠ remain 必须用 s_ota_fed（已 Feed 字节数）算，不能用 OtaW25q_Total()：
						   后者只统计已落盘的整扇区，扇区里攒着的尾巴不算，用它算会多算出
						   几百字节，导致末帧的补 0 字节被当真数据写进固件、CRC 对不上。 */
						if(s_ota_block_cnt > 0U)
						{
							uint32_t fed = (uint32_t)s_ota_block_cnt * OTA_CAN_DATA_BYTES;
							uint32_t remain = s_ota_len - s_ota_fed;

							if(remain < fed)
							{
								fed = remain;
							}
							if((fed > 0U) && (OtaW25q_Feed(s_ota_block, fed) != OTAW_OK))
							{
								Uart_Printf(UART_LOG, "[ota] 尾块写入 FAIL，放弃\r\n");
								OtaCan_SendAck((uint16_t)(s_ota_expect_seq - 1U), 1U);
								s_ota_st = OTA_CAN_ST_IDLE;
								continue;
							}
							s_ota_fed += fed;
						}
						/* Finish：补尾扇区 + 逐字节回读比对 + 算 CRC + 写 PENDING 标志。
						   这一步之后 Boot 复位就能搬运，全程不需要额外写标志的代码。 */
						fr = Boot_Loader_Finish();
						/* ACK 的 seq = 最后一帧的序号 = expect_seq-1。上位机按这个值校验，
						   若错填 expect_seq（帧数），它会判成"上一块的迟到 ACK"而中止。 */
						OtaCan_SendAck((uint16_t)(s_ota_expect_seq - 1U), (fr == 0U) ? 0U : 1U);
						Uart_Printf(UART_LOG, "[ota] 收齐 %u 帧，Finish=%d，total=%luB\r\n",
									(unsigned int)s_ota_expect_seq, (int)fr,
									(unsigned long)OtaW25q_Total());
						s_ota_block_cnt = 0U;
						s_ota_st = OTA_CAN_ST_IDLE;
					}
					else if(s_ota_block_cnt >= OTA_CAN_BLOCK_FRAMES)
					{
						/* 收满一块 -> 一次性 Feed（擦写发生在这里，此刻上位机在等 ACK）-> 回 ACK */
						int8_t wr = OtaW25q_Feed(s_ota_block, (uint32_t)s_ota_block_cnt * OTA_CAN_DATA_BYTES);

						if(wr != OTAW_OK)
						{
							s_ota_write_err = (uint8_t)wr;
							Uart_Printf(UART_LOG, "[ota] W25Q 写入 FAIL=%d，放弃\r\n", (int)wr);
							OtaCan_SendAck(s_ota_expect_seq, 1U);
							s_ota_st = OTA_CAN_ST_IDLE;
							continue;
						}
						s_ota_fed += (uint32_t)s_ota_block_cnt * OTA_CAN_DATA_BYTES;
						OtaCan_SendAck((uint16_t)(s_ota_expect_seq - 1U), 0U);
						s_ota_block_cnt = 0U;
					}
				}
			}
		}

		/* RECEIVING 态收包超时：上位机中途掉线/停发时，别让会话永远挂着占住串口屏蔽 */
		if((s_ota_st == OTA_CAN_ST_RECEIVING) &&
		   (Timer_Timeout(&s_ota_last_frame_ms, OTA_CAN_RCV_TIMEOUT_MS) != 0U))
		{
			Uart_Printf(UART_LOG, "[ota] 接收超时，got=%u/%u 帧，放弃\r\n",
						(unsigned int)s_ota_expect_seq, (unsigned int)s_ota_expect_frame);
			s_ota_st = OTA_CAN_ST_IDLE;
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
