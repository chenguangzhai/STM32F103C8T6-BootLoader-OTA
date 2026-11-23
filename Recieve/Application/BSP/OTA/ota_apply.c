/**
 * OTA 搬运实现（Boot 侧）
 *
 * 【状态机】读更新标志的 state 分流：
 *   PENDING：W25Q 0x100000 有新固件 → 搬到片内 → 标志改 APPLIED（等 App 确认）
 *   APPLIED：上次装了但 App 没确认 → 从备份区 0x200000 恢复旧固件 → 擦标志
 *
 * 【关键顺序】
 *   PENDING：搬完校验过才 SetState(APPLIED)；校验不过标志保持 PENDING，下次重试。
 *   APPLIED：先校验备份头+数据 CRC，全过才恢复；备份坏了就不回滚（停 Boot 等重传），
 *            绝不拿半截备份去覆盖片内。
 *
 * 【副作用】擦写片内 App 区期间 F1 单 bank 停 CPU 取指（20~40ms/页），Boot 阶段正常。
 */
#include "ota_apply.h"
#include "ota_flag.h"		/* 标志读写（与 APP 侧同一份定义） */
#include "ota_backup.h"		/* 备份区校验/读取 */
#include "flash_app.h"		/* 片内擦写 */
#include "crc32.h"			/* Crc32_Flash */
#include "uart_app.h"
#include "w25q.h"

#define OTA_FW_BASE         0x00100000UL    /* 新固件区，与 APP 侧一致 */
#define APPLY_CHUNK         FLASH_PAGE_BYTES

/**
  * 从 W25Q 源地址搬 length 字节到片内 app_addr，并校验 CRC。
  * src 是 0x100000（新固件）或 0x201000（备份数据）。
  * 返回 OTA_APPLY_OK / OTA_APPLY_ERR。
  */
static int8_t CopyAndVerify(uint32_t src, uint32_t app_addr,
                            uint32_t length, uint32_t expect_crc)
{
	uint8_t  buf[APPLY_CHUNK];
	uint32_t off = 0U;
	uint32_t n;

	while (off < length)
	{
		n = length - off;
		if (n > APPLY_CHUNK)
		{
			n = APPLY_CHUNK;
		}
		if (W25q_Read(src + off, buf, n) != W25Q_OK)
		{
			Uart_Printf(UART_LOG, "[apply] W25Q read FAIL at %lu\r\n", (unsigned long)off);
			return OTA_APPLY_ERR;
		}
		if (Flash_ErasePage(app_addr + off) != FLASH_OK)
		{
			Uart_Printf(UART_LOG, "[apply] erase FAIL at 0x%08lX\r\n",
						(unsigned long)(app_addr + off));
			return OTA_APPLY_ERR;
		}
		if (Flash_Write(app_addr + off, buf, n) != FLASH_OK)
		{
			Uart_Printf(UART_LOG, "[apply] write FAIL at 0x%08lX\r\n",
						(unsigned long)(app_addr + off));
			return OTA_APPLY_ERR;
		}
		off += n;
	}

	{
		uint32_t crc = Crc32_Flash(app_addr, length);

		Uart_Printf(UART_LOG, "[apply] flash crc=0x%08lX expect=0x%08lX\r\n",
					(unsigned long)crc, (unsigned long)expect_crc);
		if (crc != expect_crc)
		{
			Uart_Printf(UART_LOG, "[apply] CRC MISMATCH\r\n");
			return OTA_APPLY_ERR;
		}
	}
	return OTA_APPLY_OK;
}


