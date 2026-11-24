/**
  * CAN 驱动实现 —— 设计说明见 can_drv.h
  *
  * 职责边界（2026-11-24 调整）：位时序 / 行为开关 / 模式 / NVIC / 中断处理函数
  * 全部归 CubeMX 与 .ioc；本文件只做 CubeMX 做不到的三件事——配过滤器、启动外设、
  * 收帧入队与自测，外加一层「读回校验」防止 .ioc 被改坏后静默跑歪。
  */
#include "can_drv.h"
#include "can.h"        /* hcan 句柄（CubeMX 生成；本文件是唯一碰 HAL 的地方） */
#include "board.h"      /* Board_Millis：判超时用，不阻塞 */
#include <string.h>

/* ==================== 接收队列 ====================
 * 深度取 8 且必须是 2 的幂（下标用 & 取模，比 % 快）。
 * 之所以敢这么小：块级半双工协议下，下位机发完 ACK 才允许上位机发下一块；
 * 总线不会连续涌帧。真溢出了说明协议层没跟上，此时报错比静默丢帧安全。 */
#define CANDRV_RXQ_DEPTH    8U
#define CANDRV_RXQ_MASK     (CANDRV_RXQ_DEPTH - 1U)

/* 允许的偏差：波特率 ±1%，采样点 ±3%。
 * 不写死相等是为了容忍 PCLK1 或分频方案的合理微调，
 * 但 750k vs 500k 这种量级差一定会被拦下。 */
#define CANDRV_BAUD_TOL_PM      10U     /* 10‰ = ±1% */
#define CANDRV_SAMPLE_TOL_PM    30U     /* 30‰ = ±3% */

static can_drv_frame_t   s_rxq[CANDRV_RXQ_DEPTH];
static volatile uint16_t s_rx_head;      /* 写入位置，只在中断里改 */
static volatile uint16_t s_rx_tail;      /* 取出位置，只在主循环里改 */
static volatile uint32_t s_rx_overflow;  /* 队列满 / 硬件 FIFO 溢出导致丢弃的帧数 */
static volatile uint32_t s_err_count;
static volatile uint32_t s_busoff_count;
static uint32_t          s_tx_ok;
static uint32_t          s_baud;         /* 从 hcan.Init 反算出的真实波特率 */
static uint16_t          s_sample_pm;    /* 反算出的采样点（千分数） */
static uint8_t           s_mode;         /* CANDRV_MODE_NORMAL / LOOPBACK */

/* 清空接收队列：初始化/自测前调用，避免上一次残留的帧被当成这一次的结果 */
static void Rxq_Reset(void)
{
    s_rx_head = 0U;
    s_rx_tail = 0U;
    s_rx_overflow = 0U;
}

/* 从 hcan.Init 反算实际波特率与采样点。
 * 为什么不直接信 .ioc 里填的数字：CubeMX 的位时序可能是「按目标波特率自动算」的，
 * 界面上的数值和最终写进 BTR 的 TQ 分配不一定是你以为的那个。只有从 Init 结构体
 * （也就是 HAL 真正写进寄存器的值）反算，得到的才是板上实际在跑的。
 *
 * BTR 编码约定：TS1/TS2 字段存的是「TQ 数 - 1」，所以各要 +1；
 * 一个位 = 同步段(1TQ) + BS1 + BS2。 */
static void Measure_Timing(void)
{
    uint32_t pclk = HAL_RCC_GetPCLK1Freq();
    uint32_t ts1 = (((uint32_t)hcan.Init.TimeSeg1 & CAN_BTR_TS1) >> CAN_BTR_TS1_Pos) + 1U;
    uint32_t ts2 = (((uint32_t)hcan.Init.TimeSeg2 & CAN_BTR_TS2) >> CAN_BTR_TS2_Pos) + 1U;
    uint32_t tq  = 1U + ts1 + ts2;
    uint32_t div = (uint32_t)hcan.Init.Prescaler * tq;

    s_baud = (div != 0U) ? (pclk / div) : 0U;
    s_sample_pm = (uint16_t)(((1U + ts1) * 1000U) / tq);
}

/* 把 HAL 的 Init.Mode 翻译成驱动自己的语义值。
 *
 * ⚠ 为什么不能直接强转/比较数字：F1 的 CAN_MODE_LOOPBACK 是位掩码 0x40000000
 *   （BTR 的 LBKM 位），CAN_MODE_NORMAL 是 0；F4/F7 那套才是 1/2。
 *   按位判断既能覆盖 LOOPBACK，也能覆盖 SILENT_LOOPBACK 组合，且换了 HAL 版本不会静默错。
 *   2026-11-24 的教训：曾写成 (uint8_t)hcan.Init.Mode 与 1/2 比较，0x40000000 截成 0，
 *   loopback 被判成 normal、回环自测被整段跳过——不报错，只在日志里显示一个 "normal"。 */
