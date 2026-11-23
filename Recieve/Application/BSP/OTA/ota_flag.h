/**
 * OTA 更新标志（APP 与 Boot 的"交接文书"）
 *
 * 【必须两份一致】本文件与 Recieve/Application/BSP/OTA/ota_flag.h 逐字相同。
 *   两个工程 srcDirs 独立、互相引用不到；改一边必须同步另一边，
 *   否则 APP 写的标志 Boot 读不懂，症状只是"永远不搬运"，不报错、极难查。
 *
 * 【作用与状态机】标志是三方交接的凭据，带 state 字段：
 *   APP 收完 bin 校验通过 → 写标志 state=PENDING（有新固件待装）
 *   Boot 读 PENDING       → 备份旧固件到 0x200000 → 搬新固件 → 标志改 APPLIED
 *   App 真的活过来        → 擦标志（新固件转正）
 *   App 跑飞没擦          → 下次 Boot 读 APPLIED → 从备份恢复旧固件 → 擦标志
 *
 * 【布局】W25Q64 共 8MB，五块互不重叠：
 *   0x000000 ~ 0x000FFF  更新标志（本模块，独占扇区）
 *   0x001000 ~ 0x0FFFFF  前 1MB 剩余（保留）
 *   0x100000 ~ 0x1FFFFF  新固件区（ota_w25q，1MB）
 *   0x200000 ~ 0x2FFFFF  备份区（ota_backup，存上一版可运行固件，1MB）
 *   0x7FF000 ~ 0x7FFFFF  末扇区自检（w25q_test）
 *
 *   固件实际只有约 16KB，1MB 区绰绰有余；中间 0x300000~0x7FEFFF 闲置。
 *
 * 【为什么带 self_crc】擦标志→写标志之间若断电，扇区里是"写了一半"的内容。
 *   光看 magic 会误判成有效标志，Boot 就去搬一份可能不完整/不对的固件。
 *   self_crc 覆盖前 16 字节，能认出这种半成品。
 */
#ifndef __OTA_FLAG_H
#define __OTA_FLAG_H

#include <stdint.h>
#include "w25q.h"			/* 底层读/写/擦 W25Q */

/* 标志在 W25Q 中的地址：第 0 扇区首 */
#define OTA_FLAG_ADDR       0x000000UL

/* 'U','P','D','T' 小端。全 0xFF 空片不会命中 */
#define OTA_FLAG_MAGIC      0x54445055UL

/* state 取值 */
#define OTA_FLAG_PENDING    1U      /* 新固件待装：Boot 应备份+搬运 */
#define OTA_FLAG_APPLIED    2U      /* 已装但 App 未确认：Boot 应回滚 */

/* 返回值 */
#define OTA_FLAG_OK         0       /* 标志有效 */
#define OTA_FLAG_EMPTY      1       /* 空片，无更新 */
#define OTA_FLAG_BAD        2       /* 有内容但 magic/CRC/state 不对 */
#define OTA_FLAG_ERR        (-1)    /* 底层读写失败 */

typedef struct
{
	uint32_t magic;         /* OTA_FLAG_MAGIC */
	uint32_t length;        /* 固件字节数 */
	uint32_t crc32;         /* 固件区 [0x100000, +length) 的 CRC32 */
	uint32_t state;         /* OTA_FLAG_PENDING / APPLIED */
	uint32_t self_crc;      /* 前 16 字节的 CRC32，防写一半 */
} OtaUpdateFlag_t;          /* sizeof = 20 */

/* 读并校验标志。返回 OK / EMPTY / BAD / ERR；非 OK 时 flag 内容不可信。 */
int8_t OtaFlag_Read(OtaUpdateFlag_t *flag);

/* 写标志：先擦扇区，再写 20 字节，最后回读校验 self_crc。
 * length/crc 是固件信息，state 取 PENDING 或 APPLIED。 */
int8_t OtaFlag_Write(uint32_t length, uint32_t crc32, uint32_t state);

/* 只改 state（读-改-写），保留 length/crc。Boot 搬运完成后调它置 APPLIED。 */
int8_t OtaFlag_SetState(uint32_t state);

/* 擦标志：App 确认后 / 回滚完成后调用。 */
int8_t OtaFlag_Erase(void);

#endif /* __OTA_FLAG_H */
