/**
  * CAN 驱动：过滤器、收发一帧、错误统计、配置校验、回环自测
  *
  * 【职责边界（2026-11-24 调整，重要）】
  *   位时序（Prescaler / BS1 / BS2 / SJW）、三个行为开关（ABOM / ARD / RFL）、
  *   工作模式（Normal / Loopback）、GPIO 与 AFIO remap、NVIC 使能、中断处理函数
  *   —— 全部由 **CubeMX 与 .ioc 负责**，本驱动一个字都不改。这是单一真相来源：
  *   参数只有 .ioc 一处，重新生成代码不会与代码里的数值打架。
  *
  *   CubeMX 做不到、必须由本驱动补的三件事：
  *     1) 过滤器 bank（F1 的 CubeMX 没有过滤器配置界面）
  *     2) HAL_CAN_Start + HAL_CAN_ActivateNotification（CubeMX 只生成初始化，外设不会自己进收发态）
  *     3) 收帧入队、错误统计、自测等业务逻辑
  *
  *   那怎么防止「CubeMX 里点错了自己不知道」？本驱动提供 CanDrv_Init() 的**读回校验**：
  *   从 hcan.Init 反算真实波特率与采样点，不在预期区间就返回错误码；同时把实际值
  *   通过 CanDrv_ActualBaudrate / ActualSamplePointPermille 报给上层打印。
  *   于是 regen 之后如果真的退回 750k，日志里立刻能看见，而不是静默跑歪。
  *
 * 【本文件不 include 任何 HAL 头】
 *   上层（ISO-TP、会话状态机）只需要帧和返回值，不需要认识 CAN_HandleTypeDef。
 *
 * 【ZET6 侧适配（2026-11-25）】
 *   结构与 C8T6/APP 已验证版逐字一致，唯一差异是角色不同：ZET6 是上位机，
 *   将来在总线上要收的是下位机应答 0x581，因此本侧过滤器与回环自测帧都用
 *   CANDRV_ID_RSP；0x601 保留给上位机发命令。这样接真实总线时不必临时改过滤器。
 */
#ifndef __CAN_DRV_H
#define __CAN_DRV_H

#include <stdint.h>

/* ==================== 节点与报文 ID ====================
 * CAN 是广播总线，串口是点对点。不带节点号的话，多台设备同时在线时
 * 一条请求会被全网所有节点响应。所以沿用 UDS 惯例：
 *     上位机 -> 下位机  0x600 + node_id
 *     下位机 -> 上位机  0x580 + node_id
 * 单机取 node_id = 1，即请求 0x601 / 响应 0x581。
 * 注意：C8T6 下位机的硬件过滤器认 0x601；ZET6 上位机的过滤器认 0x581。
 *       两边都用精确匹配而不是掩码范围匹配，避免总线上的无关帧混进接收队列。 */
#define CANDRV_NODE_ID          1U
#define CANDRV_ID_REQ_BASE      0x600U
#define CANDRV_ID_RSP_BASE      0x580U
#define CANDRV_ID_REQ           (CANDRV_ID_REQ_BASE + CANDRV_NODE_ID)
#define CANDRV_ID_RSP           (CANDRV_ID_RSP_BASE + CANDRV_NODE_ID)

/* ==================== 配置预期值（只用于校验，不参与初始化） ====================
 * 这两个值不会被写进任何寄存器，只在 CanDrv_Init() 里与实际读回值比对：
 * 偏离区间就返回 CANDRV_ERR_TIMING，说明 .ioc 里的位时序被改坏了。
 *
 * 当前预期：PCLK1 = 36MHz，Prescaler=16，BS1=15TQ，BS2=2TQ
 *   波特率 = 36MHz / (16 × (1+15+2)) = 125kbps
 *   采样点 = (1+15) / 18 = 88.9%
 * 换速率只改 Prescaler（25k→80 / 50k→40 / 125k→16 / 250k→8 / 500k→4 / 1M→2），这两个预期值要跟着改。
 *
 * 2026-11-25 21:59：25k → 125k，与 C8T6 侧同步回归目标速率（原因见 can.c 的 MX_CAN_Init 注释）。 */
#define CANDRV_EXPECT_BAUD        125000UL
#define CANDRV_EXPECT_SAMPLE_PM   889U      /* 采样点千分数：88.9% = 889‰ */

