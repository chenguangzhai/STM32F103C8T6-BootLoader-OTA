# STM32F103C8T6 BootLoader OTA

完整的 STM32F103C8T6 两级 OTA 方案：上位机经 CAN 下发固件，下位机写入外部 W25Q64，
Boot 搬运到片内并支持失败回滚。**全链路已实机验证通过**（握手、去重、分帧下发、落盘、搬运、确认、人工回滚、崩溃自动回滚）。

## 工程结构

| 目录 | 角色 | 说明 |
|---|---|---|
| `Recieve/` | **Boot** (`0x08000000`) | 上电 3s 窗口；读更新标志搬运固件、校验、备份回滚；纯 UART 救砖通道兜底 |
| `APP/` | **App** (`0x08005000`) | 业务固件 + CAN OTA 会话层：KEY1 查询、去重、分帧接收、落盘、KEY2 回滚 |
| `host_computer_ZET6/` | **上位机** | ZET6 工程：串口收 bin → W25Q 槽 A → CAN 查询应答 → 分帧发送 |
| `Tools/` | 工具 | 串口助手 1.0/2.0（源码）、编译验证脚本、CRC 工具 |
| `OTA_CAN_升级方案.md` | 文档 | 设计方案 + **§14 实际落地实现（权威现状）** |
| `双C8T6_改造方案.md` | 文档 | 后续双 C8T6 硬件的移植方案（未实施） |
| `CHANGELOG.md` | 文档 | 逐次改动与实机验证记录 |

## OTA 流程

```
KEY1(PA0) 按下
  → CAN 查询帧 0x10（下位机 0x581 → 上位机 0x601）
  → 上位机查槽 A，回 0x11（有无 + 长度 + CRC）
  → 下位机与片内当前 App 比对 CRC，一致则去重跳过
  → 回 0x12 确认下载；上位机从 W25Q 槽 A 逐帧发 0x13（5B/帧，16 帧一块 ACK）
  → 下位机写 W25Q → 收齐写 PENDING → 复位
  → Boot 搬运到片内 → 校验 → APPLIED → App 确认 → 擦标志
```

异常分支：App 在确认前崩溃（标志保持 APPLIED），下次开机 Boot 自动从备份区恢复。
KEY2(PA1) 为人工回滚键。详见 `OTA_CAN_升级方案.md` §14。

**CAN 配置**：125 kbit/s，PSC=16，采样点 88.9%；改速率须同步 `.ioc` / `can.c` / `can_drv.h` 三处（两侧都要）。

## 快速开始

### 1. 编译与烧录

 firmware 由使用者自行编译（EIDE / CubeIDE），本仓不提供预编译 bin。
 编译验证可用脚本（只编译验证，不生成烧录文件）：

```bash
python Tools/verify_build.py APP       # 编译验证 App
python Tools/verify_build.py Recieve   # 编译验证 Boot
```

烧录地址：Boot `0x08000000`，App `0x08005000`，上位机 `0x08000000`（独立板）。
工具链在 `C:/Users/qinghan/.eide/tools/` 下（gcc_arm / st_cube_programer），非系统 PATH。

### 2. 串口助手（Windows GUI）

免装 Python 的可执行文件在 [GitHub Releases](https://github.com/chenguangzhai/STM32F103C8T6-BootLoader-OTA/releases)
下载，推荐 2.0（双串口 A/B，可同时接下位机与上位机）。源码与完整说明见 `Tools/2.0/`。

- 未做代码签名，Windows 首次运行会弹 SmartScreen，选「更多信息 → 仍要运行」。
- exe 不附带 `assist_config.json`（含本机串口号/窗口位置），换机器需重新配置。
- SWD 复位依赖调试器（DAP-Link / ST-Link）与 EIDE 自带 OpenOCD，串口 DTR 无效。

### 3. 升级操作

1. 上位机板通电，用串口助手把待升级 bin 发进外部 Flash 槽 A（带 `OTAE` 尾部，工具自动收尾算 CRC）。
2. 下位机板按 KEY1 发起查询 → 自动去重 → 下载 → 落盘 → 复位搬运。
3. 观察串口日志，`[ota] verify OK` + `[app] confirmed` 即升级成功；
   按 KEY2 可回滚到最近一次确认过的固件。

## 硬件接线（下位机板）

| 外设 | 引脚 |
|---|---|
| W25Q64 | SPI1 + PB0(CS) |
| CAN | PB8/PB9，收发器 STB(PB7) 需拉低 |
| KEY1 / KEY2 | PA0 / PA1，自锁开关按下保持低电平 |
| 串口 | 日志口 115200 |

## 已知边界

- CAN 物理层当前为飞线，125k 对边沿敏感；若出现 `LEC≠0` / REC 上涨，回退 25k（PSC=80）。
- 备份区语义是「最近一次确认过的固件」，刚升级完按 KEY2 只是重搬同一份，不能退回上一版。
- 项目无看门狗，崩溃后需人工复位，Boot 在下次开机时执行回滚。

## 许可与说明

个人项目，按仓库现状分发。所有实机验证记录见 `CHANGELOG.md`。
