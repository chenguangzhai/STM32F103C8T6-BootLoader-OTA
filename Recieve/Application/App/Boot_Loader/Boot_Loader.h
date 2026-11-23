/**
 * 串口收 bin -> 片内 Flash + 上电窗口跳转（可运行版）
 *
 * 【相对上一版的改动】新增 App 有效性判断、跳转函数、上电 3 秒窗口：
 *   上电后窗口期内收到串口数据 -> 留在 OTA 收 bin；
 *   窗口期内没有任何数据     -> 跳到 APP_ADDR(0x08005000) 运行新固件。
 *   这是参考 Boot_Loader 的完整方案，解决"App 写坏后永远救不回来"的问题。
 *
 * 【仍不做】CRC32、参数页、试用回滚、BKP 握手 —— 本轮只要能跳过去跑。
 */
#ifndef __BOOT_LOADER_H
#define __BOOT_LOADER_H

#include "uart_app.h"

/* 上电调一次：设置写入起点 APP_ADDR(0x08005000) 并清空落盘状态。
 * 必须在 Uart_Init() 之后调用（日志走串口）。 */
void Boot_Loader_Init(void);

/* 主循环每圈调一次：
 *   1) 从串口缓冲取数据 -> Ota_Feed()（满 1KB 擦写一页）
 *   2) 取不到数据但页里还有尾巴、且超时 -> Ota_Flush() 写出尾页
 * 返回值仍是「本次取到的字节数」，main 的 sum 统计不用改。 */
uint16_t Boot_Loader_Getlen(void);

/* 判断 app_addr 处是不是一份"看起来能跑"的固件。
 * 只读头两个 32 位字（初始 SP + 复位向量），不执行代码，对空白区也安全：
 *   SP 必须落在 SRAM(0x2000xxxx)；复位向量必须在 Flash 且最低位为 1(Thumb)。
 * 返回 1 = 有效，0 = 无效。 */
uint8_t Boot_Loader_AppValid(uint32_t app_addr);

/* 跳到 app_addr 处的固件。内部依次：关中断 -> 停 SysTick -> 清 NVIC ->
 * 停串口 DMA -> 停 SPI1 -> 设 VTOR -> 换 MSP -> 开中断 -> 跳。
 * 地址无效时直接返回，什么都不动；正常情况不返回。 */
void Boot_Loader_JumpToApp(uint32_t app_addr);

/* 上电窗口（阻塞）：等 OTA_WAIT_MS 毫秒，期间收到任意串口数据就返回 1
 * （表示留在 OTA，数据已交给 Ota_Feed，主循环继续调 Getlen 即可）；
 * 一直没数据且 App 有效则跳过去，不返回。
 * App 无效时立即返回 1，停在 OTA 等下载。 */
uint8_t Boot_Loader_BootWindow(void);

#endif
