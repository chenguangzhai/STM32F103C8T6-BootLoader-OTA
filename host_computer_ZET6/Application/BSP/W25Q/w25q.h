/**
 * W25Q64 SPI Flash 驱动（8 MB，SPI1，PA4 片选）
 *
 * 2026-11-25：按板上实装器件把容量/ID 从 W25Q128(16MB) 改为 W25Q64(8MB)。
 * OTA 布局最高只用到 0x101000（约 1MB），远在 8MB 之内，缩小容量边界不影响槽与备份。
 *
 * 【本文件是 BootLoader 侧副本，源文件在 Application/Application/BSP/W25Q/】
 * 为什么要复制而不是共享：两个工程各自只把本目录下的 Core/Drivers/Application
 * 加进 srcDirs，物理上无法引用对方的源文件。
 * 维护约定：任何修改都必须两边同步，否则 App 写进去的数据 Boot 读出来是错的，
 * 而症状只是"CRC 对不上"—— 极难定位。改动时请顺手 diff 一下两份文件。
 *
 * 板上接线（野火 STM32F103ZET6 核心板双 USB 款，见参考目录引脚分配表第 8~11 行）：
  *     PA5 → CLK(6)     PA6 → DO/IO1(2)     PA7 → DI/IO0(5)     PA4 → CS(1)
  *
  * 三个必须先记住的尺寸，它们不是一回事：
  *     擦除单位  4 KB 扇区      片内 Flash 是 2 KB 页，这里不一样
  *     编程单位  256 字节一页    超过会静默回卷到页首覆盖开头，不报错
  *     读取单位  任意长度        读没有页/扇区限制
  *
  * 和片内 Flash 的另外两个区别：
  *     1) 每次擦/写之前都要发 Write Enable(06h)，片内 Flash 是解锁一次就行
  *     2) 写完要轮询状态寄存器 BUSY 位，不像片内 Flash 写完就完事
  */
#ifndef __W25Q_H
#define __W25Q_H

#include <stdint.h>
#include "stm32f1xx_hal.h"

#define W25Q_SIZE           (8UL * 1024UL * 1024UL)    /* 64 Mbit = 8 MB（板上 W25Q64） */
#define W25Q_SECTOR_SIZE    4096UL                     /* 擦除单位 4 KB */
#define W25Q_PAGE_SIZE      256UL                      /* 编程单位 256 B */

#define W25Q_JEDEC_ID       0xEF4017UL                 /* Winbond + 64Mbit = W25Q64 */

/* 返回码：0 成功，负数失败。失败时 Flash 里可能已被部分修改，调用方自行决定重试还是放弃 */
#define W25Q_OK             0
#define W25Q_ERR_ARG       -1      /* 参数不合法（空指针 / 长度越界 / 没初始化） */
#define W25Q_ERR_ADDR      -2      /* 地址越界或没对齐 */
#define W25Q_ERR_SPI       -3      /* SPI 传输失败 */
#define W25Q_ERR_TIMEOUT   -4      /* 等 BUSY 超时 */

/**
  * @brief  绑定 SPI 句柄和片选脚，并把 CS 配成推挽输出（默认拉高）
 * @param  hspi    已由 MX_SPI1_Init() 初始化好的句柄，传 &hspi1
 * @param  cs_port 片选所在端口，传 GPIOA
 * @param  cs_pin  片选引脚，传 GPIO_PIN_4
 * @retval W25Q_OK / W25Q_ERR_ARG
 * @note   SPI 本身（模式、分频）由 CubeMX 配，这个函数只管片选。
 *         为什么不把 CS 交给硬件 NSS：PA4 同时也是 SPI1_NSS，
 *         软件片选才能在整个命令+数据周期里稳定拉低，不会由 HAL 中途释放。
  */
int8_t W25q_Init(SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin);

/**
  * @brief  读 JEDEC ID
  * @retval 0xEF4017 正常（W25Q64）；0xFFFFFF 或 0x000000 说明没接上/片选没生效
  */
uint32_t W25q_ReadId(void);

/**
  * @brief  读状态寄存器 1
  * @retval bit0=BUSY, bit1=WEL(写使能锁存), bit2~6=BP 保护位
  */
uint8_t W25q_ReadStatus(void);

/**
  * @brief  擦除一个 4 KB 扇区
  * @param  addr 扇区首地址，必须是 4096 的整数倍
  * @retval W25Q_OK / 错误码
  * @note   擦完整个扇区变 0xFF。擦除期间典型 45 ms、最坏 400 ms。
  */
int8_t W25q_EraseSector(uint32_t addr);

/**
  * @brief  写一页，最多 256 字节，且不能跨页
  * @param  addr 写到哪里
  * @param  buf  数据
  * @param  len  1~256
  * @retval W25Q_OK / 错误码
  * @note   一般不用直接调这个，用 W25q_Write() 它会自动切页。
  */
int8_t W25q_WritePage(uint32_t addr, const uint8_t *buf, uint16_t len);

/**
  * @brief  写任意长度，自动按 256 字节页边界切分
  * @param  len 想写多长都行
  * @retval W25Q_OK / 错误码
  * @note   写之前目标区域必须已擦除（全 0xFF），否则写进去的是旧数据与新数据的与。
  */
int8_t W25q_Write(uint32_t addr, const uint8_t *buf, uint32_t len);

/**
  * @brief  读任意长度
  * @retval W25Q_OK / 错误码
  */
int8_t W25q_Read(uint32_t addr, uint8_t *buf, uint32_t len);

#endif /* __W25Q_H */
