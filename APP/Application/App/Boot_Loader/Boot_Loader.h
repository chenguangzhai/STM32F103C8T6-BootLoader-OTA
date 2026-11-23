/**
 * APP 侧串口收 bin -> W25Q64（两级 OTA 的下载段）
 *
 * 【职责边界】APP 只写片外 W25Q64 的 0x100000 起（前 1MB 留给升级标志）；
 *   片内 Flash 由 Boot（Recieve）侧负责写入和跳转，APP 不碰。
 */
#ifndef __BOOT_LOADER_H
#define __BOOT_LOADER_H

#include "uart_app.h"

/* 上电调一次：绑定 W25Q64 的 SPI/片选，设写入起点 0x100000。
 * 必须在 Uart_Init() 之后（日志走串口）。 */
uint8_t Boot_Loader_Init(void);

/* 主循环每圈调一次：从串口取数据 -> OtaW25q_Feed（满 4KB 擦写一扇区）；
 * 没新数据但扇区里有尾巴且超时 -> OtaW25q_Finish() 补尾 + 回读比对。
 * 返回值仍是本次取到的字节数，main 的 sum 统计不用改。 */
uint16_t Boot_Loader_Getlen(void);

/* 主动收尾：把不满 4KB 的尾巴写出并回读校验。
 * 正常由 Getlen 的 idle 超时自动调用；上位机发完想立即看结果时也可手动调。
 * 返回 0 = 写入且逐字节比对通过，非 0 = 失败。 */
int8_t Boot_Loader_Finish(void);

#endif
