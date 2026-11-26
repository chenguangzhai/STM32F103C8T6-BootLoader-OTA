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
#include "can.h"            /* hcan：CAN 句柄由 CubeMX 生成，.ioc 已启用 CAN */

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include "board.h"
#include "uart_app.h"
#include "ota_app.h"		/* 串口数据攒成 4KB 扇区后写外部 Flash */
#include "w25q.h"			/* 外部 Flash；spi.h 已由上方 CubeMX 生成区包含 */
#include "ota_slot.h"		/* 槽 A 头部：长度/CRC 在收尾时最后写入 */
#include "ota_param.h"		/* 只用来解析上位机补发的 12 字节尾部记录 */
#include "crc32.h"			/* 2026-11-26：空闲自动收尾时从 W25Q 读回数据算 CRC */
#include "ota_log.h"		/* 黑匣子：17:36 起不再记复位、也不再倒出 [log] 历史，
						   只剩接收循环里那条 OTA_MODE 事件（当前无任何地方打印它） */
#include "can_drv.h"		/* CAN：收 0x581 下位机查询 / 发 0x601 应答；阶段 1 握手 */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* 串口超过这么多毫秒没有新数据，就把还不足一扇区的尾巴也写进外部 Flash。
 * 否则最后一扇区（比如最后 928 字节）会永远留在 RAM 里写不下去。 */
#define OTA_IDLE_FLUSH_MS   2000U

/* ==================== CAN OTA 会话层：阶段 1 查询应答 ====================
 * 方向与 C8T6 侧对应：下位机按键后发 0x581(QUERY)，本侧收；本侧查完槽 A 后发
 * 0x601(REPLY)。两侧过滤器天然匹配（本侧只收 0x581），驱动不用改。
 * REPLY 帧：B0=0x11 B1=有/无(1/0) B2B3=长度(uint16小端) B4~B7=CRC低32位小端。
 * "有新程序"的判据：OtaSlot_ReadHeader(OTA_SLOT_A) == OTA_SLOT_OK，
 * 即上次串口收 bin 时 FinishOta 回读校验通过、槽头已写——只有完整固件才会写头。
 * 阶段 1 只报有无，不发固件、不改槽头 flags。 */
#define OTA_CAN_OP_QUERY     0x10U
#define OTA_CAN_OP_REPLY     0x11U
#define OTA_CAN_OP_UPDATE_REQ 0x12U      /* 下位机确认要这份：回显长度+CRC */
#define OTA_CAN_OP_DATA      0x13U      /* 分帧数据：B1B2=seq B3~B7=5B 数据 */
#define OTA_CAN_OP_ACK       0x14U      /* 下位机块确认：B1B2=最后 seq B3=0 正常/非0 请求停 */
#define OTA_CAN_PROTO_VER    0x01U
#define OTA_CAN_REPLY_TIMEOUT_MS  100U   /* 等邮箱腾空；无 ACK 时不让主循环卡死 */

/* 阶段 2 分帧参数：必须与 C8T6 侧 main.c 逐字一致，否则双方对不上 */
#define OTA_CAN_DATA_BYTES   5U         /* DATA 帧载荷：B3~B7 */
#define OTA_CAN_BLOCK_FRAMES 16U        /* 每 16 帧等一次 ACK；给下位机留出擦写空窗 */
#define OTA_CAN_ACK_TIMEOUT_MS 1500U    /* 等 ACK 超时：>下位机最坏 400ms 擦写 + 传输余量 */
#define OTA_CAN_SEND_GAP_MS  1U         /* 块内帧间延时：给下位机主循环留出取队列的时间 */

/* 上位机发送状态机：IDLE -> SENDING(正在分帧发槽 A) */
#define OTA_TX_ST_IDLE       0U
#define OTA_TX_ST_SENDING    1U

/* 2026-11-26：CAN 周期测试帧已整体移除（宏、序号变量、主循环发送段）。
 * 原因：用户要求不再持续发送——1Hz 测试帧会一直占用总线、持续刷串口日志，
 *       干扰后续的串口落盘观察。物理层已在 11-25 22:05 验证通过（125k，ESR=0），
 *       不再需要常驻心跳。
 * 副作用/注意：CanDrv_Send() 与 CAN 初始化仍保留，外设停在就绪态；
 *             以后需要发帧时由调用方显式给超时参数，不依赖这里的宏。 */