static uint8_t Map_HalMode(uint32_t hal_mode)
{
    if ((hal_mode & CAN_BTR_LBKM) != 0U)
    {
        return CANDRV_MODE_LOOPBACK;
    }
    return CANDRV_MODE_NORMAL;
}

/* 一、初始化 */
int8_t CanDrv_Init(void)
{
    CAN_FilterTypeDef f = {0};
    uint32_t diff;

    /* 状态必须是「初始化完成但还没启动」。MX_CAN_Init() 跑完就是 READY；
       若已经是 STARTED/LISTENING，说明被重复调用或别处动过，直接报错，
       不做隐式 Stop —— 那种"看起来成功了其实把外设踢掉"的修法更难查。 */
    if (HAL_CAN_GetState(&hcan) != HAL_CAN_STATE_READY)
    {
        return CANDRV_ERR_STATE;
    }

    /* ---- 校验 1：位时序是否仍是预期值 ----
       这是「参数归 CubeMX」方案的代价对冲：参数不在代码里，就得读回来核对。 */
    Measure_Timing();
    diff = (s_baud > CANDRV_EXPECT_BAUD) ? (s_baud - CANDRV_EXPECT_BAUD)
                                         : (CANDRV_EXPECT_BAUD - s_baud);
    if ((diff * 1000U) > (CANDRV_EXPECT_BAUD * CANDRV_BAUD_TOL_PM))
    {
        return CANDRV_ERR_TIMING;
    }
    diff = (s_sample_pm > CANDRV_EXPECT_SAMPLE_PM)
               ? (uint32_t)(s_sample_pm - CANDRV_EXPECT_SAMPLE_PM)
               : (uint32_t)(CANDRV_EXPECT_SAMPLE_PM - s_sample_pm);
    if (diff > CANDRV_SAMPLE_TOL_PM)
    {
        return CANDRV_ERR_TIMING;
    }

    /* ---- 校验 2：CAN1 RX0 中断是否已由 CubeMX 使能 ----
       CubeMX 生成的中断服务函数在 stm32f1xx_it.c（F103 中密度里该向量名为
       USB_LP_CAN1_RX0_IRQHandler，与 USB 低优先级共用 IRQn 20）。
       如果 NVIC 里没勾，FIFO 会满但回调永远不会被调用 —— 症状是"发得出去、收不回来"，
       和总线故障极像。这里提前拦下，给出明确错误码，省掉一轮瞎猜。 */
    if (NVIC_GetEnableIRQ(CAN1_RX0_IRQn) == 0U)
    {
        return CANDRV_ERR_NO_IRQ;
    }

    /* ---- 过滤器：只收 0x600 + node_id ----
       CAN 是广播总线，不过滤的话总线上别人的帧全会进 FIFO。
       32 位掩码模式，掩码 0x7FF 全 1 = ID 必须逐位相等（精确匹配）。
       下位机只需认自己那一个 ID，上位机不是被过滤的对象。 */
    f.FilterBank = 0U;
    f.FilterMode = CAN_FILTERMODE_IDMASK;
    f.FilterScale = CAN_FILTERSCALE_32BIT;
    f.FilterIdHigh = (uint16_t)(CANDRV_ID_REQ << 5U);        /* STID 位于 bit31:21 */
    f.FilterIdLow = 0x0000U;
    f.FilterMaskIdHigh = (uint16_t)(0x7FFU << 5U);           /* 11 位全比较 */
    f.FilterMaskIdLow = 0x0000U;
    f.FilterFIFOAssignment = CAN_FILTER_FIFO0;
    f.FilterActivation = ENABLE;
    f.SlaveStartFilterBank = 14U;                           /* 单 CAN 实例时无实际作用，保持 CubeMX 惯例 */

    if (HAL_CAN_ConfigFilter(&hcan, &f) != HAL_OK)
    {
        return CANDRV_ERR_STATE;
    }

    /* ---- 启动 + 激活接收通知 ----
       CubeMX 只生成初始化，不会让外设进收发态；这两步必须在代码里做。 */
    if (HAL_CAN_Start(&hcan) != HAL_OK)
    {
        return CANDRV_ERR_STATE;
    }

    /* 只开接收与错误类中断。TX 完成中断不需要：CanDrv_Send 用轮询等邮箱空，
       少一个中断就少一处并发问题。 */
    if (HAL_CAN_ActivateNotification(&hcan,
                                     CAN_IT_RX_FIFO0_MSG_PENDING |
                                     CAN_IT_ERROR |
                                     CAN_IT_BUSOFF |
                                     CAN_IT_ERROR_PASSIVE) != HAL_OK)
    {
        return CANDRV_ERR_STATE;
    }

    /* 计数器在"外设真的起来了"之后才清，避免初始化失败时把历史故障信息抹掉 */
    Rxq_Reset();
    s_err_count = 0U;
    s_busoff_count = 0U;
    s_tx_ok = 0U;
    s_mode = Map_HalMode((uint32_t)hcan.Init.Mode);

    return CANDRV_OK;
}

