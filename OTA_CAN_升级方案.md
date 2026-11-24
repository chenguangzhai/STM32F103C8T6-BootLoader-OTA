# OTA 升级方案（串口信令 + 按键确认 + CAN 传输）

> 状态：阶段 1a **已实机验证通过**（CAN 驱动 + 回环自测 PASS），1b 及之后未开始；
> 阶段 4（Boot 侧 CAN）**已取消**，Boot 回到纯 UART 救砖通道
> 日期：2026-11-23 立项 / 2026-11-24 更新
> 前置版本：已完成两级 OTA 闭环（UART 收 bin → W25Q64 → 标志 → Boot 搬运 → 确认制 → 备份回滚），全部实机验证通过

---

## 1. 背景

现有 OTA 链路是「PC 串口一路灌 bin → APP 攒 4KB 扇区写 W25Q64 → 回读逐字节校验 → 算 CRC32 → 写 PENDING 标志 → 复位 → Boot 搬运到片内 → 校验 → APPLIED → APP 确认 → 失败回滚」。

功能闭环已经验证，但**传输层没有可靠性保障**：

- `Boot_Loader_Getlen()` 是盲收，不认帧边界，靠「2 秒没数据」判断发完
- `Uart_Read()` 的 1KB 环形缓冲在 115200 下约 87ms 就被灌满
- 只有 `OtaW25q_Finish()` 才整体回读 + 校验 CRC，**一次丢字节 = 整包重传**

现在能成功，依赖「PC 端连续发文件 + 主循环恰好跟得上」这个巧合。固件涨到 40KB、波特率提到 460800，或换一根质量差的 USB 转串口线，就会开始随机失败。

**因此传输层改走 CAN**：CAN 硬件提供 CRC15、接收应答、自动重传，单帧完整性由硬件保证，软件只需要管分包重组和流控。

---

## 2. 方案总览

核心思路是**控制与数据分离 + 物理在场确认**：

| 环节 | 通道 | 理由 |
|---|---|---|
| 上位机获取待升级 bin | 串口 | 按现场工具链现状 |
| 发起升级请求 | 串口 | 点对点，不会被总线上其它节点误收 |
| 下位机响应提示 | LED + 串口日志 | 操作者可见 |
| **操作者按键确认** | KEY2 (PA1) | 物理在场证明，杜绝远程/误刷 |
| 授权回执 | CAN | 告知上位机可以开始 |
| 固件数据传输 | CAN | 硬件 CRC + 自动重传 + 逐块 ACK |
| 落盘 / 校验 / 标志 / 搬运 / 回滚 | 内部 | **复用现有全部逻辑，一行不改** |

### 升级时序

```
上位机                下位机                    用户
  │                     │                       │
  │──串口：请求升级────>│                       │
  │                     │──LED 快闪+串口提示──>│
  │                     │<────按下 KEY2─────────│
  │<──CAN：READY────────│                       │
  │──CAN：块 1..N──────>│                       │
  │    (逐块 ACK)       │                       │
  │<──CAN：DONE─────────│                       │
```

### 关键设计点

1. **按键是最后一道门**。CAN 帧可被总线上任何设备伪造，但没人按键就进不了下载态。
2. **串口继续当日志通道**。升级全程通过串口打印块号进度、CRC 结果、错误码，CAN 只走数据。现在定位问题全靠串口日志，不能为了「纯 CAN」砍掉。
3. **后端完全复用**。`OtaW25q_Feed(buf, len)` 的接口不变 —— 分包层把数据拼回连续字节流后照原样喂进去。

---

## 3. 目标架构

