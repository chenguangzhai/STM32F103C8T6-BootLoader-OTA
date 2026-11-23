/**
 * CRC32 校验（IEEE 802.3，位计算法，无 256 项查表）
 *
 * 【为什么用位算法】查表法要 1KB 的 const 表放在 Flash，Boot 区只有 20KB，
 *   加了搬运逻辑后溢出 432 字节。位算法不要表，只多几十行代码，
 *   换来约 1KB Flash —— 对一个上电只跑一次的校验来说，慢一点完全可接受。
 * 【结果】与查表法输出完全相同（同一多项式 0xEDB88320、初值全 1、末取反）。
 *   APP 和 Boot 必须用同一份，否则 APP 写的 CRC Boot 校验不过。
 */
#include "crc32.h"
#include <stddef.h>		/* NULL */

/* 位算法核心：一个字节 8 次移位+异或。多项式反射形式 0xEDB88320 */
static uint32_t Crc32_Byte(uint32_t crc, uint8_t b)
{
	uint8_t i;

	crc ^= b;
	for (i = 0U; i < 8U; i++)
	{
		if ((crc & 1U) != 0U)
		{
			crc = (crc >> 1) ^ 0xEDB88320U;
		}
		else
		{
			crc >>= 1;
		}
	}
	return crc;
}

uint32_t Crc32_Update(uint32_t crc, const uint8_t *buf, uint32_t len)
{
	uint32_t i;

	if (buf == NULL)
	{
		return crc;
	}
	for (i = 0U; i < len; i++)
	{
		crc = Crc32_Byte(crc, buf[i]);
	}
	return crc;
}

uint32_t Crc32_Buf(const uint8_t *buf, uint32_t len)
{
	return ~Crc32_Update(CRC32_INIT, buf, len);
}

uint32_t Crc32_Flash(uint32_t addr, uint32_t len)
{
	uint32_t crc = CRC32_INIT;

	while (len > 0U)
	{
		/* 逐字节读片内 Flash（不按字读，位算法本来就慢，省得区分对齐） */
		crc = Crc32_Byte(crc, *(volatile const uint8_t *)addr);
		addr++;
		len--;
	}
	return ~crc;
}