/** PENDING 分支：装新固件，成功后标志转 APPLIED 等 App 确认 */
static int8_t ApplyPending(OtaUpdateFlag_t *flag, uint32_t app_addr)
{
	OtaBackupHeader_t hdr;
	int8_t            r;

	/* 1) 备份由 APP 侧在启动确认时完成（见 APP/main.c）：那一刻片内是
	   "刚装好且已确认能跑"的固件，标志里的 length 正好是它的长度。
	   Boot 这里只检查备份是否存在，用于日志提示；不备份、不覆盖。 */
	r = OtaBackup_ReadHeader(&hdr);
	if (r == OTA_BAK_OK)
	{
		Uart_Printf(UART_LOG, "[apply] backup ready for rollback\r\n");
	}
	else
	{
		Uart_Printf(UART_LOG, "[apply] WARN: no backup, rollback unavailable\r\n");
	}

	/* 2) 搬新固件 */
	Uart_Printf(UART_LOG, "[apply] PENDING: applying new fw to 0x%08lX\r\n",
				(unsigned long)app_addr);
	r = CopyAndVerify(OTA_FW_BASE, app_addr, flag->length, flag->crc32);
	if (r != OTA_APPLY_OK)
	{
		return r;				/* 标志保持 PENDING，下次重试 */
	}

	/* 3) 全过才改成 APPLIED：等 App 确认 */
	r = OtaFlag_SetState(OTA_FLAG_APPLIED);
	Uart_Printf(UART_LOG, "[apply] state -> APPLIED %s\r\n",
				(r == OTA_FLAG_OK) ? "OK" : "FAIL");
	Uart_Printf(UART_LOG, "[apply] applied OK, waiting app confirm\r\n");
	return OTA_APPLY_OK;
}


/** APPLIED 分支：App 没确认，从备份恢复旧固件 */
static int8_t ApplyRollback(OtaUpdateFlag_t *flag, uint32_t app_addr)
{
	OtaBackupHeader_t hdr;
	int8_t            r;

	(void)flag;

	/* 1) 读备份头 */
	r = OtaBackup_ReadHeader(&hdr);
	if (r != OTA_BAK_OK)
	{
		Uart_Printf(UART_LOG, "[apply] rollback: no valid backup (%d), stay in boot\r\n", (int)r);
		return OTA_APPLY_ERR;
	}

	/* 2) 校验备份数据 CRC —— 绝不拿半截备份去恢复 */
	if (OtaBackup_VerifyData(&hdr) != 1)
	{
		Uart_Printf(UART_LOG, "[apply] rollback: backup data CRC BAD, stay in boot\r\n");
		return OTA_APPLY_ERR;
	}

	/* 3) 从备份恢复 */
	Uart_Printf(UART_LOG, "[apply] ROLLBACK: restoring %luB from backup\r\n",
				(unsigned long)hdr.length);
	r = CopyAndVerify(OTA_BACKUP_DATA_ADDR, app_addr, hdr.length, hdr.crc32);
	if (r != OTA_APPLY_OK)
	{
		return r;				/* 标志保持 APPLIED，下次再试 */
	}

	/* 4) 恢复成功，擦标志：旧固件已就位 */
	if (OtaFlag_Erase() != OTA_FLAG_OK)
	{
		Uart_Printf(UART_LOG, "[apply] WARN: flag erase FAIL\r\n");
	}
	Uart_Printf(UART_LOG, "[apply] rollback done, old fw restored\r\n");
	return OTA_APPLY_OK;
}


int8_t OtaApply_CheckAndApply(uint32_t app_addr)
{
	OtaUpdateFlag_t flag;
	int8_t          r;

	r = OtaFlag_Read(&flag);
	if (r == OTA_FLAG_EMPTY)
	{
		return OTA_APPLY_NOFLAG;	/* 没有更新，BootWindow 直接跳 App */
	}
	if (r == OTA_FLAG_BAD)
	{
		Uart_Printf(UART_LOG, "[apply] flag BAD, skip\r\n");
		return OTA_APPLY_ERR;
	}
	if (r != OTA_FLAG_OK)
	{
		Uart_Printf(UART_LOG, "[apply] flag read ERR\r\n");
		return OTA_APPLY_ERR;
	}

	Uart_Printf(UART_LOG, "[apply] flag: state=%s len=%lu crc=0x%08lX\r\n",
				(flag.state == OTA_FLAG_PENDING) ? "PENDING" : "APPLIED",
				(unsigned long)flag.length, (unsigned long)flag.crc32);

	if (flag.state == OTA_FLAG_PENDING)
	{
		return ApplyPending(&flag, app_addr);
	}
	return ApplyRollback(&flag, app_addr);
}