```
python-can (USB-CAN 分析仪)
      │  8 字节 / 帧
      ▼
CAN 驱动 + 过滤器 + 中断接收        ← 新增
      │
      ▼
ISO-TP 分包重组 (SF / FF / CF / FC) ← 新增
      │  连续字节流
      ▼
会话层状态机 + 块级握手             ← 新增
      │
      ▼
OtaW25q_Feed / Finish               ← 已存在，不改
      │
      ▼
W25Q64 (0x100000) + 回读校验 + CRC32 ← 已存在，不改
      │
      ▼
OtaFlag_Write(PENDING)              ← 已存在，不改
      │
      ▼
复位 → Boot 搬运 → APPLIED → APP 确认 / 回滚  ← 已存在，不改
```

新增模块建议放置：

| 文件 | 职责 |
|---|---|
| `APP/Application/BSP/CAN/can_drv.c/.h` | 配置、过滤器、收发一帧、BusOff 恢复 |
| `APP/Application/BSP/OTA/isotp.c/.h` | ISO-TP 分包重组与流控 |
| `APP/Application/BSP/OTA/ota_session.c/.h` | 会话状态机、块级握手、超时 |

---

## 4. 状态机

```
IDLE ──串口请求──> ARMED ──按键──> READY ──首块──> DOWNLOAD ──收完──> VERIFY
  ↑                  │30s 超时      │10s 超时                        │
  └──────────────────┴──────────────┴────────────────────────────────┘
                              任何失败/超时回 IDLE
```

| 状态 | 行为 | 退出条件 |
|---|---|---|
| `IDLE` | 正常业务运行，接收串口信令 | 收到 `REQ_UPDATE`（且节点号匹配） |
| `ARMED` | LED 快闪，串口提示「请按键确认」，**不接受 CAN 数据** | 按键 → `READY`；30s 无动作 → `IDLE` |
| `READY` | CAN 广播 `READY` 帧，等待上位机 `RequestDownload` | 收到 `0x34` → `DOWNLOAD`；10s 超时 → `IDLE` |
| `DOWNLOAD` | 逐块接收、落盘、回 ACK | 全部块收完 → `VERIFY`；块超时/校验错 → `IDLE` |
| `VERIFY` | 整体回读比对 + CRC32 | 通过 → 写 PENDING 标志 → 复位；失败 → `IDLE` |

**防重入**：非 `IDLE` 状态下再收到 `REQ_UPDATE`，直接回 `BUSY`，不允许两个会话交叉。

---

## 5. 协议定义

### 5.0 串口信令（阶段 2 设计草案，**未实施**）

> 2026-11-24：阶段 2 曾写出一版实现，因「阶段 1a 的回环自测尚未在硬件上验证通过」被整体回退。
> 本节保留为设计参考，等 1a 实机 PASS 后再按此实现。

ASCII 行，`\n` 结尾（`\r` 忽略），整行 ≤ 32 字节。选 ASCII 而不是二进制，是因为现场手边
常常只有串口助手 —— 能敲字比能发十六进制重要。

| 命令 | 作用 | 正常响应 |
|---|---|---|
| `+OTA:REQ` | 请求进入升级 | `[sess] REQ accepted -> ARMED, press KEY2 within 30000ms` |
| `+OTA:CANCEL` | 取消当前会话 | `[sess] CANCEL -> IDLE` |

非 IDLE 时再发 `REQ` → `[sess] BUSY (state=n), REQ ignored`（防两个会话交叉写 Flash）。

**和 bin 字节流共用 USART1 怎么区分**：靠前 5 字节 `+OTA:`。bin 开头固定是 `00 50 00 20`
（SP = 0x20005000），不会撞。实现上用 `Uart_Peek()` 只看不取——判头不是命令的字节原样留给
bin 落盘模块，**现有 UART 升级路径一个字节都没动**。

> 已知限制：bin 传输过程中不要发信令。会话层不知道 `Boot_Loader` 是否正在传输，此时发 `REQ`
> 会让 bin 路径停止被调用，传输卡在半截。后果是 W25Q 里留一份无标志的残包（不会搬运、
> 不影响启动），重新发一次即可。

### 5.1 CAN 报文 ID（11 位标准帧）

