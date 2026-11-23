/**
 * OTA 更新标志实现（APP 侧；Recieve 侧有一份完全相同的副本）
 *
 * 【本版改动】结构体加 state 字段（PENDING/APPLIED），self_crc 覆盖前 16 字节；
 *   OtaFlag_Write 多一个 state 参数；新增 OtaFlag_SetState 供 Boot 改状态。
 *   旧版标志（16 字节、无 state）会因为 self_crc 对不上判 BAD —— 需重刷一轮，可接受。
 *
 * 【副作用】写/擦标志各擦一次 4KB 扇区（典型 45ms）。SetState 也要擦写一次，
 *   只在 Boot 搬运成功后发生一次，频率极低。
 */
#include "ota_flag.h"
#include "crc32.h"			/* self_crc 用；两侧同一份实现 */
#include <string.h>

/** 读标志并判断成色：空片 / 有效 / 写坏 */
int8_t OtaFlag_Read(OtaUpdateFlag_t *flag)
{
	uint8_t  raw[sizeof(OtaUpdateFlag_t)];
	uint8_t  blank = 1U;
	uint32_t calc;
	int8_t   r;

	if (flag == NULL)
	{
		return OTA_FLAG_ERR;
	}
	r = W25q_Read(OTA_FLAG_ADDR, raw, sizeof(raw));
	if (r != W25Q_OK)
	{
		return OTA_FLAG_ERR;
	}

	/* 全 0xFF = 从没写过 */
	for (uint32_t i = 0U; i < sizeof(raw); i++)
	{
		if (raw[i] != 0xFFU)
		{
			blank = 0U;
			break;
		}
	}
	if (blank != 0U)
	{
		return OTA_FLAG_EMPTY;
	}

	memcpy(flag, raw, sizeof(raw));
	if (flag->magic != OTA_FLAG_MAGIC)
	{
		return OTA_FLAG_BAD;
	}
	/* self_crc 覆盖前 16 字节（含 state）。写一半 / 电压跌落都会暴露 */
	calc = Crc32_Buf(raw, 16U);
	if (calc != flag->self_crc)
	{
		return OTA_FLAG_BAD;
	}
	/* state 必须是合法值 */
	if ((flag->state != OTA_FLAG_PENDING) && (flag->state != OTA_FLAG_APPLIED))
	{
		return OTA_FLAG_BAD;
	}
	/* 长度合理范围：1 ~ 固件区容量（1MB） */
	if ((flag->length == 0U) || (flag->length > 0x00100000UL))
	{
		return OTA_FLAG_BAD;
	}
	return OTA_FLAG_OK;
}


int8_t OtaFlag_Write(uint32_t length, uint32_t crc32, uint32_t state)
{
	OtaUpdateFlag_t flag;
	int8_t          r;

	flag.magic    = OTA_FLAG_MAGIC;
	flag.length   = length;
	flag.crc32    = crc32;
	flag.state    = state;
	flag.self_crc = Crc32_Buf((const uint8_t *)&flag, 16U);

	/* 先擦后写。擦除期间断电 → 标志变空片（= 没更新），Boot 直接跳旧固件，安全 */
	r = W25q_EraseSector(OTA_FLAG_ADDR);
	if (r != W25Q_OK)
	{
		return OTA_FLAG_ERR;
	}
	r = W25q_Write(OTA_FLAG_ADDR, (const uint8_t *)&flag, sizeof(flag));
	if (r != W25Q_OK)
	{
		return OTA_FLAG_ERR;
	}

	/* 写完立刻回读，确认 self_crc 对得上 —— 不猜、等实测 */
	{
		OtaUpdateFlag_t chk;

		if (OtaFlag_Read(&chk) != OTA_FLAG_OK)
		{
			return OTA_FLAG_ERR;
		}
		if ((chk.length != length) || (chk.crc32 != crc32) || (chk.state != state))
		{
			return OTA_FLAG_ERR;
		}
	}
	return OTA_FLAG_OK;
}


int8_t OtaFlag_SetState(uint32_t state)
{
	OtaUpdateFlag_t flag;
	int8_t          r;

	if ((state != OTA_FLAG_PENDING) && (state != OTA_FLAG_APPLIED))
	{
		return OTA_FLAG_ERR;
	}
	r = OtaFlag_Read(&flag);
	if (r != OTA_FLAG_OK)
	{
		return OTA_FLAG_ERR;
	}
	if (flag.state == state)
	{
		return OTA_FLAG_OK;			/* 已经是目标状态，不用白擦一次扇区 */
	}
	return OtaFlag_Write(flag.length, flag.crc32, state);
}


int8_t OtaFlag_Erase(void)
{
	int8_t r = W25q_EraseSector(OTA_FLAG_ADDR);

	return (r == W25Q_OK) ? OTA_FLAG_OK : OTA_FLAG_ERR;
}