/* 2026-11-25 17:36：复位原因快照（RST_CAUSE_* 宏、s_rst_cause、ResetCause_Snapshot()）
   全部删除。用户要求"不记录复位记录"，而这些代码的唯一用途就是给黑匣子填一笔
   RST_* 事件 + 开机打 [rst] 行；两处都已去掉，留着只会变成死代码/未使用警告。
   副作用：RCC->CSR 的复位标志不再被清除，也无人读取——对运行没有任何影响
   （标志只是累积在原地）。以后若要恢复"看复位原因"，得从 RCC->CSR 重新取。 */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* 从串口取数据的临时区。放静态区：512 字节压栈太靠近 1KB 栈底。
   攒扇区缓冲已经搬进 ota_app.c，main 里不再需要 s_page。 */
static uint8_t s_rx[512];

/* 2026-11-25：本次上电是否已经开始接收。第一包数据到达时才擦槽 A 头部，
   这样"没收到新数据"时，上次保存好的槽 A 内容不会被开机动作破坏，
   后续才能经 CAN 把已存内容发出去。 */
static uint8_t s_ota_started = 0U;

/* 2026-11-26：本次上电是否已经"收尾"过（收到合法 OTAE 尾部 / 空闲自动写槽头，二者其一）。
   为什么要它：串口助手认不出 ZET6 的 [recv] 就绪日志，会判成老协议而不发 12 字节尾部；
   空闲自动收尾是为了兜住这种情况。但一旦尾部来过，哪怕 FinishOta 校验失败，
   也不能让空闲路径把这份坏数据扶正成有效槽 —— 标志就是这两条路径的互斥锁。 */
static uint8_t s_tail_seen = 0U;

/* ===== 阶段 2：CAN 分帧发送状态（把槽 A 的 bin 发给下位机） ===== */
static uint8_t  s_tx_st = OTA_TX_ST_IDLE;     /* 发送状态 */
static uint32_t s_tx_addr;                    /* 当前读到 W25Q 的哪个地址（槽 A 数据区相对地址） */
static uint32_t s_tx_left;                    /* 还剩多少字节没发 */
static uint16_t s_tx_seq;                     /* 当前帧序号，发完自增 */
static uint16_t s_tx_total_frame;             /* 总帧数 = ceil(len/5) */
static uint16_t s_tx_block_cnt;               /* 本块已发帧数，满 16 就停下来等 ACK */
static uint32_t s_tx_ack_tick;                /* 等 ACK 的起点，超时保护 */
static uint8_t  s_tx_wait_ack;                /* 1=本块发完，正在等下位机 ACK */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/**
  * 函    数：FinishOta
  * 功    能：收到尾部记录后的收尾：Flush → 长度比对 → 回读校验 → 写槽 A 头部
  * 说    明：2026-11-25 改写。原来这里是"校验片内 Flash → 存参数 → 跳 App"，
  *           本工程不再跳 App，也不写片内 Flash；数据就是待发送内容，
  *           存在槽 A，头部最后写，写完这份数据就算可用。
  *           任何一步失败都 return，板子继续停在串口接收循环等重传。
  */
static void FinishOta(const OtaTail_t *tail)
{
	OtaSlotHeader_t hdr;
	int8_t          vr;

	Uart_Printf(UART_LOG, "[ota] 尾部：长度=%lu CRC=0x%08lX\r\n",
				(unsigned long)tail->length, (unsigned long)tail->crc32);

	/* 先把 RAM 里攒着的尾巴写出去。不 Flush 的话外部 Flash 里缺结尾一段，
	   下面读回来算的 CRC 必然对不上 */
	if(Ota_Flush() != OTA_OK)
	{
		Uart_Printf(UART_LOG, "[ota] 落盘失败\r\n");
		return;
	}

	if(Ota_Total() != tail->length)
	{
		Uart_Printf(UART_LOG, "[ota] 长度不符：收到 %lu，应为 %lu\r\n",
					(unsigned long)Ota_Total(),
					(unsigned long)tail->length);
		return;
	}

	/* 头部先在 RAM 里构造好，用它校验外部 Flash 里的数据区。
	   OtaSlot_VerifyData 是"读回来算"—— 能证明 SPI 写进去的内容是对的，
	   只信边收边算的 CRC 证明不了这一点。 */
	OtaSlot_BuildHeader(&hdr, 0U, tail->length, tail->crc32, 0U, 0U);
	vr = OtaSlot_VerifyData(OTA_SLOT_A, &hdr);
	if(vr != 1)
	{
		Uart_Printf(UART_LOG, "[ota] 外部 Flash 校验失败（%d）\r\n", (int)vr);
		return;
	}
	Uart_Printf(UART_LOG, "[ota] 校验通过 %luB\r\n", (unsigned long)tail->length);

	/* 头部最后写：写完它就等于"这份数据完整且校验通过"，
	   将来从 CAN 发出时可以直接拿头部里的长度/CRC */
	if(OtaSlot_WriteHeader(OTA_SLOT_A, &hdr) != OTA_SLOT_OK)
	{
		Uart_Printf(UART_LOG, "[ota] 槽 A 头部写入失败\r\n");
		return;
	}

	Uart_Printf(UART_LOG, "[ota] 槽 A 已保存：%luB CRC=0x%08lX\r\n",
				(unsigned long)tail->length, (unsigned long)tail->crc32);
}

