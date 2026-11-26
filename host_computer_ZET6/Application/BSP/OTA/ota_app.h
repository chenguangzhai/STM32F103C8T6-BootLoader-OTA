/**
 * OTA 落盘：把串口收到的字节流，攒成整扇区后写进外部 Flash 槽 A
 *
 * 2026-11-25 改动：本工程不再跳 App、也不写片内 Flash。
 * 串口数据是"待发送的内容"，落到 W25Q 槽 A 的数据区，等后续经 CAN 发出。
 * 因此底层擦写从 flash_app（片内 2KB 页）换成 w25q（外部 4KB 扇区 / 256B 编程页）。
 *
 *     Ota_Init(OtaSlot_DataAddr(OTA_SLOT_A));   设一次写入起点（0x001000）
 *     Ota_Feed(buf, len);                        攒满 4KB 自动擦写一个扇区
 *     Ota_Flush();                               把不足一扇区的尾巴也写出去
 *
 * 为什么要攒满 4KB 再写：W25Q 擦除单位是 4KB 扇区、典型 45ms（最坏 400ms），
 * 而编程单位只有 256B。若每 256B 都"擦+写"，同一扇区要被擦 16 次，
 * 速度和寿命都砍到 1/16。按扇区攒是最省的。
 *
 * 边界：
 *   1) 一"页"= 4KB 外部扇区，起点必须 4096 对齐（槽 A 数据区 0x001000 满足）
 *   2) Ota_Flush() 之后，下一个扇区从当前地址 + 4KB 开始，中间不留缝
 *   3) 容量上限由调用方用 OTA_SLOT_DATA_MAX 判断（508KB），本模块不越界检查
 */
#ifndef __OTA_APP_H
#define __OTA_APP_H

#include <stdint.h>

/* 与外部 W25Q 的擦除单位保持一致。不要在别处另写 4096，改这里就够。 */
#define OTA_PAGE_BYTES      4096UL

/* ==================== 返回值 ==================== */
#define OTA_OK              0
#define OTA_ERR_ARG         (-1)	/* 没调 Ota_Init / 传了空指针 */
#define OTA_ERR_ERASE       (-2)	/* 扇区擦除失败 */
#define OTA_ERR_WRITE       (-3)	/* 写入失败 */

/* 设定写入起点，同时把内部状态清零。
 * start_addr 必须是 4KB 扇区首地址，本工程固定传 OtaSlot_DataAddr(OTA_SLOT_A)=0x001000。
 * 重复调用等于"从头再来"，之前攒了没写的数据会被丢掉。 */
void Ota_Init(uint32_t start_addr);

/* 喂一段数据。内部自动按 4KB 攒扇区，攒满就擦一扇区写一扇区。
 * 一次喂多少都行：len 可以是 1 字节，也可以是 5000 字节（会连续写好几个扇区）。
 * 返回 OTA_OK 表示这一批全部处理完；返回负数表示中途某一扇区失败。
 *
 * 失败会「锁死」：之后本模块不再碰 Flash，Ota_Feed / Ota_Flush 直接返回
 * 同一个错误码，直到重新调 Ota_Init。避免反复擦同一扇区把寿命耗光。 */
int8_t Ota_Feed(const uint8_t *buf, uint32_t len);

/* 把当前扇区里"不满 4KB"的尾巴写出去。
 * 不发这个，最后一扇区会一直留在 RAM 里 —— 现象就是"数据总是少最后一段"。
 * 没有待写数据时调用它无害，直接返回 OTA_OK。 */
int8_t Ota_Flush(void);

/* 已经真正落进外部 Flash 的字节数（不含还在 RAM 里攒着的那部分）。 */
uint32_t Ota_Total(void);

/* 当前这一扇区攒了多少字节。0 表示没有待写数据。 */
uint32_t Ota_Fill(void);

/* 下一个要写的扇区首地址。用它判断有没有写越界。 */
uint32_t Ota_Addr(void);

#endif