/* ==================== 运行模式 ====================
 * 这是驱动自己的两个语义值，**故意与 HAL 的数值不同**，由 can_drv.c::Map_HalMode()
 * 做一次显式翻译。为什么不直接沿用 HAL 的值：
 *   F1 的 CAN_MODE_LOOPBACK 是位掩码 0x40000000（BTR 的 LBKM 位），NORMAL 是 0；
 *   而 F4/F7 那套是 1/2。如果这里写死数字再强转，换系列就静默错。
 *   2026-11-24 就踩过：按 F4 写成 1/2，又把 Init.Mode 强转 uint8_t，0x40000000 截成 0，
 *   loopback 被判成 normal、回环自测被跳过 —— 代码没报错，日志只显示一个 "normal"。
 * 驱动只「读出来告诉上层」，不负责设置——设置是 .ioc 的事。 */
#define CANDRV_MODE_NORMAL      0U
#define CANDRV_MODE_LOOPBACK    1U

/* ==================== 返回值 ==================== */
#define CANDRV_OK               0
#define CANDRV_ERR_PARAM       (-1)     /* 参数非法 */
#define CANDRV_ERR_STATE       (-2)     /* HAL 状态不对（未初始化 / 已在收发） */
#define CANDRV_ERR_TX          (-3)     /* 三个邮箱都忙，帧没递进去 */
#define CANDRV_ERR_TIMEOUT     (-4)     /* 等接收超时，没收到帧 */
#define CANDRV_ERR_MISMATCH    (-5)     /* 收到了，但 ID / 长度 / 内容不符 */
#define CANDRV_ERR_TIMING      (-6)     /* 位时序与预期不符：查 .ioc 的 Prescaler/BS1/BS2 */
#define CANDRV_ERR_NO_IRQ      (-7)     /* CAN1 RX0 中断未使能：CubeMX NVIC 里没勾 */

/* ==================== 一帧 ====================
 * 只搬 8 字节标准帧。CAN 经典帧上限就是 8 字节，ISO-TP 的分包层在上层做。 */
typedef struct
{
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[8];
} can_drv_frame_t;

/* 一、初始化：校验时序 → 查中断是否已使能 → 配过滤器 → Start → 激活接收通知
 *     返回 CANDRV_OK 表示外设真进入了收发状态，且配置与预期一致。 */
int8_t CanDrv_Init(void);

/* 二、停止：只停在 HAL 层，MspDeInit 由 HAL_CAN_DeInit 负责 */
void CanDrv_DeInit(void);

/* 三、发一帧
 *     timeout_ms 只用于等「邮箱腾出空位 / 这一帧发完」，不是等对端处理。
 *     回环模式下帧立刻回来；正常模式下硬件重传由 CAN 控制器自己做。 */
int8_t CanDrv_Send(uint32_t id, const uint8_t *data, uint8_t len, uint32_t timeout_ms);

/* 四、接收队列（中断入队，主循环/协议层按需取）
 *     深度很小（8 帧）：块级半双工下总线本身不会连续涌帧，
 *     真溢出了说明协议层没跟上，此时宁可报错也不能静默丢帧。 */
uint16_t CanDrv_RxPending(void);
int8_t   CanDrv_RxPop(can_drv_frame_t *out);

/* 五、自测：发一帧已知内容，等它从 RX 回来并逐字节比对。
 *     只有回环模式才可能 PASS——正常模式下需要一个真实对端配合。 */
int8_t CanDrv_SelfTest(void);

/* 六、配置实况（从 hcan 读回，供日志打印 / 上层判断） */
uint32_t CanDrv_ActualBaudrate(void);            /* 反算出的真实波特率，bps */
uint16_t CanDrv_ActualSamplePointPermille(void); /* 反算出的采样点，千分数 */
uint8_t  CanDrv_GetMode(void);                   /* CANDRV_MODE_NORMAL / LOOPBACK */

/* 七、计数器：排查用，不参与控制逻辑 */
uint32_t CanDrv_TxOkCount(void);
uint32_t CanDrv_RxOverflowCount(void);
uint32_t CanDrv_ErrorCount(void);
uint32_t CanDrv_BusOffCount(void);

#endif /* __CAN_DRV_H */