/**
  * 函    数：HandleRx
  * 功    能：统一处理收到的一包数据
  * 返 回 值：1 = 这包是尾部记录（收尾流程已接管）
  *           0 = 普通数据，已喂给 ota_app 写外部 Flash
  */
static uint8_t HandleRx(const uint8_t *buf, uint32_t len)
{
	OtaTail_t tail;
	uint32_t  i;
	uint32_t  total = Ota_Total() + Ota_Fill();

	/* 在整个数据块里找尾部记录，而不是只看块首。
	   串口是流式的：上位机发完最后一块会紧接着发尾部，
	   一次 Uart_Read 完全可能把「末尾若干字节 + 12 字节尾部」合并成一块。
	   只看块首的话 magic 根本不在开头，这 12 字节会被当成数据写进 Flash。 */
	for(i = 0U; (i + OTA_TAIL_BYTES) <= len; i++)
	{
		/* recv_total 传 total + i：扫到第 i 字节时，这 i 字节还没落盘 */
		if(Ota_ParseTail(&buf[i], OTA_TAIL_BYTES, total + i, &tail) != 0U)
		{
			/* 2026-11-26：尾部来过就锁死空闲自动收尾，无论 FinishOta 后续成功与否。
			   否则"尾部 CRC 对不上 → FinishOta 提前 return → 2 秒空闲又把坏数据写成有效槽"，
			   等于把校验失败静默改成成功，是最危险的失效方式。 */
			s_tail_seen = 1U;

			if(i > 0U)
			{
				if(Ota_Feed(buf, i) != OTA_OK)
				{
					Uart_Printf(UART_LOG, "[ota] 尾部之前写入失败\r\n");
					return 1U;
				}
			}
			FinishOta(&tail);
			return 1U;
		}
	}

	/* 槽 A 数据区只有 508KB，越过就会踩到槽 B —— 宁可报错也不越界 */
	if((Ota_Total() + Ota_Fill() + len) > OTA_SLOT_DATA_MAX)
	{
		Uart_Printf(UART_LOG, "[ota] 溢出：%lu > %lu\r\n",
					(unsigned long)(Ota_Total() + Ota_Fill() + len),
					(unsigned long)OTA_SLOT_DATA_MAX);
		return 1U;
	}

	if(Ota_Feed(buf, len) != OTA_OK)
	{
		Uart_Printf(UART_LOG, "[ota] 写 0x%08lX 失败，累计 %lu\r\n",
					(unsigned long)Ota_Addr(), (unsigned long)Ota_Total());
	}
	return 0U;
}

/**
 * 函    数：CalcSlotDataCrc —— 从 W25Q 分块读回槽数据算 CRC32
 * 参    数：addr W25Q 内部地址；len 字节数；out_crc 输出算出的 CRC32
 * 返 回 值：0 = 成功，负数 = SPI 读失败（此时 *out_crc 不可用）
 * 为什么不用 Crc32_Flash()：那函数用 *(volatile const uint8_t*)addr 直接解引用，
 *   只对能内存映射(XIP)的片内 Flash 成立。W25Q 是 SPI 外挂、不支持 XIP，
 *   OtaSlot_DataAddr() 返回的 0x001000 是 W25Q 内部地址，直接解引用读到的
 *   其实是 STM32 地址空间里别的位置（本例落在片内 Flash 别名区），
 *   算出来的 CRC 与固件内容毫无关系 —— 2026-11-26 就踩过这个坑。
 * 为什么要分块：槽最大 508KB，F103 RAM 有限，不能一次读进内存。
 *   算法与 ota_slot.c::OtaSlot_VerifyData 一致（Crc32_Update 分段累计 + 最后取反），
 *   保证这里算出的值和上位机 zlib.crc32 口径相同。
 */
