/**
 * OTA 备份区（W25Q64 0x200000，2MB 处）
 *
 * 【必须两份一致】本文件与 Recieve/Application/BSP/OTA/ota_backup.h 逐字相同。
 *
 * 【作用】升级前把"当前正在跑的旧固件"从片内 Flash 读到 W25Q 备份区。
 *   新固件跑飞时，Boot 从备份区恢复旧版本，而不是反复重灌同一份坏固件。
 *
 * 【为什么备份源是片内而不是 W25Q】W25Q 0x100000 里是"待装的新固件"，
 *   备份它没有意义。要备份的是"已确认能跑的旧固件"—— 它此刻就在片内 0x08005000，
 *   而且 App 启动时擦了标志，恰好证明这一版是被确认过的。
 *
 * 【布局】备份头独占一个 4KB 扇区，数据从下一扇区起：
 *   0x200000 ~ 0x200FFF  备份头（OtaBackupHeader_t，独占，防止写头时碰到数据）
 *   0x201000 ~ 0x2FFFFF  备份数据（约 1MB，装得下任何合法固件）
 *
 * 【防写坏】头和固件标志同款 self_crc；备份数据写完从 W25Q 读回算 CRC 比对。
 *   恢复前 Boot 必须校验头 + 数据 CRC，全过才恢复；校验不过就不回滚（停在 Boot 等重传），
 *   绝不往火坑里跳。
 */
#ifndef __OTA_BACKUP_H
#define __OTA_BACKUP_H

#include <stdint.h>
#include "w25q.h"

#define OTA_BACKUP_HDR_ADDR  0x00200000UL    /* 备份头：2MB 处，独占扇区 */
#define OTA_BACKUP_DATA_ADDR 0x00201000UL    /* 备份数据：头扇区之后 */
#define OTA_BACKUP_DATA_MAX  0x000FF000UL    /* 到 0x300000 前，约 1MB */

/* 'B','A','K','U' 小端 */
#define OTA_BACKUP_MAGIC     0x554B4142UL

/* 返回值 */
#define OTA_BAK_OK           0
#define OTA_BAK_EMPTY        1      /* 从没备份过 */
#define OTA_BAK_BAD          2      /* 头/数据校验不过 */
#define OTA_BAK_ERR          (-1)   /* 底层读写失败 */

typedef struct
{
	uint32_t magic;         /* OTA_BACKUP_MAGIC */
	uint32_t length;        /* 备份固件字节数 */
	uint32_t crc32;         /* 备份数据的 CRC32 */
	uint32_t self_crc;      /* 前 12 字节的 CRC32，防写一半 */
} OtaBackupHeader_t;        /* sizeof = 16 */

/* 从片内 flash_addr 读 length 字节，写到备份区并回读校验。
 * 顺序：擦数据区 → 写数据 → 读回算 CRC → CRC 对才写头（头存在即代表备份完整）。 */
int8_t OtaBackup_SaveFromFlash(uint32_t flash_addr, uint32_t length);

/* 读备份头并校验（不校验数据）。返回 OK / EMPTY / BAD / ERR。 */
int8_t OtaBackup_ReadHeader(OtaBackupHeader_t *hdr);

/* 校验备份数据 CRC32 是否与头一致。返回 1=一致 / 0=不一致 / 负数=读失败。 */
int8_t OtaBackup_VerifyData(const OtaBackupHeader_t *hdr);

/* 从备份区读 length 字节到 buf（Boot 恢复时用）。 */
int8_t OtaBackup_ReadData(uint32_t offset, uint8_t *buf, uint32_t len);

#endif /* __OTA_BACKUP_H */