/* 二、停止 */
void CanDrv_DeInit(void)
{
    if (HAL_CAN_GetState(&hcan) == HAL_CAN_STATE_READY)
    {
        (void)HAL_CAN_Stop(&hcan);
    }
    /* 不在这里关 NVIC：NVIC 归 CubeMX 管，关掉会让下次 Init 前的中断状态变得难解释 */
    (void)HAL_CAN_DeInit(&hcan);
}

/* 三、发一帧 */
int8_t CanDrv_Send(uint32_t id, const uint8_t *data, uint8_t len, uint32_t timeout_ms)
{
    CAN_TxHeaderTypeDef tx = {0};
    uint32_t mb = CAN_TX_MAILBOX0;
    uint32_t t0;
    uint8_t  tmp[8] = {0};
    HAL_CAN_StateTypeDef st;

    if ((id > 0x7FFU) || (len > 8U) || ((len > 0U) && (data == NULL)))
    {
        return CANDRV_ERR_PARAM;
    }

    st = HAL_CAN_GetState(&hcan);
    if ((st != HAL_CAN_STATE_READY) && (st != HAL_CAN_STATE_LISTENING))
    {
        return CANDRV_ERR_STATE;
    }

    /* 三个邮箱都占着就等一会儿。块级半双工下正常不会堵，真堵住说明上层
       没等 ACK 就狂发，等超时直接报错，不在这里死等。 */
    t0 = Board_Millis();
    while (HAL_CAN_GetTxMailboxesFreeLevel(&hcan) == 0U)
    {
        if ((Board_Millis() - t0) >= timeout_ms)
        {
            return CANDRV_ERR_TX;
        }
    }

    /* ⚠ 为什么先拷进 8 字节临时缓冲：
       F1 HAL 的 HAL_CAN_AddTxMessage 会**无视 DLC** 直接读 aData[0..7] 填满
       TDLR/TDHR。若调用方传的是 2 字节数组，就是读越界（栈上通常不出事，
       但正好贴着段边界时会 HardFault）。先补零拷进本地的 8 字节最省心。 */
    if (len > 0U)
    {
        memcpy(tmp, data, len);
    }

    tx.StdId = id;
    tx.ExtId = 0U;
    tx.IDE = CAN_ID_STD;
    tx.RTR = CAN_RTR_DATA;
    tx.DLC = len;
    tx.TransmitGlobalTime = DISABLE;

    if (HAL_CAN_AddTxMessage(&hcan, &tx, tmp, &mb) != HAL_OK)
    {
        return CANDRV_ERR_TX;
    }

    /* 等这一帧真的发完（对应邮箱空）。区别很重要：
       "递进邮箱"在 BusOff 时会返回成功但帧永远出不去，
       只有邮箱空了才说明它带着 ACK 走完了总线。 */
    t0 = Board_Millis();
    while (HAL_CAN_IsTxMessagePending(&hcan, mb) != 0U)
    {
        if ((Board_Millis() - t0) >= timeout_ms)
        {
            return CANDRV_ERR_TIMEOUT;
        }
    }

    s_tx_ok++;
    return CANDRV_OK;
}

/* 四、接收队列 */
uint16_t CanDrv_RxPending(void)
{
    return (uint16_t)((s_rx_head - s_rx_tail) & CANDRV_RXQ_MASK);
}

int8_t CanDrv_RxPop(can_drv_frame_t *out)
{
    if (out == NULL)
    {
        return CANDRV_ERR_PARAM;
    }
    if (s_rx_head == s_rx_tail)
    {
        return CANDRV_ERR_TIMEOUT;
    }
    *out = s_rxq[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1U) & CANDRV_RXQ_MASK);
    return CANDRV_OK;
}

/* 五、回环自测
 * 发一帧已知内容，等它从 RX 回来并逐字节比对。
 * 回环模式下帧由 CAN 内核内部环回，不经过引脚、不需要收发器、不需要外部 ACK，
 * 所以这条路径能证明：时序配置合法、过滤器放行、中断已接、回调被调、队列搬运正确
 * —— 阶段 1a 的全部验收内容。 */
