/**
 * OTA 备份区实现（APP 侧；Recieve 侧有一份完全相同的副本）
 *
 * 【写顺序·关键】擦数据区 → 写数据 → 读回算 CRC → CRC 对才写头。
 *   头存在 == 备份完整可用，和 ota_flag 的"标志最后写"是同一个洞见。
 *   中途断电时头还没写，备份被视为 EMPTY，Boot 不会拿半截固件去恢复。
 *
 * 【副作用】备份一次要擦约 16KB 的 W25Q 数据区（4 个扇区）+ 1 个头扇区，
 *   每个扇区擦 45ms，总计约 225ms，只在升级前发生一次，可接受。
 */
#include "ota_backup.h"
#include "crc32.h"
#include <string.h>

/* 数据区分块读回校验用的缓冲（256B，W25Q 页大小） */
static uint8_t s_verify_buf[256];


int8_t OtaBackup_SaveFromFlash(uint32_t flash_addr, uint32_t length)
{
	OtaBackupHeader_t hdr;
	uint8_t           buf[256];
	uint32_t          off;
	uint32_t          n;
	uint32_t          crc = CRC32_INIT;
	int8_t            r;

	if ((length == 0U) || (length > OTA_BACKUP_DATA_MAX))
	{
		return OTA_BAK_ERR;
	}

	/* 1) 擦数据区：length 跨几个 4KB 扇区就擦几个（多擦一个残留扇区保险） */
	{
		uint32_t n_sectors = (length + W25Q_SECTOR_SIZE - 1U) / W25Q_SECTOR_SIZE;

		for (uint32_t s = 0U; s < n_sectors; s++)
		{
			r = W25q_EraseSector(OTA_BACKUP_DATA_ADDR + s * W25Q_SECTOR_SIZE);
			if (r != W25Q_OK)
			{
				return OTA_BAK_ERR;
			}
		}
	}

	/* 2) 逐块：从片内读 → 写 W25Q → 同步累 CRC（用原始片内数据算，少一次读） */
	off = 0U;
	while (off < length)
	{
		n = length - off;
		if (n > sizeof(buf))
		{
			n = sizeof(buf);
		}
		/* 片内 Flash 可直接指针读（地址空间映射） */
		memcpy(buf, (const void *)(flash_addr + off), n);
		r = W25q_Write(OTA_BACKUP_DATA_ADDR + off, buf, n);
		if (r != W25Q_OK)
		{
			return OTA_BAK_ERR;
		}
		crc = Crc32_Update(crc, buf, n);
		off += n;
	}
	crc = ~crc;

	/* 3) 从 W25Q 读回算 CRC，确认写进去的就是刚算的（实锤，不信自己写的） */
	{
		uint32_t back = CRC32_INIT;

		off = 0U;
		while (off < length)
		{
			n = length - off;
			if (n > sizeof(s_verify_buf))
			{
				n = sizeof(s_verify_buf);
			}
			r = W25q_Read(OTA_BACKUP_DATA_ADDR + off, s_verify_buf, n);
			if (r != W25Q_OK)
			{
				return OTA_BAK_ERR;
			}
			back = Crc32_Update(back, s_verify_buf, n);
			off += n;
		}
		back = ~back;
		if (back != crc)
		{
			return OTA_BAK_BAD;		/* 数据写坏了，不写头 */
		}
	}

	/* 4) 全过才写头：头存在 = 备份完整 */
	hdr.magic    = OTA_BACKUP_MAGIC;
	hdr.length   = length;
	hdr.crc32    = crc;
	hdr.self_crc = Crc32_Buf((const uint8_t *)&hdr, 12U);

	r = W25q_EraseSector(OTA_BACKUP_HDR_ADDR);
	if (r != W25Q_OK)
	{
		return OTA_BAK_ERR;
	}
	r = W25q_Write(OTA_BACKUP_HDR_ADDR, (const uint8_t *)&hdr, sizeof(hdr));
	if (r != W25Q_OK)
	{
		return OTA_BAK_ERR;
	}
	return OTA_BAK_OK;
}


int8_t OtaBackup_ReadHeader(OtaBackupHeader_t *hdr)
{
	uint8_t  raw[sizeof(OtaBackupHeader_t)];
	uint8_t  blank = 1U;
	uint32_t calc;
	int8_t   r;

	if (hdr == NULL)
	{
		return OTA_BAK_ERR;
	}
	r = W25q_Read(OTA_BACKUP_HDR_ADDR, raw, sizeof(raw));
	if (r != W25Q_OK)
	{
		return OTA_BAK_ERR;
	}
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
		return OTA_BAK_EMPTY;
	}

	memcpy(hdr, raw, sizeof(raw));
	if (hdr->magic != OTA_BACKUP_MAGIC)
	{
		return OTA_BAK_BAD;
	}
	calc = Crc32_Buf(raw, 12U);
	if (calc != hdr->self_crc)
	{
		return OTA_BAK_BAD;
	}
	if ((hdr->length == 0U) || (hdr->length > OTA_BACKUP_DATA_MAX))
	{
		return OTA_BAK_BAD;
	}
	return OTA_BAK_OK;
}


int8_t OtaBackup_VerifyData(const OtaBackupHeader_t *hdr)
{
	uint32_t crc = CRC32_INIT;
	uint32_t off = 0U;

	if (hdr == NULL)
	{
		return OTA_BAK_ERR;
	}
	while (off < hdr->length)
	{
		uint32_t n = hdr->length - off;

		if (n > sizeof(s_verify_buf))
		{
			n = sizeof(s_verify_buf);
		}
		if (W25q_Read(OTA_BACKUP_DATA_ADDR + off, s_verify_buf, n) != W25Q_OK)
		{
			return OTA_BAK_ERR;
		}
		crc = Crc32_Update(crc, s_verify_buf, n);
		off += n;
	}
	crc = ~crc;
	return (crc == hdr->crc32) ? 1 : 0;
}


int8_t OtaBackup_ReadData(uint32_t offset, uint8_t *buf, uint32_t len)
{
	if ((buf == NULL) || ((offset + len) > OTA_BACKUP_DATA_MAX))
	{
		return OTA_BAK_ERR;
	}
	return (W25q_Read(OTA_BACKUP_DATA_ADDR + offset, buf, len) == W25Q_OK)
	       ? OTA_BAK_OK : OTA_BAK_ERR;
}
