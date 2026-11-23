/**
 * 片内 Flash 擦写（C8T6 最小可验证版）
 *
 * 【来源与差异】从 参考/Boot_Loader/Application/BSP/Flash/flash_app.h 移植。
 *   参考工程按 STM32F103xE 大容量芯片配置：2KB/页、512KB 末尾、boot 32K；
 *   本工程是 STM32F103C8T6 中容量芯片，必须改成：
 *     1) FLASH_PAGE_BYTES 0x800 -> 0x400（1KB/页，C8T6 真实擦除单位）
 *     2) BOOT_SIZE 0x8000 -> 0x5000（20K，与本工程 .ld 的 LENGTH=20K 一致）
 *     3) FLASH_END 0x08080000 -> 0x08010000（C8T6 只有 64KB，写越界会静默失败）
 *   擦写函数体与参考版完全一致：底层只认页首地址 + 半字编程，与容量无关。
 *
 * 【为什么 Recieve 能当 boot】本工程 .ld 就是 0x08000000 起、20K 长，
 *   与下面 Boot 分区严丝合缝；收到的 bin 写到 0x08005000 起的 App 区，
 *   两边互不覆盖。APP 工程将来要生成能跑在 0x08005000 的固件。
 *
 * 三条硬件铁律（不是建议）：
 *   1) 写之前必须先擦。Flash 只能 1->0，想变回 1 只能整页擦。
 *   2) 擦除最小单位一页 = 1KB（C8T6），必须给页首地址。
 *   3) 写按半字 = 2 字节，地址必须是偶数。
 */
#ifndef __FLASH_APP_H
#define __FLASH_APP_H

#include <stdint.h>

/* ==================== 分区（唯一定义处，别处一律用这些宏） ====================
 *   0x08000000 ~ 0x08004FFF   Boot  20KB（本 Recieve 固件，含 .ld 限制）
 *   0x08005000 ~ 0x0800FFFF   App   44KB（串口收到的 bin 落这里）
 *
 * 注意：Recieve 固件当前占用约 15KB，离 20K 上限只剩 5KB。
 * 本次新增的 flash_app + ota_app 会再吃掉一部分，若编译报
 * "FLASH length exceeded"，先调 .ld 或精简日志，不要动 BOOT_SIZE 去覆盖 App。
 */
#define FLASH_PAGE_BYTES    0x400U                       /* 一页 1KB（C8T6 中容量） */
#define BOOT_ADDR           0x08000000U
#define BOOT_SIZE           0x5000U                      /* 20KB，与 Recieve .ld 对齐 */
#define APP_ADDR            (BOOT_ADDR + BOOT_SIZE)      /* 0x08005000，页首 */
#define FLASH_END           0x08010000U                  /* C8T6 64KB 末尾 */
#define APP_MAX             (FLASH_END - APP_ADDR)       /* 44KB = 0xB000 */

/* ==================== 返回值 ==================== */
#define FLASH_OK            0
#define FLASH_ERR_ARG       (-1)    /* 参数不对：地址没对齐 / 空指针 / 长度 0 */
#define FLASH_ERR_ERASE     (-2)    /* 擦除失败 */
#define FLASH_ERR_WRITE     (-3)    /* 写入失败（最常见：这段没擦干净） */

/* 擦 addr 所在的这一页。addr 必须是页首（1KB 整数倍）。
 * 副作用：整页变 0xFF，不可恢复；所以 ota_app 只在攒满一页后才调用。 */
int8_t Flash_ErasePage(uint32_t addr);

/* 从 addr 起连写 len 个字节。前提：这段已擦过。
 * 奇数长度时最后一个字节补 0xFF 凑半字。写完不做校验。 */
int8_t Flash_Write(uint32_t addr, const uint8_t *buf, uint32_t len);

#endif /* __FLASH_APP_H */