int8_t CanDrv_SelfTest(void)
{
    static const uint8_t kPattern[8] = {0xA5U, 0x5AU, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U};
    can_drv_frame_t rx;
    uint32_t t0;
    int8_t   r;
    uint8_t  i;

    /* 正常模式没有对端：发出去也没人 ACK，硬测只会得到一个误导性的 FAIL */
    if (s_mode != CANDRV_MODE_LOOPBACK)
    {
        return CANDRV_ERR_STATE;
    }

    /* 先排掉可能残留的帧，否则"收到一帧"可能是上一次留下的假阳性 */
    while (CanDrv_RxPop(&rx) == CANDRV_OK)
    {
    }

    r = CanDrv_Send(CANDRV_ID_REQ, kPattern, (uint8_t)sizeof(kPattern), 100U);
    if (r != CANDRV_OK)
    {
        return r;
    }

    /* 回环帧几乎是立刻回来的，100ms 已是极宽的余量 */
    t0 = Board_Millis();
    while (CanDrv_RxPending() == 0U)
    {
        if ((Board_Millis() - t0) >= 100U)
        {
            return CANDRV_ERR_TIMEOUT;
        }
    }

    if (CanDrv_RxPop(&rx) != CANDRV_OK)
    {
        return CANDRV_ERR_TIMEOUT;
    }

    if ((rx.id != CANDRV_ID_REQ) || (rx.dlc != (uint8_t)sizeof(kPattern)))
    {
        return CANDRV_ERR_MISMATCH;
    }
    for (i = 0U; i < (uint8_t)sizeof(kPattern); i++)
    {
        if (rx.data[i] != kPattern[i])
        {
            return CANDRV_ERR_MISMATCH;
        }
    }

    return CANDRV_OK;
}

/* 六、配置实况 */
uint32_t CanDrv_ActualBaudrate(void)            { return s_baud; }
uint16_t CanDrv_ActualSamplePointPermille(void) { return s_sample_pm; }
uint8_t  CanDrv_GetMode(void)                   { return s_mode; }

/* 七、计数器 */
uint32_t CanDrv_TxOkCount(void)       { return s_tx_ok; }
uint32_t CanDrv_RxOverflowCount(void) { return s_rx_overflow; }
uint32_t CanDrv_ErrorCount(void)      { return s_err_count; }
uint32_t CanDrv_BusOffCount(void)     { return s_busoff_count; }

/* ==================== HAL 回调 ====================
 * 这些是 HAL 里的 __weak 空实现，在此覆盖即为 "钩子已接上" 的证明。
 * 注意：中断入口函数不在这里 —— 它由 CubeMX 生成在 stm32f1xx_it.c 里
 * （USB_LP_CAN1_RX0_IRQHandler / CAN1_SCE_IRQHandler），本文件只接回调。 */

/* 收到一帧：取出并入队。队列满时丢弃新帧并计数 —— ISO-TP 的连续帧序号
   会暴露缺口，让协议层走重传，比悄悄覆盖旧帧、看起来一切正常要安全得多。 */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan_p)
{
    CAN_RxHeaderTypeDef hdr;
    uint8_t d[8];
    uint16_t next;

    if (HAL_CAN_GetRxMessage(hcan_p, CAN_RX_FIFO0, &hdr, d) != HAL_OK)
    {
        return;
    }
    if (hdr.IDE != CAN_ID_STD)
    {
        return;     /* 只处理标准帧，扩展帧本设计不用 */
    }

    next = (uint16_t)((s_rx_head + 1U) & CANDRV_RXQ_MASK);
    if (next == s_rx_tail)
    {
        s_rx_overflow++;
        return;
    }

    s_rxq[s_rx_head].id = hdr.StdId;
    s_rxq[s_rx_head].dlc = (uint8_t)hdr.DLC;
    memcpy(s_rxq[s_rx_head].data, d, sizeof(s_rxq[s_rx_head].data));
    s_rx_head = next;
}

/* 错误/BusOff：ABOM 打开后硬件自己会从 BusOff 恢复（128 个连续隐性位后），
   所以这里只统计 + 清错误码，不强行 HAL_CAN_Start 重入，避免和硬件抢着恢复。
   若将来把 .ioc 里的 ABOM 关掉，这里必须补上重启动，否则等于永久失联。 */
void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan_p)
{
    /* 宏名注意：F1 的 HAL 叫 HAL_CAN_ERROR_BOF（不是 F4 那套 HAL_CAN_ERROR_BUSOFF），
       写错名字编译直接报 undeclared。 */
    if ((hcan_p->ErrorCode & HAL_CAN_ERROR_BOF) != 0U)
    {
        s_busoff_count++;
    }
    /* RX_FOV0 = 硬件 FIFO 溢出。这是方案 §7 最怕的情况（擦写期间总线还在涌帧），
       单独计数便于将来验证「块级半双工」是否真的把空窗期留出来了。 */
    if ((hcan_p->ErrorCode & HAL_CAN_ERROR_RX_FOV0) != 0U)
    {
        s_rx_overflow++;
    }
    s_err_count++;
    hcan_p->ErrorCode = HAL_CAN_ERROR_NONE;
}
