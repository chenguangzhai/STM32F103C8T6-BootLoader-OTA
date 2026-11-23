/**
 * OTA 落盘：串口字节流 -> 攒满一页 -> 写进片内 Flash
 *
 * 【来源与裁剪】从 参考/Boot_Loader/Application/BSP/OTA/ota_app.c/.h 移植，
 *   按用户本轮要求「先验证能不能写进去」做了最小裁剪：
 *   保留：Ota_Init / Ota_Feed / Ota_Flush / 状态查询、每页进度日志、失败锁死。
 *   去掉：尾部记录、CRC32、参数页、试用回滚、W25Q 双槽、跳转 App。
 *   原因：那些是「装配成功后才需要的安全网」，现在连写入是否成功都还没验证，
 *   先把最短闭环跑通，省得一次调试同时面对太多变量。
 *
 * 用法：
 *   Ota_Init(APP_ADDR);            上电设一次写入起点
 *   Ota_Feed(buf, len);            串口每收到一段就喂进来，满 1KB 自动擦写一页
 *   Ota_Flush();                   数据发完了，把不足一页的尾巴写出去
 *
 * 为什么攒满一页才写：擦一页要 20~40ms，写一页才 1~2ms。每收 256B 就擦一次
 * 会让同一页被擦 8 次，慢 8 倍、Flash 寿命也砍到 1/8。
 *
 * 边界：一页 = 1KB（C8T6），起点 APP_ADDR = 0x08005000 已 1KB 对齐；
 *       写满到 FLASH_END 会锁死，不会绕回去擦 Boot。
 */
#ifndef __OTA_APP_H
#define __OTA_APP_H

#include <stdint.h>
#include "flash_app.h"		/* 分区宏 APP_ADDR / FLASH_PAGE_BYTES 从这里来 */

/* 一页多大，与 Flash 擦除单位保持一致。不要在别处另写 1024。 */
#define OTA_PAGE_BYTES      FLASH_PAGE_BYTES

/* ==================== 返回值 ==================== */
#define OTA_OK              0
#define OTA_ERR_ARG         (-1)	/* 没调 Ota_Init / 空指针 / 长度 0 */
#define OTA_ERR_ERASE       (-2)	/* 擦除失败 */
#define OTA_ERR_WRITE       (-3)	/* 写入失败 */
#define OTA_ERR_FULL        (-4)	/* 写满 App 区，拒绝继续（本版新增，防止擦到 Flash 外） */

/* 设定写入起点并清空内部状态。
 * start_addr 必须页首对齐；重复调用等于「从头再来」，之前没写的数据丢弃。 */
void Ota_Init(uint32_t start_addr);

/* 喂一段数据，内部按 1KB 攒页，满页就擦写。len 任意（1 字节也行）。
 * 返回 OTA_OK 表示这批全处理完；负数表示中途失败。
 * 失败会「锁死」：之后 Feed/Flush 直接返回同一错误码，直到重新 Ota_Init，
 * 避免同一页被反复擦写耗尽寿命。 */
int8_t Ota_Feed(const uint8_t *buf, uint32_t len);

/* 把不满一页的尾巴写出去。不调它，最后一页会烂在 RAM 里。
 * 没有待写数据时调用无害，直接返回 OTA_OK。 */
int8_t Ota_Flush(void);

/* 已真正落进 Flash 的字节数（不含 RAM 里攒着的部分）。 */
uint32_t Ota_Total(void);

/* 当前页攒了多少字节。0 表示没待写数据。 */
uint32_t Ota_Fill(void);

/* 下一个要写的页首地址，用来判断有没有写越界。 */
uint32_t Ota_Addr(void);

#endif /* __OTA_APP_H */
