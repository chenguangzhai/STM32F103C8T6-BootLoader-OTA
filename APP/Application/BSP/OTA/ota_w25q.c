/**
 * OTA 落盘实现（APP 侧，写 W25Q64）
 *
 * 【设计要点】
 *   1) 攒 4KB 扇区：W25Q 擦除单位，攒满才擦+写。擦一次典型 45ms，
 *      若每收 256B 擦一次，同一扇区擦 16 次，慢 16 倍且寿命砍到 1/16。
 *   2) 尾扇区只写实际字节数，回读也只比实际字节 —— 剩下是擦除态 0xFF，
 *      拿整扇区比会假失败。
 *   3) 逐字节回读比对（用户本轮明确要求）：写完一个扇区后，s_sector 里仍是原始数据
 *      （下次 Feed 才覆盖），分块从 W25Q 读回 256B 跟它逐字节比。
 *      只需额外 256B RAM，就能做到全量校验，不依赖 CRC 模块，也不受 C8T6 20KB RAM 限制。
 *   4) 失败锁死：s_err 非 0 后所有操作直接返回错误，直到重新 Init。
 *
 * 【副作用】擦写期间 SPI1 被本模块占用（CS=PB0），与 W25q_Test 共用驱动；
 *   两者不能并发。APP 主循环单线程、自检只在开机跑一次，实际不冲突。
 */
#include "ota_w25q.h"
#include "uart_app.h"		/* 进度/校验日志 */
#include "crc32.h"			/* 算固件 CRC32 写进更新标志 */
#include "ota_flag.h"		/* 校验通过后写更新标志，通知 Boot 搬运 */
#include <string.h>

/* 1 = 打每扇区进度与校验结果；验证阶段开，稳定后可改 0 */
#define OTAW_LOG_EN     1

#if (OTAW_LOG_EN != 0)
#define OTAW_LOG(...)   Uart_Printf(UART_LOG, __VA_ARGS__)
#else
#define OTAW_LOG(...)   do { } while (0)
#endif

/* 4KB 攒页缓冲放静态区（栈只有 1KB，局部大数组会踩底） */
static uint8_t  s_sector[OTA_W25Q_SECTOR];
/* 回读分块缓冲：256B 足够，逐块跟 s_sector 比，不需整包缓存 */
static uint8_t  s_readback[256];

static uint32_t s_addr;			/* 当前扇区要写到的 W25Q 地址 */
static uint32_t s_fill;			/* s_sector 里已攒字节数 */
static uint32_t s_total;		/* 真正落进 W25Q 的字节数 */
static uint32_t s_bad_off;		/* 回读比对失败的第一个偏移（相对固件起点） */
static int8_t   s_err;			/* 失败锁死 */
static uint8_t  s_inited;


/**
  * 回读比对：把 [addr, addr+len) 从 W25Q 读回，跟 expect 逐字节比。
  * 返回 OTAW_OK / OTAW_ERR_READ / OTAW_ERR_VERIFY。
  * 失败时 s_bad_off 记录第一个不一致字节相对固件起点的偏移。
  */
static int8_t VerifyBytes(uint32_t addr, const uint8_t *expect, uint32_t len,
                          uint32_t base_off)
{
	uint32_t done = 0U;
	int8_t   r;

	while (done < len)
	{
		uint32_t chunk = len - done;

		if (chunk > sizeof(s_readback))
		{
			chunk = sizeof(s_readback);
		}
		r = W25q_Read(addr + done, s_readback, chunk);
		if (r != W25Q_OK)
		{
			return OTAW_ERR_READ;
		}
		if (memcmp(s_readback, &expect[done], chunk) != 0)
		{
			for (uint32_t k = 0U; k < chunk; k++)
			{
				if (s_readback[k] != expect[done + k])
				{
					s_bad_off = base_off + done + k;
					break;
				}
			}
			return OTAW_ERR_VERIFY;
		}
		done += chunk;
	}
	return OTAW_OK;
}


/**
  * 擦一扇区 + 写一扇区 + 回读比对（先擦后写是 W25Q 硬件铁律）
  * addr 必须 4KB 对齐，len 不超过一扇区。
  */
static int8_t PushSector(uint32_t addr, const uint8_t *buf, uint32_t len,
                         uint32_t base_off)
{
	int8_t r;

	if ((addr < OTA_W25Q_BASE) || ((addr + OTA_W25Q_SECTOR) > OTA_W25Q_END))
	{
		return OTAW_ERR_FULL;
	}
	r = W25q_EraseSector(addr);
	if (r != W25Q_OK)
	{
		return OTAW_ERR_ERASE;
	}
	r = W25q_Write(addr, buf, len);		/* 内部按 256B 自动切页 */
	if (r != W25Q_OK)
	{
		return OTAW_ERR_WRITE;
	}
	/* 写完立刻回读比对：此刻 buf(s_sector) 还是原始数据，下一包才覆盖 */
	r = VerifyBytes(addr, buf, len, base_off);
	if (r != OTAW_OK)
	{
		return r;
	}
	return OTAW_OK;
}


int8_t OtaW25q_Init(SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin)
{
	if (W25q_Init(hspi, cs_port, cs_pin) != W25Q_OK)
	{
		return OTAW_ERR_ARG;
	}

	s_addr    = OTA_W25Q_BASE;
	s_fill    = 0U;
	s_total   = 0U;
	s_bad_off = 0U;
	s_err     = OTAW_OK;
	s_inited  = 1U;

	OTAW_LOG("[wota] init base=0x%06lX sector=%luB\r\n",
			 (unsigned long)OTA_W25Q_BASE, (unsigned long)OTA_W25Q_SECTOR);
	return OTAW_OK;
}