沿用 UDS 惯例，便于以后接标准诊断仪：

| 方向 | ID | 说明 |
|---|---|---|
| 上位机 → 下位机 | `0x600 + node_id` | 命令 / 数据 |
| 下位机 → 上位机 | `0x580 + node_id` | 响应 / ACK |

`node_id` 占用低 4 位，单机时取 1。**这是必须的**：CAN 是广播，串口是点对点，不带节点号的话多台设备同时在线时会全部响应同一个请求。

### 5.2 ISO-TP 帧格式（每帧 8 字节）

| 类型 | 首字节 | 格式 | 用途 |
|---|---|---|---|
| SF 单帧 | `0n` | `0n` + n 字节数据 | 会话信令（≤7 字节） |
| FF 首帧 | `1L LL` | 12 位长度 + 6 字节数据 | 块的第一帧 |
| CF 连续帧 | `2n` | 序号 0~15 循环 + 7 字节数据 | 块的后续帧 |
| FC 流控 | `3s BB TT` | s: 0=继续 1=等待 2=溢出；BB 块大小；TT 最小间隔 | 控制发送节奏 |

CF 的 4 位序号用于检测漏帧，正好替代自定义帧号。

### 5.3 会话信令（SF 承载，UDS 风格 SID）

| SID | 名称 | 请求载荷 | 正响应 |
|---|---|---|---|
| `0x34` | RequestDownload | `fw_len(4) + fw_crc32(4) + version(4) + hw_id(2)` | `0x74` + `block_size(2)` |
| `0x36` | TransferData | `block_seq(2) + data(≤1024)` | `0x76` + `block_seq(2)` |
| `0x37` | TransferExit | `fw_crc32(4)` | `0x77` + 校验结果 |
| `0x7F` | 负响应 | `0x7F + SID + NRC` | — |

常见 NRC：`0x13` 长度错、`0x22` 条件不满足（未授权）、`0x31` 空间不足、`0x71` 传输中断、`0x78` 响应挂起。

`RequestDownload` 阶段就校验 `hw_id` 和 `fw_len`，**板型不对直接拒收**，不用等刷进去跑飞再回滚。

### 5.4 块大小与节拍

取 **1024 字节/块**：

- 1024 < 4095，ISO-TP 的 12 位长度字段装得下，不需要分多次 TransferData
- 每块 = 1 个 FF + 146 个 CF，1Mbps 下约 **16ms**
- 每 4 块（4096 字节）触发一次 W25Q 扇区擦写，约 **45ms**
- 块间必须等 ACK，这个空窗期正好容纳擦写

**这是整个方案最关键的约束**，详见 §7。

17KB 固件 ≈ 17 块、4 次擦除，理论总时长约 **0.5 秒**（1Mbps）。

---

## 6. CAN 配置修正（硬前置）

> **当前配置不可用。** `APP/Core/Src/can.c` 与 `Recieve/Core/Src/can.c` 完全一致，均为 CubeMX 默认值，从未验证过。

### 6.1 现状问题

PCLK1 = 36MHz，当前 `Prescaler=16 / BS1=1TQ / BS2=1TQ`：

```
波特率 = 36MHz / 16 / (1+1+1) = 750 kbps
采样点 = (1+1)/3 = 66.7%
```

| 问题 | 后果 |
|---|---|
| 750k 非常规速率（125/250/500/1M） | 总线上没有第二个设备会认 |
| 采样点 66.7%（CiA 推荐 87.5%） | 容差极窄，易误码 |
| BS1 仅 1TQ | 重同步段几乎为零，线稍长即错 |

三条参数对 OTA 明确有害：

| 参数 | 当前 | 必须改为 | 不改的后果 |
|---|---|---|---|
| `AutoRetransmission` | DISABLE | **ENABLE** | 发帧丢失不重发，固件静默残缺 |
| `AutoBusOff` | DISABLE | **ENABLE** | 一次总线错误永久失联，只能手动复位 |
| `ReceiveFifoLocked` | DISABLE | **ENABLE** | FIFO 满时静默覆盖旧帧，最难查的丢帧 |