static int8_t CalcSlotDataCrc(uint32_t addr, uint32_t len, uint32_t *out_crc)
{
	static uint8_t s_buf[W25Q_PAGE_SIZE];	/* 静态区不进栈，复用一块 256B */
	uint32_t crc = CRC32_INIT;
	uint32_t left = len;

	if(out_crc == NULL)
	{
		return -1;
	}
	while(left > 0U)
	{
		uint32_t chunk = (left > W25Q_PAGE_SIZE) ? W25Q_PAGE_SIZE : left;

		if(W25q_Read(addr, s_buf, chunk) != W25Q_OK)
		{
			return -1;
		}
		crc = Crc32_Update(crc, s_buf, chunk);
		addr += chunk;
		left -= chunk;
	}
	*out_crc = ~crc;	/* Crc32_Update 不做最终取反，这里补上，与 Crc32_Buf 一致 */
	return 0;
}

/**
 * 发一帧 DATA：从槽 A 的当前读位置取 5 字节，seq 填 B1B2，凑满 8 字节发出。
 * 为什么用 W25q_Read 而不是内存缓冲：槽 A 最大 508KB，RAM 放不下，只能边读边发。
 * 末帧不足 5 字节时补 0 —— 下位机按 REPLY 里的长度裁掉填充，不会多写字节。
 */
static int8_t OtaCan_SendDataFrame(uint16_t seq)
{
	uint8_t  frame[8];
	uint32_t chunk;
	int8_t   r;

	chunk = (s_tx_left > OTA_CAN_DATA_BYTES) ? OTA_CAN_DATA_BYTES : s_tx_left;

	frame[0] = OTA_CAN_OP_DATA;
	frame[1] = (uint8_t)(seq & 0xFFU);
	frame[2] = (uint8_t)((seq >> 8) & 0xFFU);
	frame[3] = 0U; frame[4] = 0U; frame[5] = 0U; frame[6] = 0U; frame[7] = 0U;

	if(chunk > 0U)
	{
		if(W25q_Read(s_tx_addr, &frame[3], chunk) != W25Q_OK)
		{
			return -1;    /* 读槽失败，绝对不能发全 0 冒充数据 */
		}
	}

	r = CanDrv_Send(CANDRV_ID_REQ, frame, (uint8_t)sizeof(frame), OTA_CAN_REPLY_TIMEOUT_MS);
	if(r != CANDRV_OK)
	{
		return r;
	}

	s_tx_addr += chunk;
	s_tx_left -= chunk;
	return CANDRV_OK;
}

/**
 * 处理一帧 ACK（由主循环的统一分发点喂进来，不在这里取队列）。
 * 为什么不能再开一个 while 取队列：CAN 只有一个接收队列，两处同时取会互相吞帧。
 * 校验 ACK 的 seq 必须是本块最后一帧，防止上一块的迟到 ACK 被误认。
 * 返回 1=正常可发下一块；-1=下位机报错中止；0=不是 ACK。
 */
