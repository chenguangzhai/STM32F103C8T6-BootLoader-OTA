/**
 * OTA 落盘（APP 侧，写入 W25Q64 片外 Flash）
 *
 * 【职责】把串口收到的 bin 攒成 4KB 扇区，写进 W25Q64 的 0x100000 起。
 *   写完后回读逐字节比对，确认真的写进去了。
 *
 * 【为什么不用 Recieve 那套 ota_app】那套底层是片内 Flash：擦 1KB 页、2 字节半字写。
 *   W25Q64 完全不同：擦 4KB 扇区、写 256B 页（W25q_Write 内部自动切页）。
 *   颗粒度差 4 倍，直接复用会擦错边界，所以单独做一个模块。
 *
 * 【布局】W25Q64 共 8MB：
 *   0x000000 ~ 0x0FFFFF  前 1MB：留给用户存"是否升级"标志（本模块不碰）
 *   0x100000 ~ 0x7FEFFF  固件区：bin 写这里（本模块）
 *   0x7FF000 ~ 0x7FFFFF  末 4KB：W25q_Test 自检专用（不冲突）
 *
 * 用法：
 *   OtaW25q_Init(&hspi1, W25Q_CS_GPIO_Port, W25Q_CS_Pin);   上电一次
 *   OtaW25q_Feed(buf, len);      每收到一段喂进来，满 4KB 自动擦写一扇区
 *   OtaW25q_Finish();            发完了：补尾扇区 + 回读比对
 */
#ifndef __OTA_W25Q_H
#define __OTA_W25Q_H

#include <stdint.h>
#include "stm32f1xx_hal.h"
#include "w25q.h"

/* 固件在 W25Q64 中的起始地址：1MB，前面留给升级标志 */
#define OTA_W25Q_BASE       0x00100000UL
/* 固件区上限：末扇区留给自检，不写进去 */
#define OTA_W25Q_END        0x007FF000UL
/* 攒页单位 = W25Q 擦除单位 4KB（与片内 Flash 的 1KB 不同） */
#define OTA_W25Q_SECTOR     W25Q_SECTOR_SIZE

/* 返回值 */
#define OTAW_OK             0
#define OTAW_ERR_ARG        (-1)    /* 没 Init / 空指针 / 长度 0 */
#define OTAW_ERR_ERASE      (-2)    /* 扇区擦除失败 */
#define OTAW_ERR_WRITE      (-3)    /* 写入失败 */
#define OTAW_ERR_READ       (-4)    /* 回读失败 */
#define OTAW_ERR_VERIFY     (-5)    /* 回读内容不一致 */
#define OTAW_ERR_FULL       (-6)    /* 超出固件区，拒绝继续 */

/* 绑定 SPI/片选并设写入起点（内部会调 W25q_Init）。
 * 重复调用等于从头再来，之前没写的数据丢弃。 */
int8_t OtaW25q_Init(SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin);

/* 喂一段数据，内部按 4KB 攒扇区，满就擦+写。len 任意。
 * 失败会锁死，直到重新 Init（防止同一扇区被反复擦写耗尽寿命）。 */
int8_t OtaW25q_Feed(const uint8_t *buf, uint32_t len);

/* 收尾：把不满 4KB 的尾巴写出，然后回读 [BASE, BASE+Total) 逐字节比对。
 * 返回 OTAW_OK 表示写入且校验都通过；OTA_VERIFY 时可用 OtaW25q_BadOffset() 查第一个错字节。 */
int8_t OtaW25q_Finish(void);

/* 状态查询 */
uint32_t OtaW25q_Total(void);     /* 已写进 W25Q 的字节数 */
uint32_t OtaW25q_Fill(void);      /* 当前扇区攒了多少字节 */
uint32_t OtaW25q_BadOffset(void); /* 回读比对失败的第一个偏移（相对固件起点），成功时为 0 */

#endif /* __OTA_W25Q_H */