此外 `main.c` 只调了 `MX_CAN_Init()`，**没有 `HAL_CAN_ConfigFilter`、没有 `HAL_CAN_Start`、没有接收中断** —— 外设从未进入收发状态。

### 6.2 正确分频

BS1/BS2 固定，只改 Prescaler 即可切换速率：

| 速率 | Prescaler | BS1 | BS2 | SJW | 采样点 |
|---|---|---|---|---|---|
| 250 kbps | 8 | 15TQ | 2TQ | 1TQ | 88.9% |
| **500 kbps（推荐）** | 4 | 15TQ | 2TQ | 1TQ | 88.9% |
| 1 Mbps | 2 | 15TQ | 2TQ | 1TQ | 88.9% |

验算：`36MHz / (4 × 18) = 500 kbps`。

### 6.3 初始化顺序与职责划分

```c
MX_CAN_Init();                          // CubeMX 生成：分频/时段/三开关/模式
HAL_CAN_ConfigFilter(&hcan, &filter);   // 手写：只收 0x600+node_id（F1 的 CubeMX 无过滤器界面）
HAL_CAN_Start(&hcan);                   // 手写：CubeMX 只生成初始化，不会让外设进收发态
HAL_CAN_ActivateNotification(&hcan, CAN_IT_RX_FIFO0_MSG_PENDING);  // 手写
```

**2026-11-24 定稿的职责边界**：位时序、`ABOM/ARD/RFL` 三开关、工作模式、GPIO/AFIO 重映射、
NVIC 使能、中断处理函数（`stm32f1xx_it.c` 里的 `USB_LP_CAN1_RX0_IRQHandler` / `CAN1_SCE_IRQHandler`）
**全部由 CubeMX 与 `.ioc` 负责**；`can_drv.c` 只补 CubeMX 做不到的三件事——过滤器、Start、
收帧入队——外加一层**读回校验**：从 `hcan.Init` 反算真实波特率与采样点，偏离 500kbps/88.9%
就返回错误码 `-6` 并把实际值打到串口。

这样做的好处是参数只有 `.ioc` 一个来源，重新生成代码不会和代码里的数值打架；
代价是「CubeMX 里点错了要靠运行时校验来暴露」，而不是靠编译期。

> 陷阱：F103 中密度把 USB 低优先级与 CAN1 RX0 复用同一个向量（IRQn 20），
> 生成的函数名是 `USB_LP_CAN1_RX0_IRQHandler`。手写成 `CAN1_RX0_IRQHandler` 时
> **链接不报错**（它只是个没人引用的普通函数），而真正的中断掉进 `Default_Handler`
> 的 `while(1)`，表现为「初始化成功但永远收不到帧」。

`AutoBusOff` 打开后还必须处理 BusOff 恢复（检测 `CAN_IT_ERROR` → `HAL_CAN_Start()` 重入），否则等于没开。

### 6.4 收发器使能（原方案遗漏，2026-11-24 补）

`gpio.c` 把 `CAN_STB`(PB7) 配成推挽输出，**上电初值是 `GPIO_PIN_SET`（高）**。
若它是收发器待机脚（TJA1050 的 S / SN65HVD230 的 Rs 一类），意味着正常总线模式下
收发器一直处于待机，帧根本出不去——表现为「配置全对、示波器上 TX 无波形」。

- 回环测试不受影响：环回在 CAN 内核内部完成，不经过引脚。
- 切正常模式前必须把 PB7 拉低（`HAL_GPIO_WritePin(CAN_STB_GPIO_Port, CAN_STB_Pin, GPIO_PIN_RESET)`），
  并确认板上是否有硬件下拉；没有就在 `CanDrv_Init()` 里显式拉低。

