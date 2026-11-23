/**
  * W25Q64 上电自检实现
  *
  * 逐步校验、每步都打串口：失败时日志直接指出是哪一步，不用猜是初始化、
  * 接线、擦除还是数据比对的问题。
  *
  * 用「递增图案 + 跨页起点」而不是全 0xAA：
  *   1) 全 0xAA 即使地址线错位也可能读回一样，测不出地址错；
  *   2) 起点故意设在页尾前 100 字节、长度 300，强制跨过 256B 页边界，
  *      顺便验证 W25q_Write() 的自动切页没有回卷覆盖页首。
  */
#include <stddef.h>
#include "w25q_test.h"
#include "w25q.h"
#include "uart_app.h"       /* Uart_Printf / UART_LOG：日志走现有串口封装 */
#include "spi.h"            /* hspi1：本工程 W25Q 挂 SPI1 */
#include "main.h"           /* W25Q_CS_Pin / W25Q_CS_GPIO_Port */

/* 自检扇区：末扇区首地址。8 MB - 4 KB = 0x7FF000，避开前部固件区 */
#define TEST_SECTOR_ADDR    (W25Q_SIZE - W25Q_SECTOR_SIZE)
/* 写入起点落在页尾前 100 字节，长度 300 -> 必然跨一页，专门压测切页逻辑 */
#define TEST_ADDR           (TEST_SECTOR_ADDR + 100UL)
#define TEST_LEN            300U

/* MISO 引脚：SPI1_MISO = PA6。
   【修正】spi.c:86 把 PA6 配成 GPIO_NOPULL（浮空输入），浮空引脚电平本来就是飘的，
   读到 0 或 1 都不能当作「短路/接线错误」的结论——上次的诊断注释是错的。
   这里保留回读只是为了留档，不再用它下判断。 */
#define MISO_GPIO_PORT      GPIOA
#define MISO_PIN            GPIO_PIN_6

/*==== GPIO 模拟 SPI 诊断（bit-bang）====
   用途：HAL SPI 读不到 ID 时，用纯 GPIO 翻转再读一次，彻底区分两类问题：
         bit-bang 读到 EF 40 17 → HAL SPI 配置/时序有 bug，继续查软件；
         bit-bang 也读不到     → 硬件接线/供电问题，软件无解。
   引脚与 SPI1 默认映射一致：PA5=SCK, PA6=MISO, PA7=MOSI, PB0=CS。 */
#define BB_SCK_PORT         GPIOA
#define BB_SCK_PIN          GPIO_PIN_5
#define BB_MISO_PORT        GPIOA
#define BB_MISO_PIN         GPIO_PIN_6
#define BB_MOSI_PORT        GPIOA
#define BB_MOSI_PIN         GPIO_PIN_7

/**
  * 函    数：粗略延时，给 GPIO 翻转留建立/保持时间
  * 注    意：Cortex-M3 @72MHz 一条指令约 14ns；HAL_GPIO_WritePin 本身十几个指令周期
  *           已远超 W25Q 的 t_SU/t_HD(5ns)。这里再叠几个 NOP 纯粹是保守起见。
  */
static void BB_Delay(void)
{
	volatile uint8_t n = 6U;

	while(n-- > 0U)
	{
		__NOP();
	}
}

/**
  * 函    数：用 GPIO 模拟 SPI Mode 0 读 JEDEC ID（绕开 HAL SPI 外设）
  * 返 回 值：正常 0xEF4017；失败 0xFFFFFF 或 0x000000
  * 注    意：会临时把 PA5/6/7 重配成普通 GPIO，结束后调用 MX_SPI1_Init() 恢复。
  */