int8_t OtaW25q_Feed(const uint8_t *buf, uint32_t len)
{
	uint32_t i = 0U;

	if ((s_inited == 0U) || (buf == NULL) || (len == 0U))
	{
		return OTAW_ERR_ARG;
	}
	if (s_err != OTAW_OK)
	{
		return s_err;
	}

	while (i < len)
	{
		uint32_t space = OTA_W25Q_SECTOR - s_fill;
		uint32_t n     = ((len - i) < space) ? (len - i) : space;

		memcpy(&s_sector[s_fill], &buf[i], n);
		s_fill += n;
		i      += n;

		if (s_fill >= OTA_W25Q_SECTOR)		/* 满一扇区，落盘 + 比对 */
		{
			int8_t r = PushSector(s_addr, s_sector, OTA_W25Q_SECTOR, s_total);

			if (r != OTAW_OK)
			{
				s_err = r;
				if (r == OTAW_ERR_VERIFY)
				{
					OTAW_LOG("[wota] VERIFY FAIL sector 0x%06lX off=%lu\r\n",
							 (unsigned long)s_addr, (unsigned long)s_bad_off);
				}
				else
				{
					OTAW_LOG("[wota] FAIL sector 0x%06lX err=%d\r\n",
							 (unsigned long)s_addr, (int)r);
				}
				return r;
			}

			s_total += OTA_W25Q_SECTOR;
			OTAW_LOG("[wota] sector 0x%06lX ok+verified, total=%lu\r\n",
					 (unsigned long)s_addr, (unsigned long)s_total);
			s_addr += OTA_W25Q_SECTOR;
			s_fill  = 0U;
		}
	}
	return OTAW_OK;
}


int8_t OtaW25q_Finish(void)
{
	int8_t r;

	if (s_inited == 0U)
	{
		return OTAW_ERR_ARG;
	}
	if (s_err != OTAW_OK)
	{
		return s_err;
	}

	/* 1) 补写尾扇区（只写实际字节数），同样回读比对 */
	if (s_fill > 0U)
	{
		r = PushSector(s_addr, s_sector, s_fill, s_total);
		if (r != OTAW_OK)
		{
			s_err = r;
			OTAW_LOG("[wota] tail FAIL 0x%06lX %luB err=%d off=%lu\r\n",
					 (unsigned long)s_addr, (unsigned long)s_fill, (int)r,
					 (unsigned long)s_bad_off);
			return r;
		}
		s_total += s_fill;
		OTAW_LOG("[wota] tail 0x%06lX %luB ok+verified, total=%lu\r\n",
				 (unsigned long)s_addr, (unsigned long)s_fill, (unsigned long)s_total);
		s_addr += OTA_W25Q_SECTOR;
		s_fill  = 0U;
	}

	if (s_total == 0U)
	{
		OTAW_LOG("[wota] no data\r\n");
		return OTAW_OK;
	}

	/* 2) 打印头 16 字节，供肉眼跟原 bin 核对（向量表应 0x2000xxxx / 0x0800xxxx） */
	{
		uint8_t head[16];

		r = W25q_Read(OTA_W25Q_BASE, head, sizeof(head));
		if (r == W25Q_OK)
		{
			OTAW_LOG("[wota] head:");
			for (uint32_t k = 0U; k < sizeof(head); k++)
			{
				OTAW_LOG(" %02X", (unsigned)head[k]);
			}
			OTAW_LOG("\r\n");
		}
	}

	OTAW_LOG("[wota] VERIFY OK total=%lu app=0x%06lX\r\n",
			 (unsigned long)s_total, (unsigned long)OTA_W25Q_BASE);

	/* 3) 算固件 CRC32（从 W25Q 读回来算，用 Crc32_Update 分块累计），
	      再写更新标志通知 Boot 来搬运。
	   为什么必须从 W25Q 读回算：边收边算只能证明"收到了什么"，
	   读回算才能证明"Flash 里存的到底是什么"。
	   写标志失败不算固件写入失败（数据已经逐字节校验通过），只打日志，
	   让用户能重发一次触发重写标志，不必重传整个 bin。 */
	{
		uint32_t crc = CRC32_INIT;
		uint32_t done = 0U;

		while (done < s_total)
		{
			uint32_t chunk = s_total - done;

			if (chunk > sizeof(s_readback))
			{
				chunk = sizeof(s_readback);
			}
			r = W25q_Read(OTA_W25Q_BASE + done, s_readback, chunk);
			if (r != W25Q_OK)
			{
				OTAW_LOG("[wota] CRC read FAIL at %lu\r\n", (unsigned long)done);
				return OTAW_ERR_READ;
			}
			crc = Crc32_Update(crc, s_readback, chunk);
			done += chunk;
		}
		crc = ~crc;				/* 标准 CRC32 收尾取反，与 Crc32_Buf 一致 */

		OTAW_LOG("[wota] fw crc32=0x%08lX\r\n", (unsigned long)crc);

		/* state=PENDING：告诉 Boot"有新固件待装，先备份旧固件再搬我" */
		r = OtaFlag_Write(s_total, crc, OTA_FLAG_PENDING);
		if (r != OTA_FLAG_OK)
		{
			OTAW_LOG("[wota] FLAG WRITE FAIL err=%d (bin ok, resend to retry)\r\n", (int)r);
		}
		else
		{
			OTAW_LOG("[wota] UPDATE FLAG SET -> boot will apply on reset\r\n");
		}
	}

	return OTAW_OK;
}


uint32_t OtaW25q_Total(void)     { return s_total;  }
uint32_t OtaW25q_Fill(void)      { return s_fill;   }
uint32_t OtaW25q_BadOffset(void) { return s_bad_off; }