### 6.5 CubeMX 配置清单（阶段 1a 用，照此勾选）

| 位置 | 项 | 值 |
|---|---|---|
| Connectivity → CAN | Mode | **Loopback**（1a 自测用；1b 改回 Normal） |
| CAN → Bit Timings | Prescaler | **4** |
| 同上 | Time Quanta in Bit Segment 1 | **15 TQ** |
| 同上 | Time Quanta in Bit Segment 2 | **2 TQ** |
| 同上 | ReSynchronization Jump Width | **1 TQ** |
| 同上 | Baud Rate / Sample Point（核对显示） | 500000 / **88.9%** |
| CAN → Advanced | Auto Bus Off | **Enable** |
| 同上 | Auto Retransmission | **Enable** |
| 同上 | Receive Fifo Locked Mode | **Enable** |
| 同上 | Time Triggered / Auto Wake Up / Tx Fifo Priority | Disable |
| NVIC | CAN1 RX0 interrupt | **勾选**（生成 `USB_LP_CAN1_RX0_IRQHandler`） |
| NVIC | CAN1 SCE interrupt | **勾选**（生成 `CAN1_SCE_IRQHandler`） |
| GPIO | PB7 / CAN_STB | 推挽输出；1b 时初值改 **Low** |

生成后**必须回头核对 `can.c`**：`Prescaler=4`、`TimeSeg1=CAN_BS1_15TQ`、`TimeSeg2=CAN_BS2_2TQ`、
三个开关为 `ENABLE`。如果 CubeMX 的「自动算时序」把 BS1/BS2 算成了别的分配（例如 14/3），
要么手填改回来，要么接受它——因为 `CanDrv_Init()` 会把**实际**波特率与采样点打到串口，
并在偏离 500kbps/88.9% 时返回 `-6`，不会静默跑歪。

---

## 7. 最关键的约束：擦写期间 FIFO 溢出

W25Q64 擦一个 4KB 扇区典型 **45ms**（最坏 400ms），这期间 STM32F103 单 bank 停 CPU 取指，但 **CAN1 的 FIFO0/FIFO1 各只有 3 级深度**。

1Mbps 下 45ms 会涌进约 **400 帧**，硬件装不下，必然溢出丢帧。

串口方案里这个问题被 1KB 环形缓冲和主循环速度掩盖了，**CAN 上躲不掉**。所以协议必须做成**块级半双工**：

- 上位机发完一块 → 停下等 ACK
- 下位机收齐 1024 字节 → 必要时擦扇区、写 W25Q、回读 → 回 ACK
- 上位机收到 ACK 才发下一块

这样擦写空窗期总线上没有数据，FIFO 不会溢出。代价是吞吐略降，但换来确定性。

**断点续传是这个设计的副产品**：断电重启后，上位机先查「上一块号是多少」，从下一块接着发即可，不必重传整包。

---

## 8. 与现有代码的衔接

| 现有接口 | 是否改动 | 说明 |
|---|---|---|
| `OtaW25q_Init / Feed / Finish` | **不改** | 分包层拼回连续流后照原样喂 |
| `OtaW25q_Total / BadOffset` | 不改 | 进度与错误定位继续可用 |
| `OtaFlag_Write(len, crc, PENDING)` | 不改 | 只是数据来源从 UART 换成 CAN |
| `OtaBackup_*` / `OtaApply_*` | 不改 | Boot 侧逻辑与传输通道无关 |
| `OtaUpdateFlag_t` | **扩字段** | 增加 `version`、`hw_id`，`self_crc` 覆盖范围同步扩展 |
| `Boot_Loader_Getlen()`（UART 盲收） | 保留 | 降级为调试/救砖通道 |

**`OtaFlag_*` 两侧必须同步修改**：`APP/Application/BSP/OTA/ota_flag.h` 与 `Recieve/Application/BSP/OTA/ota_flag.h` 逐字一致，改一边不改另一边，症状只是「永远不搬运」，不报错、极难查。

