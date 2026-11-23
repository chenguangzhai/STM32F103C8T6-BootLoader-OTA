/**
 * OTA 搬运（Boot 侧）：W25Q64 固件 -> 片内 App 区
 *
 * 【职责】Boot 上电读到有效的更新标志后，把 W25Q 0x100000 的固件
 *   搬进片内 0x08005000，校验通过再交给 Boot 跳转。
 *
 * 【流程】
 *   1) OtaFlag_Read() 读标志；无标志直接返回"无需搬运"
 *   2) 检查长度/CRC 合法性（长度不超 App 区）
 *   3) 逐 1KB 页：从 W25Q 读 -> 擦片内页 -> 写片内页
 *   4) Crc32_Flash() 从片内读回算 CRC，与标志里的比对
 *   5) 全通过 -> OtaFlag_Erase() 擦标志 -> 返回成功（Boot 随后跳转）
 *
 * 【失败处理】任何一步失败都返回错误，且**不擦标志** ——
 *   下次上电会重试搬运，这是"失败可恢复"的关键。只有搬运+校验全过才擦标志。
 *
 * 【风险（本版未处理）】擦标志后跳转失败，下次上电不再搬运。
 *   完整方案需要试用确认+回滚（参考工程有），本轮按用户要求只做最小闭环。
 */
#ifndef __OTA_APPLY_H
#define __OTA_APPLY_H

#include <stdint.h>

/* 返回值 */
#define OTA_APPLY_OK        0       /* 搬运+校验成功，标志已擦 */
#define OTA_APPLY_NOFLAG    1       /* 没有更新标志，无需搬运 */
#define OTA_APPLY_ERR       (-1)    /* 搬运失败（标志保留，下次重试） */

/* 检查更新标志并把 W25Q 固件搬进片内 App 区。
 * 返回 OTA_APPLY_OK / NOFLAG / ERR。
 * 参数 app_addr 是片内写入起点（APP_ADDR = 0x08005000）。 */
int8_t OtaApply_CheckAndApply(uint32_t app_addr);

#endif /* __OTA_APPLY_H */
