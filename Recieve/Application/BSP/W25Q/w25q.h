/**
 * W25Q64 SPI Flash 驱动（8 MB，SPI1，PB0 片选）
 *
 * 【来源】从 Boot_Loader 工程的 W25Q 驱动移植而来。
 * 移植原因：Recieve 工程和 Boot_Loader 各自只把本目录下的 Application 加进
 *           srcDirs，物理上无法直接引用对方源文件，所以这里保留一份独立副本。
 * 与源版的差异（只改器件参数，通信逻辑完全一致）：
 *     1) 容量 16 MB(W25Q128) -> 8 MB(W25Q64)，W25Q_SIZE 改小；
 *     2) JEDEC ID 0xEF4018 -> 0xEF4017（容量码 0x18 -> 0x17）；
 *     3) 接线说明改为 Recieve 板的 SPI1；
 *     4) 去掉"两份文件必须同步"的约定——Recieve 侧不再有第二份副本，
 *        但如果需要和 Boot_Loader 共用同一颗 Flash，两版命令集相同可互认。
 * 注意：两个型号的命令集和 4KB 扇区 / 256B 页结构完全相同，故 .c 的实现可直接复用。
 *
 * Recieve 板上接线（由 spi.c / main.h 的 CubeMX 配置决定）：
 *     PA5 -> CLK(6)     PA6 -> SO/MISO(2)     PA7 -> SI/MOSI(5)     PB0 -> CS(1)
 *     SPI1 初始化：模式0(CPOL=0/CPHA=0)、软件 NSS、预分频 8
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

#define W25Q_SIZE           (8UL * 1024UL * 1024UL)    /* 64 Mbit = 8 MB（W25Q64） */
#define W25Q_SECTOR_SIZE    4096UL                     /* 擦除单位 4 KB */
#define W25Q_PAGE_SIZE      256UL                      /* 编程单位 256 B */

#define W25Q_JEDEC_ID       0xEF4017UL                 /* Winbond + 64Mbit（容量码 0x17） */

/* 返回码：0 成功，负数失败。失败时 Flash 里可能已被部分修改，调用方自行决定重试还是放弃 */
#define W25Q_OK             0
#define W25Q_ERR_ARG       -1      /* 参数不合法（空指针 / 长度越界 / 没初始化） */
#define W25Q_ERR_ADDR      -2      /* 地址越界或没对齐 */
#define W25Q_ERR_SPI       -3      /* SPI 传输失败 */
#define W25Q_ERR_TIMEOUT   -4      /* 等 BUSY 超时 */

/**
  * @brief  绑定 SPI 句柄和片选脚，并把 CS 配成推挽输出（默认拉高）
  * @param  hspi    已由 MX_SPI1_Init() 初始化好的句柄，传 &hspi1
  * @param  cs_port 片选所在端口，传 W25Q_CS_GPIO_Port（GPIOB）
  * @param  cs_pin  片选引脚，传 W25Q_CS_Pin（GPIO_PIN_0）
  * @retval W25Q_OK / W25Q_ERR_ARG
  * @note   SPI 本身（模式、分频）由 CubeMX 配，这个函数只管片选。
  *         CS 未在 CubeMX 里交给硬件 NSS：软件片选才能精确控制帧边界，
  *         也便于将来同一 SPI 上挂其它器件。
  *         与源版一致：这里会再初始化一次 CS；gpio.c 已把它配成输出高，
  *         对已配置引脚重复 HAL_GPIO_Init 只是重新写一次寄存器，无副作用。
  */
int8_t W25q_Init(SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin);

/**
  * @brief  读 JEDEC ID
  * @retval 0xEF4017 正常；0xFFFFFF 或 0x000000 说明没接上/片选没生效/时序错位
  * @note   内部已改为全双工 TransmitReceive，修复了原版拆分收发丢应答字节的问题
  */
uint32_t W25q_ReadId(void);

/**
  * @brief  【诊断用】读 JEDEC ID 的 3 个原始字节，供自检定位问题
  * @param  id 输出 3 字节，依次应为 0xEF / 0x40 / 0x17
  * @retval W25Q_OK / 错误码
  * @note   全 FF 一般是 MISO 悬空或芯片没响应；全 00 多半是 MISO 被拉低/接线错。
  *         仅用于诊断，不改变 Flash 状态。
  */
int8_t W25q_ReadIdRaw(uint8_t id[3]);

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