---

## 9. 已知风险与对策

| 风险 | 对策 |
|---|---|
| **App 起不来时 Boot 收不到 CAN** | **2026-11-24 定稿：Boot 不接 CAN**，只保留已验证的 UART 救砖通道（`Boot_Loader_BootWindow()` 的 3 秒窗口）。理由：传输通道与搬运逻辑本就解耦，`OtaW25q_Feed()` 不关心字节来自哪；为「App 坏了」这一窄场景再维护一套裁剪版协议栈，代价（Flash + 双实现差异）大于收益。**前置条件：现场串口可达**；若设备封死只能引出 CAN，此条须重新评估 |
| **CAN 广播导致多节点误响应** | 协议带 `node_id`，串口请求与 CAN ID 双重过滤 |
| **擦写期间 FIFO 溢出** | 块级半双工握手，见 §7 |
| **升级中途断电** | 块级 ACK 天然支持续传；标志未写则 Boot 不搬运，回到旧固件 |
| **状态机永久挂起** | ARMED 30s / READY 10s / 块间超时，超时回 IDLE |
| **两路会话交叉** | 非 IDLE 状态收到新请求回 `BUSY` |
| **备份区写一半断电** | 备份已有 header + CRC 校验，损坏则拒绝回滚并保持现状 |
| **Boot 区 Flash 余量** | 2026-11-24 移除 Boot 的 CAN 后：**18460B/20KB = 90.14%，余 2020B**（移除前 19684B/96.11%，余 796B；净省 **1224B**）。即使将来要恢复 Boot-CAN，也只有 2020B 可用，`can_drv.c` 现版 1116B text 勉强够但无余量 → 若真要做得先裁剪 |

---

## 10. 分期实施

| 阶段 | 内容 | 验收标准 |
|---|---|---|
| **1** | 修 CAN 配置：分频、重传、BusOff、过滤器、Start、中断 | 总线上能和 PC 稳定收发一帧 |
| **2** | 串口请求 + 状态机 + 按键确认 | 串口发请求 → LED 闪 → 按键 → 串口打印 READY |
| **3** | ISO-TP 分包 + 会话层 + 接 `OtaW25q` | 完整传一个 bin，PENDING 标志写成功，Boot 正常搬运 |
| ~~4~~ | ~~Boot 侧 CAN 并行入口~~ **已取消（2026-11-24）** | 原计划：App 空片时也能通过 CAN 完成首次烧录。<br>取消理由见 §9：传输通道与搬运逻辑解耦，Boot 不需要知道字节来源；<br>该场景由已验证的 UART 救砖通道覆盖。前提是**现场串口可达**。<br>Boot 侧 CAN 配置与 `can.c/can.h` 已全部移除，节省 1224B Flash |

阶段 1 是硬前置，当前配置什么都做不了。总工期从 4 期减为 3 期。

---

## 11. 待确认（2026-11-24 已拍板 3 项）

1. **CAN 是否只用于升级**：暂按「只用于升级」推进，OTA 走独立 ID 段
   （`0x600/0x580 + node_id`），业务通信若以后也上 CAN 再评估总线负载。
2. **波特率 → 500 kbps**（`Prescaler=4`，采样点 88.9%）。250k/1M 只改分频即可切换。
3. **USB-CAN 分析仪**：暂无。阶段 1 先用 `CAN_MODE_LOOPBACK` 自测，
   有硬件后再走真实总线；型号确定后选 `python-can` 后端（PCAN / 周立功 / 创芯 / slcan）。
4. **节点数 → 单机**（`node_id=1`）。注意：下位机的硬件过滤器只需认自己的 `0x601`，
   上位机不是被过滤对象，所以用精确匹配而非掩码范围匹配。
5. **KEY2(PA1) 复用**：仍被 `OTA_TEST_CRASH` 测试桩占用，阶段 2 开工前必须挪走。

