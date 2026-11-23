/**
 * OTA 落盘实现（最小可验证版）
 *
 * 【来源】从 参考/Boot_Loader/Application/BSP/OTA/ota_app.c 移植，
 *   攒页/落盘/锁死三段逻辑照抄；相对参考版的差异（均为本轮需求导致）：
 *     1) 新增 OTA_ERR_FULL 与 Ota_PushPage 里的越界判断 —— 参考版靠 main 里
 *        OTA_APP_MAX 手动拦，这里直接收进模块内，少一个调用方义务。
 *     2) 去掉尾部记录/CRC/参数页相关调用，本模块只管写，不管写得对不对。
 *     3) 每页日志保留（Uart_Printf 走 UART_LOG），这是本轮验证写入成功的主要证据。
 * 【副作用】s_err 一旦非 0 会锁死所有写入，只能重新 Ota_Init 解锁；
 *   这是故意的 —— 写坏了还继续擦只会把 Flash 寿命耗光。
 */
#include "ota_app.h"
#include "uart_app.h"		/* 进度日志 */
#include <string.h>

/* 1 = 每落盘一页/一次尾巴就打一行日志；验证阶段必须开，调通后可改 0 */
#define OTA_LOG_EN      1

#if (OTA_LOG_EN != 0)
#define OTA_LOG(...)    Uart_Printf(UART_LOG, __VA_ARGS__)
#else
#define OTA_LOG(...)    do { } while (0)
#endif

/* 攒页缓冲：整整一页，放静态区不进栈（栈只有 1KB） */
static uint8_t  s_page[OTA_PAGE_BYTES];

static uint32_t s_addr;			/* 当前页要写到的页首地址 */
static uint32_t s_fill;			/* s_page 里已攒字节数 */
static uint32_t s_total;		/* 真正落进 Flash 的字节数 */
static uint8_t  s_inited;		/* 没 Ota_Init 不许写 */
static int8_t   s_err;			/* 写失败后锁死，避免反复擦同一页 */


/**
  * 把 buf 的 len 字节从 addr 落盘（先擦后写）
  * addr 必须页首对齐，len 不超过一页。
  * 本版新增越界检查：宁可报 FULL 停下，也不让擦写越出 C8T6 的 64KB。
  */
static int8_t Ota_PushPage(uint32_t addr, const uint8_t *buf, uint32_t len)
{
	if ((addr < APP_ADDR) || ((addr + OTA_PAGE_BYTES) > FLASH_END))
	{
		return OTA_ERR_FULL;
	}
	if (Flash_ErasePage(addr) != FLASH_OK)
	{
		return OTA_ERR_ERASE;
	}
	if (Flash_Write(addr, buf, len) != FLASH_OK)
	{
		return OTA_ERR_WRITE;
	}
	return OTA_OK;
}


void Ota_Init(uint32_t start_addr)
{
	s_addr   = start_addr;
	s_fill   = 0U;
	s_total  = 0U;
	s_inited = 1U;
	s_err    = OTA_OK;
	OTA_LOG("[ota] init: start=0x%08lX page=%luB\r\n",
			(unsigned long)start_addr, (unsigned long)OTA_PAGE_BYTES);
}


int8_t Ota_Feed(const uint8_t *buf, uint32_t len)
{
	uint32_t i = 0U;

	if ((s_inited == 0U) || (buf == NULL) || (len == 0U))
	{
		return OTA_ERR_ARG;
	}
	if (s_err != OTA_OK)			/* 之前写坏过，别再碰 Flash */
	{
		return s_err;
	}

	while (i < len)
	{
		uint32_t space = OTA_PAGE_BYTES - s_fill;					/* 这页还剩几个空位 */
		uint32_t n     = ((len - i) < space) ? (len - i) : space;	/* 本次塞多少 */

		memcpy(&s_page[s_fill], &buf[i], n);
		s_fill += n;
		i      += n;

		if (s_fill >= OTA_PAGE_BYTES)		/* 满一页，落盘 */
		{
			int8_t r = Ota_PushPage(s_addr, s_page, OTA_PAGE_BYTES);

			if (r != OTA_OK)
			{
				/* 不清 s_fill：留着现场方便调试；同时锁死错误码，
				   否则下一包又来擦这一页，1 万次寿命很快耗光 */
				s_err = r;
				OTA_LOG("[ota] FAIL addr=0x%08lX err=%d\r\n",
						(unsigned long)s_addr, (int)r);
				return r;
			}

			s_total += OTA_PAGE_BYTES;
			OTA_LOG("[ota] page 0x%08lX ok, total=%lu\r\n",
					(unsigned long)s_addr, (unsigned long)s_total);

			s_addr  += OTA_PAGE_BYTES;
			s_fill   = 0U;
		}
	}

	return OTA_OK;
}


int8_t Ota_Flush(void)
{
	int8_t r;

	if (s_inited == 0U)
	{
		return OTA_ERR_ARG;
	}
	if (s_err != OTA_OK)
	{
		return s_err;
	}
	if (s_fill == 0U)			/* 没尾巴，什么都不做 */
	{
		return OTA_OK;
	}

	r = Ota_PushPage(s_addr, s_page, s_fill);
	if (r != OTA_OK)
	{
		s_err = r;
		OTA_LOG("[ota] tail FAIL addr=0x%08lX %luB err=%d\r\n",
				(unsigned long)s_addr, (unsigned long)s_fill, (int)r);
		return r;
	}

	s_total += s_fill;
	OTA_LOG("[ota] tail 0x%08lX %luB ok, total=%lu\r\n",
			(unsigned long)s_addr, (unsigned long)s_fill, (unsigned long)s_total);

	/* 尾巴写完地址照样前进一整页：这一页结束了，
	   后面再来数据必须落到下一页，否则覆盖刚写的内容 */
	s_addr  += OTA_PAGE_BYTES;
	s_fill   = 0U;

	return OTA_OK;
}


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
