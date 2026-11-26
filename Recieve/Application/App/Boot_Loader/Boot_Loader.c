/**
 * 串口收 bin -> 片内 Flash + 上电窗口跳转（可运行版）
 *
 * 【相对上一版的改动】
 *   1) 新增 Boot_Loader_AppValid() / Boot_Loader_JumpToApp()，从参考 Boot_Loader 移植；
 *      差异：参考用 SPI2，本工程 W25Q64 挂在 SPI1，跳转前 DeInit 的是 hspi1。
 *   2) 新增 Boot_Loader_BootWindow()：上电 3 秒窗口，收到数据留 OTA，没有就跳 App。
 *   3) 窗口期收到的第一包数据直接喂 Ota_Feed，不丢（参考 main.c 的 HandleRx 同款处理）。
 *
 * 【副作用/注意】
 *   · JumpToApp 会关中断、停 SysTick/DMA/SPI、换栈，调用后本工程的任何状态都不再可靠；
 *     它不返回，除非 App 向量表无效。
 *   · 擦一页 20~40ms 期间 F1 单 bank 停 CPU 取指，但 UART DMA 仍在收；
 *     1KB 环缓冲扛得住 115200 下约 460 字节，主机一口气灌 44KB 仍可能溢出。
 */
#include "Boot_Loader.h"
#include "ota_app.h"		/* Ota_Init / Ota_Feed / Ota_Flush / Ota_Fill */
#include "board.h"			/* Board_Millis / Timer_Timeout */
#include "flash_app.h"		/* APP_ADDR */
#include "usart.h"			/* huart1：跳转前要 AbortReceive */
#include "spi.h"			/* hspi1：跳转前要 DeInit（参考是 hspi2，本板是 SPI1） */
#include "main.h"			/* W25Q_CS_Pin / W25Q_CS_GPIO_Port：绑定 W25Q 用 */
#include "w25q.h"			/* W25q_Init：搬运前必须绑定 SPI/片选 */
#include "ota_apply.h"		/* 检查更新标志 -> 从 W25Q 搬运固件到片内 */

/* 超过这么久没新数据，就把不足一页的尾巴写出去 */
#define OTA_IDLE_FLUSH_MS   2000U

/* 上电后留给上位机的升级窗口。3 秒内收到数据就留在 OTA，没有就跳 App。
 * 为什么必须有：无条件跳的话，App 写坏或跑飞后 bootloader 再也醒不过来，
 * 只能拿 ST-Link 救。调大 -> 开机慢但从容；调小 -> 开机快但要手快。 */
#define OTA_WAIT_MS         3000U

/* App 向量表判据 */
#define APP_SP_BASE         0x20000000U
#define APP_SP_MASK         0xFFFE0000U
#define APP_VEC_BASE        0x08000000U
#define APP_VEC_MASK        0xFF000000U

/* 512 字节放静态区，不压栈：栈只有 1KB，局部大数组容易踩底 */
static uint8_t  s_rx[512];
static uint32_t s_last_rx;		/* 最后一次收到数据的时刻 */
static int8_t   s_write_err;	/* 落盘失败标记，避免每圈重复刷错误日志 */

/* 把一段数据喂给落盘模块，写失败只报一次 */
static void Feed_To_Flash(const uint8_t *buf, uint16_t len)
{
	if (s_write_err != OTA_OK)
	{
		return;
	}
	{
		int8_t r = Ota_Feed(buf, len);

		if (r != OTA_OK)
		{
			s_write_err = r;
			Uart_Printf(UART_LOG, "[ota] WRITE FAIL err=%d total=%lu\r\n",
						(int)r, (unsigned long)Ota_Total());
		}
	}
}


void Boot_Loader_Init(void)
{
	/* 显式绑定 W25Q64（SPI1 + PB0 片选）：搬运不依赖"自检是否跑过"这个隐含前提。 */
	(void)W25q_Init(&hspi1, W25Q_CS_GPIO_Port, W25Q_CS_Pin);

	s_last_rx   = Board_Millis();
	s_write_err = OTA_OK;
	Ota_Init(APP_ADDR);			/* 0x08005000，App 区第一页页首 */
}


uint16_t Boot_Loader_Getlen(void)
{
	uint16_t rx_len;

	/* 不等 Uart_FrameReady：连续字节流没有帧边界，IDLE 单发信号会漏。 */
	rx_len = Uart_Read(UART_LOG, s_rx, sizeof(s_rx));

	if (rx_len > 0U)
	{
		s_last_rx = Board_Millis();
		Feed_To_Flash(s_rx, rx_len);
		return rx_len;
	}

	/* 没新数据 + 页里有尾巴 + 超过 2 秒 -> 补写尾页 */
	if ((s_write_err == OTA_OK) && (Ota_Fill() > 0U) &&
		(Timer_Timeout(&s_last_rx, OTA_IDLE_FLUSH_MS) != 0U))
	{
		int8_t r = Ota_Flush();

		if (r != OTA_OK)
		{
			s_write_err = r;
			Uart_Printf(UART_LOG, "[ota] TAIL FAIL err=%d total=%lu\r\n",
						(int)r, (unsigned long)Ota_Total());
		}
	}

	return 0U;
}


uint8_t Boot_Loader_AppValid(uint32_t app_addr)
{
	uint32_t sp = *(volatile uint32_t *)(app_addr);
	uint32_t rv = *(volatile uint32_t *)(app_addr + 4U);

	if ((sp & APP_SP_MASK) != APP_SP_BASE)		return 0U;	/* SP 不像 RAM 地址 */
	if ((rv & APP_VEC_MASK) != APP_VEC_BASE)	return 0U;	/* 复位向量不在 Flash */
	if ((rv & 0x1U) == 0U)						return 0U;	/* 缺 Thumb 位，跳过去就 HardFault */

	return 1U;
}