---

## 12. 实施进展

| 日期 | 阶段 | 内容 | 状态 |
|---|---|---|---|
| 2026-11-24 | **1a ✅** | CAN 驱动 `BSP/CAN/can_drv.c/.h`：过滤器、Start、收帧队列、**读回校验**、回环自测；参数与 NVIC/中断函数归 CubeMX | **实机 PASS**：`[can] init OK, 500000 bps, sample 88.8%, loopback` + `[can] loopback self-test PASS (tx=1 ovf=0 err=0 bof=0)`（固件 20628B / crc 0xC3FBDE7B）。<br>已证明：时序被硬件接受、发帧完成、进 FIFO0、中断与向量正确、回调入队、8 字节内容一致。<br>**未证明**：真实总线物理层（PB8/PB9 复用、收发器 STB、120Ω 终端、对端 ACK），以及「过滤器会挡住别的 ID」 |
| 2026-11-24 | ~~4~~ | **移除 Boot 侧 CAN**：删 `Recieve/Core/Src/can.c`、`can.h`、`MX_CAN_Init()` 调用、`it.c/it.h` 两处中断、`.ioc` 的 CAN 与 PB8/PB9 复用；Boot 回到纯 UART 通道 | 完成：编译 36 文件零错误零警告，FLASH **18460B/20KB = 90.14%**（省 1224B），镜像内无 CAN 代码，IRQn 20/22 落回 `Default_Handler` |
| — | 1b | 真实总线收发 | 待分析仪到位 + PB7 拉低 |
| — | 2 | 串口请求 + 状态机 + 按键确认 | **未开始**。2026-11-24 曾实现一版，因阶段 1a 未实机验证先行回退（`ota_session.*` 已删除、`uart_app`/`board` 已还原），§5.0 保留协议设计 |
| — | 3 | ISO-TP + 会话层 + 接 `OtaW25q` | 未开始 |
| — | 4 | Boot 侧 CAN 并行入口 | 未开始 |

**阶段 1a 期间发现的两个坑（都已处理，记录备查）**：

1. F103 中密度的 CAN RX0 中断向量名为 `USB_LP_CAN1_RX0_IRQHandler`（与 USB 低优先级共用，
   IRQn 20），**不是** `CAN1_RX0_IRQHandler`。写错名字链接不报错——它只是个没人引用的
   普通函数，而真正的中断掉进 `Default_Handler` 的 `while(1)`，表现为「初始化成功但永远收不到帧」。
2. F1 HAL 的 BusOff 错误码是 `HAL_CAN_ERROR_BOF`，不是 F4 那套 `HAL_CAN_ERROR_BUSOFF`。
3. **源码注释里不能写出 CubeMX 标记原文**（段名 + BEGIN/END 的组合）。`Recieve/main.c` 曾有
   一句注释直接引用了「2 号用户代码段开始标记」，CubeMX 按字符串找标记时先命中注释，
   于是把 Includes 尾部到真正结束标记之间整段当成用户代码重新插入 —— 生成出两个 `int main()`，
   工程编译不过，且**每次生成都逐字节复现**。提到标记时务必断开写（如「2 号用户代码段」）。

---

## 附：现有分区与地址布局（不变）

**片内 Flash（64KB）**

| 区域 | 地址 | 大小 |
|---|---|---|
| Boot | `0x08000000` | 20KB |
| App | `0x08005000` | 44KB |

**W25Q64（8MB）**

| 区域 | 地址 | 用途 |
|---|---|---|
| `0x000000` | 4KB | 更新标志（独占扇区） |
| `0x001000` ~ `0x0FFFFF` | — | 保留 |
| `0x100000` ~ `0x1FFFFF` | 1MB | 新固件区 |
| `0x200000` ~ `0x2FFFFF` | 1MB | 备份区（数据从 `0x201000` 起） |
| `0x7FF000` | 4KB | W25Q 自检扇区 |