static int8_t OtaCan_OnAck(const can_drv_frame_t *f)
{
	uint16_t got;

	if((f->id != CANDRV_ID_RSP) || (f->dlc < 4U) || (f->data[0] != OTA_CAN_OP_ACK))
	{
		return 0;
	}
	got = (uint16_t)((uint16_t)f->data[1] | ((uint16_t)f->data[2] << 8));
	if((f->data[3] != 0U) || (got != (uint16_t)(s_tx_seq - 1U)))
	{
		Uart_Printf(UART_LOG, "[can] ACK 异常 seq=%u err=%u，下发中止\r\n",
					(unsigned int)got, (unsigned int)f->data[3]);
		return -1;
	}
	return 1;
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
  /* 2026-11-25：补上 CAN 外设初始化。漏掉时 hcan 一直停在 RESET 态，
     后面的 CanDrv_Init() 会在状态检查处直接返回 -2，日志只显示 init FAIL。 */
  MX_CAN_Init();
  /* 2026-11-25：按野火 ZET6 核心板规格书，板载 W25Q 在 SPI1(PA4~PA7)。 */
  MX_SPI1_Init();
  /* USER CODE BEGIN 2 */
  Uart_Init();

	/* 【阶段 1b】ZET6 侧 CAN 真实收发（2026-11-25 改动：由回环改为往总线发）
	   参数由 .ioc 生成（本轮 .ioc 的 CAN.Mode 已由 LOOPBACK 改成 NORMAL），
	   CanDrv_Init() 仍只做读回校验 + 配过滤器 + Start + 激活中断；
	   错误码：-2 状态 / -6 位时序 / -7 CAN RX0 中断未使能。
	   回环分支保留：将来若把 .ioc 改回 LOOPBACK（比如没有第二节点时自测），这段仍然可用。 */
	{
		int8_t cr = CanDrv_Init();

		if(cr == CANDRV_OK)
		{
			Uart_Printf(UART_LOG, "[can] init OK, %lu bps, sample %u.%u%%, %s\r\n",
						(unsigned long)CanDrv_ActualBaudrate(),
						(unsigned int)(CanDrv_ActualSamplePointPermille() / 10U),
						(unsigned int)(CanDrv_ActualSamplePointPermille() % 10U),
						(CanDrv_GetMode() == CANDRV_MODE_LOOPBACK) ? "loopback" : "normal");

			if(CanDrv_GetMode() == CANDRV_MODE_LOOPBACK)
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
				/* Normal 模式不跑开机自测：那会发一帧已知内容等回环，真实总线上
				   没有对端配合时只会得到误导性的 FAIL。周期测试帧已于 2026-11-26 移除，
				   现在 CAN 只保持"初始化完成、随时可发"的状态，不再自动占总线。 */
				Uart_Printf(UART_LOG, "[can] normal mode: 接收就绪，不主动发帧\r\n");
			}
		}
		else
		{
			Uart_Printf(UART_LOG, "[can] init FAIL code=%d\r\n", (int)cr);
		}
	}

  // Led_Fan(1);

	/* ---------- 外部 Flash 自检 ----------
	   串口数据要落到 W25Q 槽 A，所以先确认 SPI1 链路是通的：
	   看到 0xEF4017 就说明接线/时序/片选都对。 */
	{
		uint32_t w25q_id = 0xFFFFFFFFUL;

		if(W25q_Init(&hspi1, W25Q128_CS_GPIO_Port, W25Q128_CS_Pin) == W25Q_OK)
		{
			w25q_id = W25q_ReadId();
		}
		Uart_Printf(UART_LOG, "[boot] W25Q 芯片ID=0x%06lX %s，应为 0x%06lX\r\n",
					(unsigned long)w25q_id,
					(w25q_id == W25Q_JEDEC_ID) ? "正常" : "异常",
					(unsigned long)W25Q_JEDEC_ID);
	}

	/* 2026-11-25 17:36：黑匣子的"倒出历史（[log] 最近 N 条）"与"追加本次复位事件"
	   一并删除（用户要求）。注意 ota_log 模块本身仍在编译：接收循环开始新传输时会
	   记一条 OTA_MODE 事件，只是已经没有任何地方把它打印出来了。 */

	/* 2026-11-25 17:30：槽状态只读打印（[slot] A/B + 十六进制转储）整块删除。
	   槽头部仍由 ota_slot.c 读写（FinishOta 校验/写头要用），只是开机不再刷屏。
	   要看槽内容时，收尾日志里的 [ota] 槽 A 已保存 已经带长度和 CRC。 */

	/* 2026-11-25：不再有上电窗口、不再跳 App，开机就停在接收循环。
	   Ota_Init 先设一次起点；真正开始接收时还会重设一次并擦掉旧头部。 */
	Ota_Init(OtaSlot_DataAddr(OTA_SLOT_A));

	Uart_Printf(UART_LOG, "\r\n[recv] 就绪，外部 Flash 槽A 数据起点=0x%08lX，容量=%luB，协议=2\r\n",
				(unsigned long)OtaSlot_DataAddr(OTA_SLOT_A),
				(unsigned long)OTA_SLOT_DATA_MAX);

	uint16_t len;						/* 这次从串口取到多少字节 */
	uint32_t last_rx = Board_Millis();	/* 最后一次收到数据的时刻 */

	/* 2026-11-26：can_tx_tick / can_tx_seq 随周期测试帧一并删除。
	   保留 last_rx 供串口空闲 Flush 使用。 */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	len = Uart_Read(UART_LOG, s_rx, sizeof(s_rx));	/* 取到 0 表示这段时间没有新数据 */

	if(len > 0U)
	{
		last_rx = Board_Millis();

		/* 本次上电的第一包数据 = 新一次传输开始。
		   先擦槽 A 头部让旧内容失效，再从数据区起点重写；
		   擦头部是这个流程里唯一会破坏已有数据的动作，所以只在这里做一次。 */
		if(s_ota_started == 0U)
		{
			(void)OtaSlot_EraseHeader(OTA_SLOT_A);
			Ota_Init(OtaSlot_DataAddr(OTA_SLOT_A));
			s_ota_started = 1U;
			(void)OtaLog_Append(OTA_LOG_EVT_OTA_MODE, 0U, 0U, (uint32_t)len);
			Uart_Printf(UART_LOG, "[recv] 开始接收 槽A 0x%08lX\r\n",
						(unsigned long)OtaSlot_DataAddr(OTA_SLOT_A));
		}

		/* 攒扇区 + 落盘藏在 Ota_Feed 里；先分辨这包是不是尾部记录 */
		HandleRx(s_rx, len);
	}
	else if((s_ota_started != 0U) && (s_tail_seen == 0U) &&
			(Ota_Total() > 0U) && (Timer_Timeout(&last_rx, OTA_IDLE_FLUSH_MS) != 0U))
	{
		/* 2026-11-26：空闲自动收尾（替代缺失的 OTAE 尾部记录）。
		   为什么加：串口助手靠 [ota] ready...proto=2 判协议，ZET6 打的是 [recv] 就绪，
		   匹配不上就当老协议、不发尾部，结果数据写全了却没写槽头，CAN 查询永远 reply NO。
		   2 秒无新数据就当作"这一包传完了"：先把不足一扇区的尾巴落盘，
		   再从 W25Q 读回数据算 CRC 并写槽头，效果和收到尾部记录一样。

		   触发条件为什么从 Ota_Fill()>0 改成 Ota_Total()>0：
		   最后一段若刚好凑满 4096 被 Ota_Feed 直接落盘，Ota_Fill()==0，
		   旧条件会让分支进不去、槽头漏写（偶发 reply NO，很难查）。

		   取舍（必须知道）：CRC 是"从 Flash 读回自己算"，不是上位机给的独立校验源，
		   只能证明落盘内容稳定，证明不了串口传输过程没丢字节；
		   另外传一半断线也会被当成完整固件 —— 要堵这个洞得加长度预声明。
		   2026-11-26 修复：原实现用 Crc32_Flash() 直接解引用地址算 CRC，那是给片内
		   XIP Flash 用的；W25Q 走 SPI、不支持内存映射，算出来是片内别处的 CRC
		   （实机得到 0x9C69185F，与真实 bin 的 0x91C2B6C8 不符）。改用 CalcSlotDataCrc()
		   经 W25q_Read 分块读回再算，才和上位机 zlib 口径一致。 */
		if(Ota_Flush() == OTA_OK)
		{
			uint32_t n = Ota_Total();
			uint32_t crc = 0U;
			OtaSlotHeader_t hdr;

			if(CalcSlotDataCrc(OtaSlot_DataAddr(OTA_SLOT_A), n, &crc) == 0)
			{
				OtaSlot_BuildHeader(&hdr, 0U, n, crc, 0U, 0U);
				if(OtaSlot_WriteHeader(OTA_SLOT_A, &hdr) == OTA_SLOT_OK)
				{
					Uart_Printf(UART_LOG, "[ota] 自动收尾：槽 A 已保存 %luB CRC=0x%08lX\r\n",
								(unsigned long)n, (unsigned long)crc);
				}
				else
				{
					Uart_Printf(UART_LOG, "[ota] 自动收尾：槽 A 头部写入失败\r\n");
				}
			}
			else
			{
				/* 读回失败就不写槽头：宁可 CAN 查询回"无"，也不能广播一个错 CRC */
				Uart_Printf(UART_LOG, "[ota] 自动收尾：读回 CRC 失败，槽头未写\r\n");
			}
		}
		else
		{
			Uart_Printf(UART_LOG, "[ota] 自动收尾：尾巴落盘失败\r\n");
		}
		s_tail_seen = 1U;	/* 只做一次，避免每 2 秒重复写同一个扇区耗寿命 */
	}

	/* ---------- CAN 统一分发（阶段 1 查询 + 阶段 2 分帧下发） ----------
	   只有一个接收队列，所有 0x581 帧必须在这里一次性取干净、按 opcode 分发：
	     QUERY       -> 查槽 A 头，回 REPLY（有/无 + 长度 + CRC）
	     UPDATE_REQ  -> 校验回显的长度/CRC 与槽头一致，启动分帧发送
	     ACK         -> 交 OtaCan_OnAck，放行下一块 / 报错中止
	   ⚠ 每圈取干净：队列深度只有 8，发送时若压着不取，ACK 会被后面的帧挤掉。 */
	{
		can_drv_frame_t f;

		while(CanDrv_RxPop(&f) == CANDRV_OK)
		{
			if((f.id == CANDRV_ID_RSP) && (f.dlc == 8U) && (f.data[0] == OTA_CAN_OP_QUERY))
			{
				OtaSlotHeader_t hdr;
				int8_t hs = OtaSlot_ReadHeader(OTA_SLOT_A, &hdr);
				uint8_t rep[8];
				int8_t sr;

				rep[0] = OTA_CAN_OP_REPLY;
				rep[1] = (hs == OTA_SLOT_OK) ? 1U : 0U;   /* 有/无新程序 */
				rep[2] = (uint8_t)(hdr.fw_len & 0xFFU);   /* 长度只取 16 位：当前 bin 约 20KB，够用 */
				rep[3] = (uint8_t)(hdr.fw_len >> 8);
				rep[4] = (uint8_t)(hdr.fw_crc & 0xFFU);   /* CRC 低 32 位，小端 */
				rep[5] = (uint8_t)((hdr.fw_crc >> 8)  & 0xFFU);
				rep[6] = (uint8_t)((hdr.fw_crc >> 16) & 0xFFU);
				rep[7] = (uint8_t)((hdr.fw_crc >> 24) & 0xFFU);

				sr = CanDrv_Send(CANDRV_ID_REQ, rep, (uint8_t)sizeof(rep), OTA_CAN_REPLY_TIMEOUT_MS);

				Uart_Printf(UART_LOG, "[can] query from host: slot A %s, reply %s %s (len=%luB crc=0x%08lX)\r\n",
							(hs == OTA_SLOT_OK) ? "valid" : "none",
							(rep[1] != 0U) ? "YES" : "NO",
							(sr == CANDRV_OK) ? "sent" : "FAIL",
							(unsigned long)hdr.fw_len, (unsigned long)hdr.fw_crc);
			}
			else if((f.id == CANDRV_ID_RSP) && (f.dlc == 8U) &&
					(f.data[0] == OTA_CAN_OP_UPDATE_REQ) && (s_tx_st == OTA_TX_ST_IDLE))
			{
				OtaSlotHeader_t hdr;
				uint32_t want_len = (uint32_t)f.data[2] | ((uint32_t)f.data[3] << 8);
				uint32_t want_crc = (uint32_t)f.data[4] | ((uint32_t)f.data[5] << 8) |
									((uint32_t)f.data[6] << 16) | ((uint32_t)f.data[7] << 24);
				int8_t hs = OtaSlot_ReadHeader(OTA_SLOT_A, &hdr);

				/* 回显校验：下位机要的必须正是槽头这份。防止查询与请求之间槽被重传覆盖，
				   导致下位机拿 A 的 CRC 去校验 B 的内容，白传一遍还判失败。 */
				if((hs != OTA_SLOT_OK) || (hdr.fw_len != want_len) || (hdr.fw_crc != want_crc))
				{
					Uart_Printf(UART_LOG, "[can] UPDATE_REQ 不匹配槽 A (len=%lu crc=0x%08lX)，忽略\r\n",
								(unsigned long)want_len, (unsigned long)want_crc);
					continue;
				}

				s_tx_addr        = OtaSlot_DataAddr(OTA_SLOT_A);
				s_tx_left        = hdr.fw_len;
				s_tx_seq         = 0U;
				s_tx_total_frame = (uint16_t)((hdr.fw_len + OTA_CAN_DATA_BYTES - 1U) / OTA_CAN_DATA_BYTES);
				s_tx_block_cnt   = 0U;
				s_tx_wait_ack    = 0U;
				s_tx_ack_tick    = Board_Millis();
				s_tx_st          = OTA_TX_ST_SENDING;

				Uart_Printf(UART_LOG, "[ota] 开始下发 %luB / %u 帧 (crc=0x%08lX)\r\n",
							(unsigned long)hdr.fw_len, (unsigned int)s_tx_total_frame,
							(unsigned long)hdr.fw_crc);
			}
			else if((f.id == CANDRV_ID_RSP) && (s_tx_st == OTA_TX_ST_SENDING))
			{
				int8_t ar = OtaCan_OnAck(&f);

				if(ar < 0)
				{
					Uart_Printf(UART_LOG, "[ota] 下发中止：ACK 报错\r\n");
					s_tx_st = OTA_TX_ST_IDLE;
				}
				else if(ar > 0)
				{
					/* 收尾 ACK：下位机已 Finish+写 PENDING。会话到此结束。
					   注意 C8T6 只在收齐那一帧回一次 ACK，所以这是最后一帧的确认。 */
					if(s_tx_seq >= s_tx_total_frame)
					{
						Uart_Printf(UART_LOG, "[ota] 下发完成：%luB / %u 帧，下位机校验通过\r\n",
									(unsigned long)(s_tx_total_frame * OTA_CAN_DATA_BYTES),
									(unsigned int)s_tx_total_frame);
						s_tx_st = OTA_TX_ST_IDLE;
					}
					else
					{
						s_tx_wait_ack  = 0U;
						s_tx_block_cnt = 0U;
					}
				}
			}
		}
	}

	/* ---------- 分帧发送推进（非阻塞，每圈最多发 1 帧） ----------
	   为什么每圈只发一帧：主循环要持续取 CAN 队列（收 ACK）和串口数据。
	   若一整块在一个循环里连发，期间收不到 ACK 也取不到新查询，等价于卡死等待。
	   块级流控：发满 OTA_CAN_BLOCK_FRAMES 帧就停下等 ACK，ACK 到了才发下一块。 */
	if(s_tx_st == OTA_TX_ST_SENDING)
	{
		if(s_tx_wait_ack != 0U)
		{
			/* 等 ACK 超时：下位机可能写 Flash 失败或掉线，别永远挂着 */
			if(Timer_Timeout(&s_tx_ack_tick, OTA_CAN_ACK_TIMEOUT_MS) != 0U)
			{
				Uart_Printf(UART_LOG, "[ota] 下发中止：等 ACK 超时 (seq=%u)\r\n",
							(unsigned int)s_tx_seq);
				s_tx_st = OTA_TX_ST_IDLE;
			}
		}
		else if(s_tx_seq < s_tx_total_frame)
		{
			int8_t sr = OtaCan_SendDataFrame(s_tx_seq);

			if(sr != CANDRV_OK)
			{
				Uart_Printf(UART_LOG, "[ota] 下发中止：发帧 FAIL=%d seq=%u\r\n",
							(int)sr, (unsigned int)s_tx_seq);
				s_tx_st = OTA_TX_ST_IDLE;
			}
			else
			{
				s_tx_seq++;
				s_tx_block_cnt++;

				if(s_tx_block_cnt >= OTA_CAN_BLOCK_FRAMES)
				{
					s_tx_wait_ack  = 1U;
					s_tx_ack_tick  = Board_Millis();
				}
				else if(s_tx_seq >= s_tx_total_frame)
				{
					/* 最后未满一块：也必须进等 ACK 态，等下位机 Finish 的收尾确认。
					   若漏掉这一步，收尾 ACK 会被当普通帧丢弃，状态机永远停在 SENDING。 */
					s_tx_wait_ack  = 1U;
					s_tx_ack_tick  = Board_Millis();
				}
				else
				{
					/* 块内帧间让出一圈：下位机主循环要取队列 + W25Q 攒扇区 */
					Delay_Ms(OTA_CAN_SEND_GAP_MS);
				}
			}
		}
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