static uint32_t BB_ReadId(void)
{
	uint8_t tx[4] = { 0x9FU, 0xFFU, 0xFFU, 0xFFU };
	uint8_t rx[4] = { 0U, 0U, 0U, 0U };
	uint8_t i, b;
	GPIO_InitTypeDef g;
	uint32_t id;

	/* SCK/MOSI：推挽输出，空闲低（Mode 0 的 CPOL=0） */
	HAL_GPIO_WritePin(BB_SCK_PORT, BB_SCK_PIN, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(BB_MOSI_PORT, BB_MOSI_PIN, GPIO_PIN_RESET);
	g.Pin   = BB_SCK_PIN | BB_MOSI_PIN;
	g.Mode  = GPIO_MODE_OUTPUT_PP;
	g.Pull  = GPIO_NOPULL;
	g.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(BB_SCK_PORT, &g);

	/* MISO：输入 + 内部上拉。关键！浮空时读数无意义，上拉后空闲稳定为高。
	   若 Flash 正常驱动 DO，读数由上拉转为 Flash 输出，不影响结果。 */
	g.Pin  = BB_MISO_PIN;
	g.Mode = GPIO_MODE_INPUT;
	g.Pull = GPIO_PULLUP;
	HAL_GPIO_Init(BB_MISO_PORT, &g);

	/* CS 拉低选中芯片 */
	HAL_GPIO_WritePin(W25Q_CS_GPIO_Port, W25Q_CS_Pin, GPIO_PIN_RESET);
	BB_Delay();

	for(i = 0U; i < 4U; i++)
	{
		for(b = 0U; b < 8U; b++)
		{
			/* SCK 低期间改 MOSI（Mode 0：Master 在下降沿改数据） */
			HAL_GPIO_WritePin(BB_MOSI_PORT, BB_MOSI_PIN,
			                  (tx[i] & (0x80U >> b)) ? GPIO_PIN_SET : GPIO_PIN_RESET);
			BB_Delay();
			/* SCK 上升沿：Flash 采样 MOSI，MCU 采样 MISO */
			HAL_GPIO_WritePin(BB_SCK_PORT, BB_SCK_PIN, GPIO_PIN_SET);
			BB_Delay();
			if(HAL_GPIO_ReadPin(BB_MISO_PORT, BB_MISO_PIN) != GPIO_PIN_RESET)
			{
				rx[i] |= (uint8_t)(1U << (7U - b));
			}
			/* SCK 下降沿：Flash 准备下一位 */
			HAL_GPIO_WritePin(BB_SCK_PORT, BB_SCK_PIN, GPIO_PIN_RESET);
			BB_Delay();
		}
	}

	/* CS 拉高结束命令 */
	HAL_GPIO_WritePin(W25Q_CS_GPIO_Port, W25Q_CS_Pin, GPIO_PIN_SET);
	BB_Delay();

	/* 恢复 SPI1 引脚为复用功能，避免影响后续通信 */
	MX_SPI1_Init();

	/* rx[0] 是发命令期间的空转，rx[1..3] 才是 ID */
	id = ((uint32_t)rx[1] << 16) | ((uint32_t)rx[2] << 8) | (uint32_t)rx[3];
	return id;
}

/**
  * 函    数：生成写入用的递增图案
  * 参    数：buf 目标缓冲，len 长度
  * 返 回 值：无
  * 注    意：图案值与写入地址的低字节绑定，地址线错位时读回内容对不上。
  */
static void MakePattern(uint8_t *buf, uint16_t len)
{
	uint16_t i;

	for(i = 0U; i < len; i++)
	{
		buf[i] = (uint8_t)(TEST_ADDR + i);
	}
}

/**
  * 函    数：把一段缓冲按 HEX 打印前若干字节
  * 参    数：tag 行首标记，buf 数据，len 长度
  * 返 回 值：无
  * 注    意：只打前 8 字节，避免一帧日志超过 Uart_Printf 的 100 字节上限。
  */
static void PrintHead(const char *tag, const uint8_t *buf, uint16_t len)
{
	uint16_t n = (len > 8U) ? 8U : len;
	uint16_t i;

	Uart_Printf(UART_LOG, "%s ", tag);
	for(i = 0U; i < n; i++)
	{
		Uart_Printf(UART_LOG, "%02X ", buf[i]);
	}
	Uart_Printf(UART_LOG, "\r\n");
}

uint8_t W25q_Test(void)
{
	static uint8_t tx[TEST_LEN];
	static uint8_t rx[TEST_LEN];
	uint32_t id;
	uint16_t i;
	uint16_t bad = 0U;

	Uart_Printf(UART_LOG, "\r\n[W25Q] test start\r\n");

	/* 步骤 1：绑定 SPI 句柄与片选脚 */
	if(W25q_Init(&hspi1, W25Q_CS_GPIO_Port, W25Q_CS_Pin) != W25Q_OK)
	{
		Uart_Printf(UART_LOG, "[W25Q] FAIL step1: init\r\n");
		return 1U;
	}
	Uart_Printf(UART_LOG, "[W25Q] step1 init ok\r\n");

	/* 步骤 2：读 JEDEC ID。0xFFFFFF/0x000000 基本就是没接上或片选没生效 */
	id = W25q_ReadId();
	Uart_Printf(UART_LOG, "[W25Q] ID=0x%06lX\r\n", id);
	if(id != W25Q_JEDEC_ID)
	{
		uint8_t  raw[3] = {0U, 0U, 0U};
		uint8_t  st, sr;
		unsigned cs_lvl, miso_lvl;
		uint32_t bb;

		/*【诊断说明·修正】上次把 miso_idle=0 解读为「MISO 被拉到地」是错的：
		   spi.c 把 PA6 配成 GPIO_NOPULL，浮空引脚读 0/1 都不代表电气状态。
		   真正能下结论的只有下面这个 bit-bang 测试——它给 MISO 加了内部上拉，
		   并且完全绕开 SPI 外设。 */
		st      = W25q_ReadIdRaw(raw);
		sr      = W25q_ReadStatus();
		cs_lvl  = (unsigned int)HAL_GPIO_ReadPin(W25Q_CS_GPIO_Port, W25Q_CS_Pin);
		miso_lvl= (unsigned int)HAL_GPIO_ReadPin(MISO_GPIO_PORT, MISO_PIN);

		Uart_Printf(UART_LOG, "[W25Q] HAL raw=%02X%02X%02X st=%d\r\n",
		            raw[0], raw[1], raw[2], (int)st);
		Uart_Printf(UART_LOG, "[W25Q] sr=0x%02X cs=%u miso=%u\r\n",
		            (unsigned int)sr, cs_lvl, miso_lvl);

		/* 决定性诊断：纯 GPIO 模拟 SPI 再读一次 ID */
		bb = BB_ReadId();
		Uart_Printf(UART_LOG, "[W25Q] BB id=0x%06lX\r\n", bb);
		if(bb == W25Q_JEDEC_ID)
		{
			Uart_Printf(UART_LOG, "[W25Q] => HAL SPI CONFIG ISSUE\r\n");
		}
		else
		{
			Uart_Printf(UART_LOG, "[W25Q] => HARDWARE: wiring/power\r\n");
		}
		Uart_Printf(UART_LOG, "[W25Q] FAIL step2: ID expect 0x%06lX\r\n", W25Q_JEDEC_ID);
		return 2U;
	}

	/* 步骤 3：擦除末扇区。擦完应为全 0xFF */
	if(W25q_EraseSector(TEST_SECTOR_ADDR) != W25Q_OK)
	{
		Uart_Printf(UART_LOG, "[W25Q] FAIL step3: erase\r\n");
		return 3U;
	}
	Uart_Printf(UART_LOG, "[W25Q] step3 erase ok\r\n");

	/* 步骤 4：写入跨页图案（起点在页尾前 100 字节，长度 300） */
	MakePattern(tx, TEST_LEN);
	if(W25q_Write(TEST_ADDR, tx, TEST_LEN) != W25Q_OK)
	{
		Uart_Printf(UART_LOG, "[W25Q] FAIL step4: write\r\n");
		return 4U;
	}
	Uart_Printf(UART_LOG, "[W25Q] step4 write %uB ok\r\n", (unsigned int)TEST_LEN);

	/* 步骤 5：读回并逐字节比对，统计不一致个数 */
	if(W25q_Read(TEST_ADDR, rx, TEST_LEN) != W25Q_OK)
	{
		Uart_Printf(UART_LOG, "[W25Q] FAIL step5: read\r\n");
		return 5U;
	}
	for(i = 0U; i < TEST_LEN; i++)
	{
		if(rx[i] != tx[i])
		{
			bad++;
		}
	}
	PrintHead("tx:", tx, TEST_LEN);
	PrintHead("rx:", rx, TEST_LEN);

	if(bad != 0U)
	{
		Uart_Printf(UART_LOG, "[W25Q] FAIL step5: %u bytes mismatch\r\n", (unsigned int)bad);
		return 6U;
	}

	Uart_Printf(UART_LOG, "[W25Q] PASS\r\n");
	return 0U;
}
