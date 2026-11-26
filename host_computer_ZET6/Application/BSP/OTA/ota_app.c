/**
 * OTA 落盘实现 —— 设计说明见 ota_app.h
 *
 * 2026-11-25：底层从片内 Flash 换成外部 W25Q（SPI1/PA4~PA7）。
 * 这里仍然只做「攒」和「落盘」，不做长度/CRC 校验——那是 main 的收尾逻辑。
 */
#include "ota_app.h"
#include "uart_app.h"		/* 进度日志。不想让这层打印就把 OTA_LOG_EN 改成 0 */
#include "w25q.h"			/* 外部 Flash：擦 4KB 扇区 + 按 256B 页写入 */
#include <string.h>

/* 1 = 每落盘一扇区/一次尾巴就打一行日志，0 = 全静音 */
#define OTA_LOG_EN      1

#if (OTA_LOG_EN != 0)
#define OTA_LOG(...)    Uart_Printf(UART_LOG, __VA_ARGS__)
#else
#define OTA_LOG(...)    do { } while (0)
#endif


/* 攒扇区缓冲：整整 4KB。放静态区，不进栈。 */
static uint8_t  s_page[OTA_PAGE_BYTES];

static uint32_t s_addr;			/* 当前这一扇区要写到哪个扇区首地址 */
static uint32_t s_fill;			/* s_page 里已经攒了多少字节 */
static uint32_t s_total;		/* 已经真正落进外部 Flash 的字节数 */
static uint8_t  s_inited;		/* 没调过 Ota_Init 就不许写 */
static int8_t   s_err;			/* 一旦某扇区写失败就锁死，避免反复擦同一扇区把寿命耗光 */


/**
  * 函    数：把 buf 的 len 个字节写到外部 Flash 的 addr 处（先擦 4KB 扇区再写）
  * 说    明：addr 必须是扇区首地址，len 不能超过一扇区。
  *           W25q_Write() 内部会按 256B 页边界自动切分，无需在本层处理。
  * 返 回 值：OTA_OK / OTA_ERR_ERASE / OTA_ERR_WRITE
  */
static int8_t Ota_PushPage(uint32_t addr, const uint8_t *buf, uint32_t len)
{
	if(W25q_EraseSector(addr) != W25Q_OK)
	{
		return OTA_ERR_ERASE;
	}
	if(W25q_Write(addr, buf, len) != W25Q_OK)
	{
		return OTA_ERR_WRITE;
	}
	return OTA_OK;
}


/**
  * 函    数：设定写入起点并清空内部状态
  * 参    数：start_addr 第一个扇区首地址，必须 4096 对齐（槽 A 数据区 = 0x001000）
  */
void Ota_Init(uint32_t start_addr)
{
	s_addr   = start_addr;
	s_fill   = 0U;
	s_total  = 0U;
	s_inited = 1U;
	s_err    = OTA_OK;
}


/**
  * 函    数：喂数据，攒满一扇区就落盘
  * 参    数：buf 数据首地址；len 字节数（可以是任意值，不用自己对齐）
  * 返 回 值：OTA_OK 全部处理完；负数表示某一扇区失败了
  * 说    明：失败时已写的扇区不回滚。调用方看到负数应该停手，
  *           否则后面的数据会落在错误的位置上。
  */
int8_t Ota_Feed(const uint8_t *buf, uint32_t len)
{
	uint32_t i = 0U;

	if((s_inited == 0U) || (buf == NULL) || (len == 0U))
	{
		return OTA_ERR_ARG;
	}
	if(s_err != OTA_OK)			/* 之前已经写坏过，别再碰 Flash */
	{
		return s_err;
	}

	while(i < len)
	{
		uint32_t space = OTA_PAGE_BYTES - s_fill;					/* 这一扇区还剩几个空位 */
		uint32_t n     = ((len - i) < space) ? (len - i) : space;	/* 这次实际能塞多少 */

		memcpy(&s_page[s_fill], &buf[i], n);
		s_fill += n;
		i      += n;

		if(s_fill >= OTA_PAGE_BYTES)		/* 满一扇区，落盘 */
		{
			int8_t r = Ota_PushPage(s_addr, s_page, OTA_PAGE_BYTES);

			if(r != OTA_OK)
			{
				/* 不清 s_fill：把攒着的数据留着，方便调试时看现场。
				   同时把错误锁死 —— 否则下一包数据又会来擦这一扇区，
				   失败一次擦一次，10 万次寿命很快就耗光了。 */
				s_err = r;
				OTA_LOG("[ota] 写入失败 地址=0x%08lX 错误码=%d\r\n",
						(unsigned long)s_addr, (int)r);
				return r;
			}

			s_total += OTA_PAGE_BYTES;
			OTA_LOG("[ota] 扇区 0x%08lX 写入成功，累计 %lu\r\n",
					(unsigned long)s_addr, (unsigned long)s_total);

			s_addr  += OTA_PAGE_BYTES;
			s_fill   = 0U;
		}
	}

	return OTA_OK;
}


/**
  * 函    数：把不满一扇区的尾巴写出去
  * 返 回 值：OTA_OK / 负数
  * 说    明：只写 s_fill 个字节，这一扇区剩下部分保持擦除态 0xFF。
  *           写完地址照样前进一整扇区 —— 因为「这一扇区已经结束了」，
  *           后面再来数据必须落到下一扇区，否则会覆盖刚写的内容。
  */
int8_t Ota_Flush(void)
{
	int8_t r;

	if(s_inited == 0U)
	{
		return OTA_ERR_ARG;
	}
	if(s_err != OTA_OK)			/* 已经写坏过，别再碰 Flash */
	{
		return s_err;
	}
	if(s_fill == 0U)			/* 没尾巴，什么都不用做 */
	{
		return OTA_OK;
	}

	r = Ota_PushPage(s_addr, s_page, s_fill);
	if(r != OTA_OK)
	{
		s_err = r;
		OTA_LOG("[ota] 尾部写入失败 地址=0x%08lX %luB 错误码=%d\r\n",
				(unsigned long)s_addr, (unsigned long)s_fill, (int)r);
		return r;
	}

	s_total += s_fill;
	OTA_LOG("[ota] 尾部 0x%08lX %luB 写入成功，累计 %lu\r\n",
			(unsigned long)s_addr, (unsigned long)s_fill, (unsigned long)s_total);

	s_addr  += OTA_PAGE_BYTES;
	s_fill   = 0U;

	return OTA_OK;
}


/* ==================== 状态查询 ==================== */

uint32_t Ota_Total(void)
{
	return s_total;
}

uint32_t Ota_Fill(void)
{
	return s_fill;
}

uint32_t Ota_Addr(void)
{
	return s_addr;
}