/**
  * 跳到 App 运行（正常不返回）
  *
  * 为什么跳之前要清理这么多 —— CPU 只是把 PC 指过去，Boot 留下的东西全都还在：
  *   · SysTick 还在数数，跳过去 ms 级就进 Boot 的中断
  *   · USART1 / DMA1_Channel5 中断使能还开着
  *   · DMA 脱离 CPU 独立搬运，光关中断拦不住它继续写内存
  *   · VTOR 还指着 0x08000000，异常会跑进 Boot 的向量表
  *   · MSP 还是 Boot 的栈
  * 少清一项，症状都是"跳过去能跑几秒，然后莫名其妙死"。
  */
void Boot_Loader_JumpToApp(uint32_t app_addr)
{
	uint32_t sp;
	uint32_t rv;
	void (*entry)(void);

	if (Boot_Loader_AppValid(app_addr) == 0U)
	{
		return;					/* 地址无效，什么都不动 */
	}

	sp    = *(volatile uint32_t *)(app_addr);
	rv    = *(volatile uint32_t *)(app_addr + 4U);
	entry = (void (*)(void))rv;

	__disable_irq();			/* 1) 关总中断 */

	/* 2) 停 SysTick。不清它，App 里第一次 HAL_Delay 会立刻超时 */
	SysTick->CTRL = 0U;
	SysTick->LOAD = 0U;
	SysTick->VAL  = 0U;

	/* 3) 关所有 NVIC 使能并清挂起。只关 SysTick 不够，USART1/DMA 还挂着 */
	NVIC->ICER[0] = 0xFFFFFFFFU;
	NVIC->ICPR[0] = 0xFFFFFFFFU;

	/* 4) 停串口 DMA 接收，否则它会继续往 s_rx 所在环形缓冲灌数据 */
	HAL_UART_AbortReceive(&huart1);

	/* 4b) 停 SPI1。本板 W25Q64 挂 SPI1（参考工程是 SPI2）。
	      不 DeInit 就跳，App 里 MX_SPI1_Init() 是在一个活着的 SPI 上改配置，
	      最坏第一次读 W25Q 的 ID 就错，看起来像"驱动坏了"。 */
	HAL_SPI_DeInit(&hspi1);

	/* 5) 提前把 VTOR 指过去。App 的 SystemInit 里 USER_VECT_TAB_ADDRESS 是注释掉的，
	      不会再设 VTOR —— 全靠这一句，否则 App 的中断全跑进 Boot 向量表 */
	SCB->VTOR = app_addr;

	/* 6) 换栈顶。这一步之后不能再调用任何函数（Boot 的栈帧已失效） */
	__set_MSP(sp);

	/* 7) 开中断再跳。不开的话 App 的 SysTick 一直挂着，HAL_Delay 死等 */
	__enable_irq();

	entry();

	while (1)					/* 理论上到不了；真到了说明 App 一进去就异常返回 */
	{
	}
}


uint8_t Boot_Loader_BootWindow(void)
{
	uint32_t t_boot = Board_Millis();
	uint16_t len;
	int8_t   apply_st;

	/* ============ 第一件事：检查更新标志，有更新才搬运 ============
	   这解决"Boot 每次上电都从 W25Q 搬 16KB"的浪费：
	   APP 校验通过后才写标志；Boot 读到标志才搬，搬完擦标志；
	   没标志就跳过，直接走下面的窗口/跳转，不碰 W25Q 也不碰片内 Flash。
	   搬运失败时标志保留，下次上电重试（见 ota_apply.c）。 */
	apply_st = OtaApply_CheckAndApply(APP_ADDR);
	if (apply_st == OTA_APPLY_ERR)
	{
		/* 搬运失败：片内 App 可能是旧的或写坏的。
		   不在这里死等 —— 继续走下面的窗口，让 AppValid 决定能不能跳；
		   若连向量表都坏了，下面会停在 OTA 等串口重传。 */
		Uart_Printf(UART_LOG, "[boot] apply failed, will retry next reset\r\n");
	}

	/* App 无效（空片/向量表错/写了一半）就不用等，直接停在这里等串口下载 */
	if (Boot_Loader_AppValid(APP_ADDR) == 0U)
	{
		Uart_Printf(UART_LOG, "[boot] no valid app, waiting for bin\r\n");
		return 1U;
	}

	Uart_Printf(UART_LOG, "[boot] window %lums, send bin to stay in OTA\r\n",
				(unsigned long)OTA_WAIT_MS);

	while (Timer_Timeout(&t_boot, OTA_WAIT_MS) == 0U)
	{
		len = Uart_Read(UART_LOG, s_rx, sizeof(s_rx));
		if (len > 0U)
		{
			s_last_rx = Board_Millis();
			Uart_Printf(UART_LOG, "[boot] OTA entered\r\n");

			/* 这一包别丢：万一是尾部记录或固件开头，喂进去继续处理 */
			Feed_To_Flash(s_rx, len);
			return 1U;
		}
	}

	Uart_Printf(UART_LOG, "[boot] jump to 0x%08lX\r\n", (unsigned long)APP_ADDR);
	Boot_Loader_JumpToApp(APP_ADDR);
	Uart_Printf(UART_LOG, "[boot] jump failed (invalid vector)\r\n");

	/* 跳转失败（理论不会，前面已 AppValid）：留在 OTA 等重传 */
	return 1U;
}
