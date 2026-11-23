/**
 * APP 侧串口收 bin -> W25Q64（两级 OTA 的下载段）
 *
 * 【职责边界】APP 只写片外 W25Q64（0x100000 起），绝不碰片内 Flash。
 *   片内 Flash 由 Boot（Recieve）侧从 W25Q 搬运固件后写入并跳转，两边职责分离。
 *   这样 APP 不会覆盖自己的代码（APP 本身链接在 0x08005000）。
 *
 * 【功能】
 *   1) Boot_Loader_Getlen() 从串口取数据 -> OtaW25q_Feed()（攒 4KB 扇区写片外）；
 *      不等 Uart_FrameReady —— OTA 是裸字节流，IDLE 单发信号会漏。
 *   2) Boot_Loader_Init()（绑定 W25Q64）与 Boot_Loader_Finish()（补尾 + 回读比对）。
 *   3) idle 2 秒自动 Finish：发完 bin 停手就能看到校验结果，不用额外发命令。
 *
 * 【副作用】擦一扇区 45~400ms 期间 SPI1/CS 被占用；W25Q 是外部 SPI，不阻塞 F1 取指，
 *   主循环能继续取串口数据，但 1KB 环缓冲在极端情况下仍可能溢出，建议分包慢发。
 */
#include "Boot_Loader.h"
#include "ota_w25q.h"		/* OtaW25q_* */
#include "board.h"			/* Board_Millis / Timer_Timeout */
#include "spi.h"			/* hspi1：W25Q64 挂 SPI1 */
#include "main.h"			/* W25Q_CS_Pin / W25Q_CS_GPIO_Port */

/* 架构约定：APP 侧只写片外 W25Q64，不碰片内 Flash。
   片内 Flash 由 Boot（Recieve）侧负责写入和跳转，两边职责分离。 */

/* 超过这么久没新数据，就补尾扇区 + 回读校验 */
#define OTA_IDLE_FLUSH_MS   2000U

static uint8_t  s_rx[512];		/* 静态区，不压栈（栈只有 1KB） */
static uint32_t s_last_rx;
static int8_t   s_write_err;	/* 失败锁死，避免每圈重复刷日志 */
static uint8_t  s_finished;		/* 已 Finish 过且成功，避免空闲时反复回读 */


uint8_t Boot_Loader_Init(void)
{
	s_last_rx   = Board_Millis();
	s_write_err = OTAW_OK;
	s_finished  = 0U;

	/* 绑定 W25Q64：SPI1 + PB0 片选；内部设写入起点 0x100000 */
	return OtaW25q_Init(&hspi1, W25Q_CS_GPIO_Port, W25Q_CS_Pin);
}


int8_t Boot_Loader_Finish(void)
{
	int8_t r;

	if (s_finished != 0U)
	{
		return OTAW_OK;			/* 已经校验通过，不重复回读 */
	}
	if (s_write_err != OTAW_OK)
	{
		return s_write_err;
	}

	r = OtaW25q_Finish();
	if (r != OTAW_OK)
	{
		s_write_err = r;
		Uart_Printf(UART_LOG, "[wota] FINISH FAIL err=%d bad_off=%lu\r\n",
					(int)r, (unsigned long)OtaW25q_BadOffset());
	}
	else
	{
		s_finished = 1U;
	}
	return r;
}


uint16_t Boot_Loader_Getlen(void)
{
	uint16_t rx_len;

	/* 不等 FrameReady：裸字节流没有帧边界，IDLE 单发信号会漏。
	   收完 Finish 后不再吃数据，除非重新 Init（防止校验通过后又被新字节破坏） */
	if (s_finished != 0U)
	{
		return 0U;
	}

	rx_len = Uart_Read(UART_LOG, s_rx, sizeof(s_rx));

	if (rx_len > 0U)
	{
		s_last_rx = Board_Millis();

		if (s_write_err == OTAW_OK)
		{
			int8_t r = OtaW25q_Feed(s_rx, rx_len);

			if (r != OTAW_OK)
			{
				s_write_err = r;
				Uart_Printf(UART_LOG, "[wota] WRITE FAIL err=%d total=%lu\r\n",
							(int)r, (unsigned long)OtaW25q_Total());
			}
		}
		return rx_len;
	}

	/* 没新数据 + 扇区里有尾巴 + 超过 2 秒 -> 补尾 + 回读比对 */
	if ((s_write_err == OTAW_OK) && (OtaW25q_Fill() > 0U) &&
		(Timer_Timeout(&s_last_rx, OTA_IDLE_FLUSH_MS) != 0U))
	{
		(void)Boot_Loader_Finish();
	}

	return 0U;
}
