# 变更日志

## 2026-11-22 13:52 — 在 Recieve 工程新增 W25Q64 SPI Flash 驱动

### 问题（如有）

Recieve 工程需要使用 W25Q64（8 MB）Flash，但只有 Boot_Loader 侧针对 W25Q128（16 MB）的驱动副本，
且两个工程相互无法引用源文件，需在 Recieve 侧建立一份独立驱动。

### 改动

- `Recieve/Application/BSP/W25Q/w25q.h` — 新建。从 Boot_Loader 的 w25q.h 移植并按 W25Q64 调整器件参数：
  W25Q_SIZE 由 16 MB 改为 8 MB、JEDEC ID 由 0xEF4018 改为 0xEF4017；接线说明改为 Recieve 的
  SPI1(PA5/6/7)+PB0 片选；去掉"两份文件须同步"的约定。API 与源版一致，调用方传 &hspi1、
  W25Q_CS_GPIO_Port、W25Q_CS_Pin 即可。
- `Recieve/Application/BSP/W25Q/w25q.c` — 新建。从 Boot_Loader 的 w25q.c 复制，通信逻辑逐字保留
  （W25Q 全系列命令集相同），仅文件头注释注明来源、差异与副作用；边界检查因引用 W25Q_SIZE 宏
  自动适配 8 MB 容量上限。
- 说明：EIDE 的 srcDirs 已包含 Application 目录，新驱动会被自动编译；未改动 main.c / gpio.c /
  spi.c，调用方需自行在初始化阶段调用 W25q_Init(&hspi1, W25Q_CS_GPIO_Port, W25Q_CS_Pin)。

## 2026-11-22 14:06 — 修复构建错误并加入 W25Q64 上电自检

### 问题（如有）

构建失败，报错位置在 board.c（不是 W25Q 驱动）：`LED_Pin` / `LED_GPIO_Port` 未声明。
原因是 Recieve 的 main.h 在引脚改版后只剩 W25Q_CS 宏，旧 LED 宏被删，而 board.c 仍在引用。
用工程同款编译选项逐个编译全部 30 个 .c，确认只有 board.c 报错，w25q.c 编译干净。

### 改动

- `Recieve/Core/Inc/main.h::USER CODE BEGIN Private defines` — 补回 LED_Pin=GPIO_PIN_0 /
  LED_GPIO_Port=GPIOA（与参考 Recieve 一致，gpio.c 已把 PA0 配成推挽输出）。放在 USER 块内，
  CubeMX 重新生成 main.h 时不会被覆盖。副作用：PA0 现在有了 LED 语义，board.c 可正常编译。
- `Recieve/Application/BSP/W25Q/w25q_test.h` — 新建。声明 W25q_Test(void)，注释声明内部自调用
  W25q_Init()、阻塞、只在上电跑一次。
- `Recieve/Application/BSP/W25Q/w25q_test.c` — 新建。上电自检：初始化 → 读 JEDEC ID → 擦末扇区
  (0x7FF000，避开前部固件区) → 写 300 字节递增图案（起点在页尾前 100 字节，强制跨页）→
  读回逐字节比对；每步结果经 Uart_Printf(UART_LOG, ...) 打印，失败返回步骤编号 1~6。
  副作用：每次上电都会真实擦写末扇区；固件若占用末尾需改 TEST_SECTOR_ADDR。
- `Recieve/Core/Src/main.c::USER CODE BEGIN Includes` — include w25q_test.h。
- `Recieve/Core/Src/main.c::USER CODE BEGIN 2` — 在 Uart_Init() 之后调用 (void)W25q_Test()。
  放在串口初始化之后是为了保证日志能打出来；自检内部自行绑定 SPI1/PB0。
- 验证：30 个源文件全部编译零错误零警告；含启动文件的完整链接通过，符号 W25q_Test /
  W25q_Init / W25q_ReadId / Uart_Printf 均在；Flash 14896 B / 20 KB，RAM 3836 B / 20 KB。

## 2026-11-22 14:18 — 修复 W25Q 读 ID 全 0：拆分收发改为全双工收发

### 问题（如有）

上电自检打印 `ID=0x000000`（期望 0xEF4017），停在 step2。已排除引脚和供电：
用户确认接线为 SPI1(PA5/6/7)+PB0，模块为 VCC/GND+4 线、WP/HOLD 板载上拉。
根因是原驱动把「发命令」和「收数据」拆成 HAL_SPI_Transmit + HAL_SPI_Receive 两步：
SPI 全双工下，发 0x9F 命令的那 8 个时钟里 Flash 已在 MISO 送出第一个 ID 字节，
Transmit 把这段数据丢掉，后续 Receive 读到错位数据；0x03 读数据命令同理会丢第一个数据字节。

### 改动

- `Recieve/Application/BSP/W25Q/w25q.c` — 新增 static Transceive() 封装 HAL_SPI_TransmitReceive，
  用 dummy byte 边发边收。W25q_ReadId / W25q_ReadStatus / WaitBusy / W25q_Read 全部改走它：
  ReadId/ReadStatus 收 1 字节应答；WaitBusy 的 BUSY 位取 rx[1]；Read 用 4 字节命令+1 dummy
  一次收发接住首个数据字节(rx[4])，剩余块仍用 Rx() 分 1KB 读取。只写命令(写使能/擦除/页编程)
  保留 HAL_SPI_Transmit。副作用：无；CS 拉低期间时序不变。
- `Recieve/Application/BSP/W25Q/w25q.c::W25q_ReadIdRaw` — 新增诊断函数，返回 JEDEC ID 原始
  三字节(rx[1..3])，不改变器件状态。
- `Recieve/Application/BSP/W25Q/w25q.h` — 声明 W25q_ReadIdRaw，并补充 ReadId 注释说明时序修复。
- `Recieve/Application/BSP/W25Q/w25q_test.c` — ID 失败分支新增诊断打印：raw 三字节、状态寄存器、
  以及直接读 PA6 的 MISO 空闲电平(MISO_GPIO_PORT/MISO_PIN)。用途：raw 全 FF=悬空/无响应，
  全 00=MISO 被拉低/接线错；sr=0xFF=芯片未应答；miso_idle=0 指向硬件接线问题。成功路径零开销。
- 验证：30 个源文件编译零错误零警告。待用户烧录后回传新一轮串口日志确认 ID。

## 2026-11-22 14:22 — 修正误判并加入 bit-bang 决定性诊断（HAL SPI 仍读 FF）

### 问题（如有）

新日志：`ID=0xFFFFFF`、`raw=FF FF FF st=0`、`sr=0x00 miso_idle=0`。
上次把 `miso_idle=0` 解读为「MISO 对地短路/接线错」是**误判**：spi.c:86 把 PA6 配成
GPIO_MODE_INPUT + GPIO_NOPULL（浮空），浮空脚读 0 读 1 都是随机的，不能作为电气结论。
且 `raw=FF`(SPI 读全 1) 与 `miso_idle=0`(GPIO 读 0) 本身矛盾，说明浮空采样不可信。
st=0 表明 HAL_SPI_TransmitReceive 本身返回 HAL_OK，SPI 外设时序层面没有报错。

### 改动

- `Recieve/Application/BSP/W25Q/w25q_test.c` — 修正 MISO 诊断注释，明确浮空输入读数不可用于判断接线。
- `Recieve/Application/BSP/W25Q/w25q_test.c` — 新增 BB_ReadId()：用纯 GPIO 翻转模拟 SPI Mode 0
  读 JEDEC ID，完全绕开 HAL SPI 外设。PA5/PA6/PA7 临时重配为普通 GPIO，
  MISO 加内部上拉消除浮空；读完调用 MX_SPI1_Init() 恢复复用配置。
  失败分支打印 `BB id=0x%06lX` 与结论：
  BB 读到 EF4017 → HAL SPI 配置/时序问题；BB 也读不到 → 硬件接线/供电问题。
- 验证：30 个源文件编译零错误零警告。待用户烧录回传 BB 诊断结果以最终定性。

## 2026-11-22 14:25 — 问题定性闭环：DO/DI 接反（非软件故障）

### 问题（如有）

用户回传日志：`ID=0xEF4017`、step3/4/5 全通过、`PASS`。用户确认根因是 **DO 与 DI 两根线接反**
（即 MISO/MOSI 接反），重接后一次通过。此前 `FF FF FF` + `st=0` 是接线错误的表象，
不是驱动缺陷；14:18 的 Transceive 全双工修复本身正确且必要，保留。

### 改动

- 本条为问题定性记录，**无代码改动**。
- `Recieve/Application/BSP/W25Q/w25q_test.c` 中的 BB_ReadId() bit-bang 诊断保留：
  只在自检失败时执行、成功路径零开销，后续硬件问题可继续用它快速区分软/硬件。
- 结论归档：以后遇到 `raw=FF FF FF` 且 `st=0`（SPI 传输成功但 Flash 不应答），
  除查供电/片选外，**优先核对 DO↔MISO、DI↔MOSI 是否接反**——这是本次的实际根因。

## 2026-11-22 19:03 — Recieve 工程新增「串口 bin 写片内 Flash」最小验证版

### 问题（如有）

用户要把串口收到的 bin 写进 STM32F103C8T6 片内 Flash。原 `Boot_Loader_Getlen()` 把数据读进局部
`rx_buf[512]` 后只返回长度、数据被丢弃，main 只打印 sum；参考工程虽有一整套 OTA，但按大容量
芯片（2KB 页 / 512KB 末尾 / boot 32K）写的，且分区与本工程 `.ld` 不符。本轮只要求先验证
"能否写入成功"，不做 CRC / 参数页 / 回滚 / 跳转，因此按最小闭环实现。

### 改动

- `Recieve/Application/BSP/Flash/flash_app.h` — 新建。从 `参考/Boot_Loader/.../flash_app.h` 移植并
  按 C8T6 订正：`FLASH_PAGE_BYTES` 0x800→0x400（中容量 1KB/页）、`BOOT_SIZE` 0x8000→0x5000
  （与本工程 `.ld` LENGTH=20K 对齐）、`FLASH_END` 0x08080000→0x08010000（C8T6 只有 64KB）；
  App 区 = 0x08005000~0x0800FFFF 共 44KB。副作用：C8T6 若按 2KB 擦会连擦两页，此宏必须保持 1KB。
- `Recieve/Application/BSP/Flash/flash_app.c` — 新建。擦写逻辑与参考版逐字一致，新增两处越界
  检查：擦页地址必须落在 [APP_ADDR, FLASH_END)、写范围不得越过 FLASH_END，防止写飞到 Boot 或
  Flash 外。注释订正为 C8T6 规格。副作用：整个工程只有本文件碰 `HAL_FLASH_*`，别处必须走这里。
- `Recieve/Application/BSP/OTA/ota_app.h` — 新建。从参考版裁剪，只保留 `Ota_Init/Ota_Feed/
  Ota_Flush` + 状态查询；去掉尾部记录/CRC/参数页/试用回滚/跳转；新增 `OTA_ERR_FULL`(-4) 表示
  写满 App 区。`OTA_PAGE_BYTES = FLASH_PAGE_BYTES`，不在别处另写 1024。
- `Recieve/Application/BSP/OTA/ota_app.c` — 新建。攒满 1KB 才擦写（`Ota_PushPage` 先擦后写），
  每页/尾页经 `Uart_Printf(UART_LOG,...)` 打进度日志（本轮验证的主要证据）；失败锁死 `s_err`，
  避免反复擦同一页耗寿命。`Ota_PushPage` 内部加越界判断（参考版靠 main 拦，这里收进模块）。
  副作用：日志每页一行，验证通过后可将 `OTA_LOG_EN` 改 0 静音。
- `Recieve/Application/App/Boot_Loader/Boot_Loader.h` — 改。新增 `Boot_Loader_Init()` 声明；
  `Getlen()` 注释更新为"取数→落盘→补尾页"，签名不变，main 的 sum 统计无需改。
- `Recieve/Application/App/Boot_Loader/Boot_Loader.c` — 改（原文件先完整读过）。三处实质改动：
  ① 删掉 `Uart_FrameReady()` 门槛——原注释说"故意不等"但代码在等，自相矛盾；OTA 是裸字节流
     没有帧边界，等 IDLE 单发信号会漏，改为直接 `Uart_Read()`；
  ② 取到数据不再丢弃，直接 `Ota_Feed()`，写失败打一次错误并锁 `s_write_err` 防刷屏；
  ③ 新增 2 秒 idle 超时补尾页 `Ota_Flush()`，否则最后一个不足 1KB 的尾巴永远留在 RAM。
  `s_rx[512]` 改静态（栈只有 1KB，局部大数组易踩底）。
- `Recieve/Core/Src/main.c` — 改。`USER CODE BEGIN 2` 中在 `W25q_Test()` 之后加 `Boot_Loader_Init()`
  （必须在 `Uart_Init()` 之后，日志走 UART_LOG）。原 while 循环的 sum 统计与打印保持不变。
- 验证：33 个源文件按 `.eide` 同款参数编译零错误零警告；完整链接通过，符号 `Ota_Init /
  Ota_Feed / Ota_Flush / Flash_ErasePage / Flash_Write / Boot_Loader_Init / Boot_Loader_Getlen`
  均在 ELF 中。Flash 17620B text + 92B data / 20480B（20K），RAM 5424B bss + 92B / 20480B。
  注：链接时发现 `build/Debug/.obj/Application/BSP/board.o` 是旧路径残留，与新的 `Board/board.o`
  符号重复；本次用 compile_commands.json 列出的目标文件链接避开它，IDE 里若报 `multiple
  definition of Led_Fan` 需 clean 后重建。
- 验证结果（2026-11-22 19:07，用户实机回传）：发送 8296 B / 0.73s，日志逐页推进，
  最后一整页 `0x08006C00 ok, total=8192`，尾页 `0x08007000 104B ok, total=8296`，
  与发送字节数完全一致 —— **片内 Flash 写入链路打通**。8 个满页(8192B) + 1 个尾页(104B)，
  2 秒 idle flush 正常触发。本版**不跳转 App、不校验 CRC**，仅确认写入动作成功。

## 2026-11-22 19:15 — Recieve 增加上电窗口跳转，APP 固件可运行

### 问题（如有）

写入验证通过后，需要让 App 区的新固件真正跑起来。用户选「上电 3 秒窗口」触发方式
（参考 Boot_Loader 的完整方案），APP 工程此前已把 `.ld` 改为 `ORIGIN=0x08005000 LENGTH=44K`
（本次核对时已是新值，无需再改）。

### 改动

- `Recieve/Application/App/Boot_Loader/Boot_Loader.h` — 改。新增 `Boot_Loader_AppValid()`、
  `Boot_Loader_JumpToApp()`、`Boot_Loader_BootWindow()` 三个声明；注释更新为"可运行版"。
- `Recieve/Application/App/Boot_Loader/Boot_Loader.c` — 改（先完整读过）。四处改动：
  ① 从参考 Boot_Loader 移植 `AppValid()`（检查头两个 32 位字：SP 在 SRAM、复位向量在 Flash
     且最低位 Thumb=1）与 `JumpToApp()`（关中断→停 SysTick→清 NVIC→AbortReceive→SPI DeInit
     →设 VTOR→换 MSP→开中断→跳）；关键差异：参考 DeInit 的是 hspi2，本板 W25Q64 在 **SPI1**，
     改为 `HAL_SPI_DeInit(&hspi1)`，并在 include 里补 `usart.h`/`spi.h`。
  ② 新增 `Boot_Loader_BootWindow()`：上电等 3000ms，期间收到任意串口数据就返回 1 留在 OTA，
     且把这一包直接喂 `Ota_Feed`（参考 main.c 的同款处理，防止首包被当噪声丢掉）；
     没数据且 App 有效则跳过去不返回；App 无效立即返回 1 停在 OTA 等下载。
  ③ 抽出 `Feed_To_Flash()` 供 Getlen 与窗口共用，写失败只报一次。
  ④ include 补 `usart.h`/`spi.h`。副作用：跳转后本工程所有状态不再可靠（栈已换成 App 的）。
- `Recieve/Core/Src/main.c` — 改。`Boot_Loader_Init()` 之后加 `(void)Boot_Loader_BootWindow()`，
  放在 W25q_Test() 之后，避免自检中途被跳转打断看不到 ID 日志。窗口阻塞最多 3 秒。
- 未改 `APP/STM32F103XX_FLASH.ld` —— 核对时已是 `ORIGIN=0x8005000 LENGTH=44K`，正确。
- 未改 `APP/Core/Src/system_stm32f1xx.c` —— `USER_VECT_TAB_ADDRESS` 是注释掉的，
  `SystemInit()` 不会重置 VTOR，Boot 在跳转前设的 `SCB->VTOR=APP_ADDR` 能保留。
- 验证：Recieve 33 源文件编译零错误、完整链接通过，符号 `AppValid/JumpToApp/BootWindow`
  均在 ELF 中；Flash 18540B/20480B，RAM 5516B/20480B。APP 31 源文件编译链接通过，
  生成 bin 8296B（与用户实机发送的完全一致），向量表头 `SP=0x20005000`、`RV=0x08006F6D`
  （Flash 内、Thumb 位=1），`AppValid()` 判据可放行。
- 待验证：烧录新 Recieve 后，3 秒内不发送应看到 `[boot] jump to 0x08005000` 并运行 App；
  3 秒内发 bin 应看到 `[boot] OTA entered` 并留在收固件流程。本版仍无 CRC/参数页/回滚。

## 2026-11-22 19:30 — APP 改为串口收 bin 写 W25Q64（0x100000）+ 回读逐字节比对

### 问题（如有）

用户要把"串口收 bin"从 Recieve 移到 APP 工程，但落盘目标改为 **W25Q64 片外 Flash**，
地址 0x100000（前 1MB 留给升级标志），片内 Flash 函数保留作调试。这是两级 OTA 的前半段：
APP 下载固件到 W25Q，Boot 以后从 W25Q 搬进片内并跳转。要求写完回读逐字节比对确认成功。

### 改动

- `APP/Application/BSP/Flash/flash_app.h/.c` — 新建（从 Recieve 复制）。片内 Flash 驱动，
  主链路不调用，保留作调试。因 `--gc-sections` 会剔除未引用函数，试过 `used+retain`
  无效（GCC 14 对 ARM 未生成 SHF_GNU_RETAIN），最终在 `APP/.eide/eide.yml` 的 LD_FLAGS
  加 `-u Flash_ErasePage -u Flash_Write` 强制保留。副作用：多占约 900B Flash；
  ⚠ APP 自身链接在 0x08005000，调试写片内会覆盖自己，慎用。
- `APP/.eide/eide.yml` — 改。LD_FLAGS 增 `-u Flash_ErasePage -u Flash_Write`。
- `APP/Application/BSP/OTA/ota_w25q.h/.c` — 新建。W25Q 版攒页落盘：
  ① 攒 **4KB 扇区**（W25Q 擦除单位，与片内 1KB 不同），满扇区才 `W25q_EraseSector` +
     `W25q_Write`（内部按 256B 自动切页）；
  ② 尾扇区只写实际字节数；
  ③ **逐字节回读比对**：写完扇区后 s_sector 仍是原始数据，分块读回 256B 用 memcmp 比，
     只需额外 256B RAM，不依赖 CRC、不受 C8T6 20KB RAM 限制（这是满足用户
     "回读逐字节比对"要求的关键设计）；失败记 s_bad_off 第一个不一致偏移。
  ④ 失败锁死，直到重新 Init。布局：前 1MB 保留 / 0x100000~0x7FEFFF 固件 / 末 4KB 自检。
- `APP/Application/App/Boot_Loader/Boot_Loader.h/.c` — 改（先完整读过）。
  Getlen 从"数数就丢"改为 `OtaW25q_Feed()`；新增 `Boot_Loader_Init()`（绑定 SPI1/PB0，
  起点 0x100000）、`Boot_Loader_Finish()`（补尾+比对）；idle 2 秒自动 Finish；
  校验通过后 s_finished 锁住不再吃数据，防止新字节破坏结果。去掉 Uart_FrameReady 门槛。
- `APP/Core/Src/main.c` — 改。Uart_Init 后调 Boot_Loader_Init 并打 `[app] w25q init OK/FAIL`；
  主循环恢复 Boot_Loader_Getlen + sum 统计；**LED 改用 Timer_Expired 非阻塞闪烁**，
  删掉原来的 HAL_Delay(500) —— 它会让主循环 1 秒才取一次串口，1KB 环缓冲 87ms 就满，
  必丢字节。副作用：LED 仍 500ms 翻转，但不再阻塞接收。
- 验证：APP 33 源文件（含新增 OTA/Flash）按 eide 同款参数编译零错误；
  完整链接通过（LD_FLAGS 带 -u），符号 `OtaW25q_Init/Feed/Finish`、
  `Boot_Loader_Init/Finish`、`Flash_ErasePage/Write` 全部在 ELF 中。
  Flash 16824B/20480B，RAM 8168B/20480B。向量表头 SP=0x20005000，合法。
- 注意：APP 的 build/Debug/compile_commands.json 是旧的（不含新目录），
  本次用 w25q.c 的命令作模板补 -IApplication/BSP/OTA 和 Flash 编译；
  IDE 里 clean/rebuild 刷新后会自动带上，若报找不到 ota_w25q.h 就是缓存未刷新。
- 验证结果（2026-11-22 19:33，用户实机回传）：发送 APP.bin 16916B / 1.49s（块 256B、间隔
  1ms），日志显示 4 个满扇区 `0x100000/101000/102000/103000` 逐个 `ok+verified, total=4096/
  8192/12288/16384`，尾扇区 `0x104000 532B ok+verified, total=16916`，与发送字节数一致；
  逐字节回读比对全部通过。`head: 00 50 00 20 29 85 00 08 2D 61 00 08 2F 61 00 08`
  → SP=0x20005000（合法 SRAM 栈顶）、RV=0x08008529（Flash 内、Thumb 位=1），
  两份后续向量也在 Flash 区，说明写入的是一份可被 Boot 跳转的合法固件。
  **结论：APP 串口收 bin 写 W25Q64(0x100000) + 逐字节回读校验链路打通。**
  注：本次发的正是 APP 自身固件（16916B 与编译产物吻合），属自下载自测。
  前 1MB 升级标志区本版未实现（用户留待后续）。

## 2026-11-22 19:38 — 删除 APP 侧片内 Flash 代码，职责归位

### 问题（如有）

上一版为"保留作调试"在 APP 里留了片内 Flash 驱动，但按两级 OTA 的分工，
APP 只负责下载固件到片外 W25Q64，片内 Flash 应由 Boot（Recieve）侧从 W25Q 搬运后写入。
用户要求删除 APP 侧片内 Flash 代码，避免职责混淆和误覆盖自身（APP 链接在 0x08005000）。

### 改动

- `APP/Application/BSP/Flash/flash_app.h/.c` — 删除整个文件与目录（上一版新建的）。
- `APP/.eide/eide.yml` — 删除 `LD_FLAGS: -u Flash_ErasePage -u Flash_Write`
  （那是为强制保留调试符号加的，文件删了就不再需要）。副作用：链接参数回到默认。
- `APP/Application/App/Boot_Loader/Boot_Loader.c` — 删 `#include "flash_app.h"` 及相关的
  used/retain 注释；文件头与段内注释改为"职责边界：APP 只写片外 W25Q，片内由 Boot 写"。
- `APP/Application/App/Boot_Loader/Boot_Loader.h` — 注释同步为职责边界说明。
- 验证：源码树 grep `flash_app/Flash_ErasePage/Flash_Write/HAL_FLASH` 零匹配；
  32 源文件（少一个 flash_app.c）编译零错误；链接通过；ELF 中 W25Q 链路 6 个符号
  （`OtaW25q_Init/Feed/Finish`、`Boot_Loader_Init/Getlen/Finish`）齐全，
  `Flash_ErasePage/Write` 与 `HAL_FLASH_*` 符号均为空。Flash 15896B（较上版 16824B
  省约 928B），RAM 8132B/20480B。功能不变 —— 上一版实测的写入链路不依赖片内 Flash。
- 副作用/注意：EIDE 里若保留旧的 build 缓存，`APP.objlist`/`compile_commands.json`
  仍列着已删的 flash_app.c，需 clean/rebuild 刷新，否则可能报找不到文件。

## 2026-11-22 19:51 — 两级 OTA 闭环：更新标志 + Boot 按需搬运

### 问题（如有）

用户要求 Boot **不是每次上电都从 W25Q 搬固件**，而是只在 APP 置了"更新标志"后才搬，
没标志就直接跳转。这避免每次上电无谓擦写片内 Flash（耗时且损寿命）。
标志放前 1MB 保留区（用户规划），结构 magic + 长度 + CRC + 自检 CRC（用户选择）。

### 改动

- `APP/Application/BSP/CRC/crc32.c/.h`、`Recieve/Application/BSP/CRC/crc32.c/.h` — 新建。
  从参考 Boot_Loader 复制。**后因 Boot 区 Flash 溢出改为位算法版**（无 256 项查表）：
  查表法 `s_crc_table[256]` 占 1KB，加了搬运逻辑后 Boot 区溢出 432 字节；
  位算法不要表，省约 1KB，结果与标准 CRC32 完全一致（用 Python zlib + 4 组向量验证
  "123456789"=0xCBF43926 等全部匹配）。两边 crc32.c 逐字一致，APP 写的 CRC Boot 才能校验过。
- `APP/Application/BSP/OTA/ota_flag.h/.c` — 新建。更新标志，独占 W25Q 第 0 扇区 0x000000
  （前 1MB 保留区开头，与固件 0x100000、自检 0x7FF000 不重叠）。结构 16 字节：
  magic('UPDT' 小端) + length + crc32 + self_crc（前 12 字节 CRC，防写一半）。
  `OtaFlag_Read/Write/Erase`：Write 先擦后写再回读校验；Read 区分 EMPTY/BAD/OK。
- `Recieve/Application/BSP/OTA/ota_flag.h/.c` — 新建，与 APP 版**逐字相同**（diff 验证）。
  两工程 srcDirs 独立无法互相引用，必须各存一份；改一边要同步另一边。
- `APP/Application/BSP/OTA/ota_w25q.c` — 改。`OtaW25q_Finish()` 末尾：从 W25Q 分块读回
  用 `Crc32_Update` 算固件 CRC32（读回算才是实锤），然后 `OtaFlag_Write(total, crc)` 置标志。
  标志写失败不算固件失败（数据已逐字节校验过），只打日志提示重发。
- `Recieve/Application/BSP/OTA/ota_apply.h/.c` — 新建。Boot 搬运：`OtaApply_CheckAndApply()`
  读标志 → 无标志返回 NOFLAG；有标志检查长度 ≤ APP_MAX → 逐 1KB 页从 W25Q(0x100000)
  读、擦片内页、写片内页 → `Crc32_Flash()` 从片内读回算 CRC 比对 → 全过才擦标志 → 返回 OK。
  **失败不擦标志**（下次上电重试），擦标志放最后。固件地址用本地 `OTA_FW_BASE=0x100000`
  常量（与 APP 侧 ota_w25q.h 一致，不 include 对方头文件）。
- `Recieve/Application/App/Boot_Loader/Boot_Loader.c` — 改。`BootWindow()` 开头插入
  `OtaApply_CheckAndApply(APP_ADDR)`：有更新先搬，搬完继续走窗口/跳转；搬运失败只打日志，
  继续让 AppValid 决定能否跳（跳不了就停在 OTA）。`Boot_Loader_Init()` 显式 `W25q_Init()`
  绑定 SPI1/PB0，不依赖 W25q_Test 的副作用。新增 include：`ota_apply.h`/`w25q.h`/`main.h`。
- `Tools/crc32_bitwise.c`、`Tools/verify_build.py` — 新建（工具）。前者是位算法 CRC 源，
  复制到两工程；后者是编译验证脚本（旧 compile_commands 不含新目录，按 w25q.c 模板补
  -I 并跳过已存在的条目，避免重复定义）。
- 验证：APP 34 文件、Recieve 36 文件均编译链接零错误。符号：APP 侧 `OtaFlag_Write/Read`、
  `Crc32_Update`、`OtaW25q_Finish` 齐全且无 `HAL_FLASH_*`（职责分离）；
  Boot 侧 `OtaApply_CheckAndApply`、`OtaFlag_Read/Erase`、`Crc32_Flash`、`Boot_Loader_BootWindow`
  齐全。Flash：Boot 19776B/20480B（**仅余 704B**，改用位算法 CRC 才装得下），
  APP 16628B/20480B。RAM：Boot 5516B，APP 8224B。
- 待验证（烧录顺序）：① 烧 APP → 发 bin（应仍逐扇区 verified，末尾多打 `fw crc32` 与
  `UPDATE FLAG SET`）；② 复位进 Boot → 应打 `[apply] update flag` → 逐页搬运 → 
  `[apply] flash crc=... expect=...` 一致 → `[apply] flag erased` → 跳转 App；
  ③ 再复位（无标志）→ 应打 `[apply] no update flag, skip` → 直接跳，不搬运。
- **风险（本版未处理）**：Boot 擦标志后若新固件跑飞，下次上电不再搬运（无回滚）。
  完整方案需试用确认+BKP 握手（参考工程有），本轮按用户要求只做最小闭环。
  Boot 区仅余 704B，后续加功能需注意，必要时扩 BOOT_SIZE 并同步改 APP 链接脚本。

### 实机端到端验证通过（2026-11-22 19:56，用户回传完整日志）

- APP 发 bin 16628B / 1.46s：4 满扇区 + 尾扇区 244B 全部 `ok+verified`，
  `head: 00 50 00 20 55 83 00 08 ...`（SP=0x20005000、RV=0x08008355，合法可跳转），
  `fw crc32=0xCA0DB6D5`，`UPDATE FLAG SET`。
- 复位后 Boot：`[apply] update flag: len=16628 crc=0xCA0DB6D5` → 逐页搬运 →
  `[apply] flash crc=0xCA0DB6D5 expect=0xCA0DB6D5`（**两端 CRC 完全一致，位算法与查表等价的实机证明**）
  → `flag erased, update applied` → `jump to 0x08005000` → APP 重新打印 `[wota] init`，
  说明新固件真的跑起来了。**两级 OTA 全链路闭环**。
- 首次上电日志有 `[apply] flag BAD, skip` —— 标志区残留旧数据（非本版写入，
  magic/self_crc 不匹配）。BAD 分支拒绝搬运、直接跳旧 App，行为符合设计（安全侧）；
  APP 随后 `OtaFlag_Write()` 先擦后写覆盖了它，闭环正常。
  注意：当前 `BootWindow()` 对 BAD 也打 "will retry next reset"，但 BAD 不会自愈
  （只有 APP 重写标志才会变有效），这句日志措辞有误导，待用户决定是否改成自愈擦除。
- 尚未验证：第三次复位应出现 `[apply] no update flag, skip`（证明标志擦除生效、不重复搬运）。

### 实机验证：无标志跳过（2026-11-23 09:25）

- 用户回传第三次复位日志：`[apply] no update flag, skip` → `[boot] jump to 0x08005000` →
  `[wota] init` / `[app] w25q init OK`。证明标志擦除生效、Boot 不重复搬运，**两级 OTA
  全部分支实测通过**（有标志搬运 / 无标志跳过 / 逐字节校验 / CRC 两端一致 / 跳转运行）。

## 2026-11-23 09:32 — 确认制：擦标志的权力从 Boot 交给 App（自愈跑飞的固件）

### 问题（如有）

原流程 Boot 搬运成功后立刻擦标志再跳转。若新 App 一进去就跑飞（时钟死等 / HardFault），
下次上电标志是空的，Boot 直接又跳那份坏固件，板子卡死只能 ST-Link 手动救。
用户要求加确认制：只有 App 真的活过来了才擦标志。

### 改动

- `Recieve/Application/BSP/OTA/ota_apply.c` — 改（先完整读过）。删除第 5 步 `OtaFlag_Erase()`
  调用，改为打 `[apply] applied OK, flag kept for app confirm`；API 返回值不变。
  副作用：搬运成功后标志继续存在，若 App 没擦则下次上电会重新搬运同一份固件（这是自愈机制，
  也是代价 —— 固件本身有 bug 时会循环重灌，不会回退到旧版本，因为没有第二份固件槽）。
- `APP/Core/Src/main.c` — 改。`USER CODE BEGIN Includes` 加 `ota_flag.h`；
  `Boot_Loader_Init()` 之后（所有外设初始化完成）加确认逻辑：先 `OtaFlag_Read()` 判断标志有效
  （避免每次启动白擦空扇区 45ms），有效则 `OtaFlag_Erase()`，打 `[app] update confirmed, flag
  erased / erase FAIL`。放在初始化后而非 main 第一行：时钟/串口/W25Q 都起来才算"真的活过来"，
  更早 HardFault 的固件不会擦标志、会被 Boot 重灌。擦失败只打日志不影响运行（最坏多搬一次）。
- 验证：APP 34 / Boot 36 文件编译链接零错误。符号：APP 侧有 `OtaFlag_Erase` + `OtaFlag_Read`
  + `Boot_Loader_Init`；Boot 侧只有 `OtaFlag_Read`（不再擦），`OtaApply_CheckAndApply` 在。
  Flash：Boot 19600B/20480B（删擦标志代码腾出 84B，余量 880B），APP 16672B/20480B。
- 待验证：发 bin → Boot 搬运后应打 `applied OK, flag kept for app confirm`（**不再**打
  `flag erased`）→ 跳 App → App 打 `update confirmed, flag erased` → 再复位 Boot 打
  `no update flag, skip`。自愈路径（故意让 App 跑飞）本轮未测。

### 实机验证通过（2026-11-23 09:37，用户回传日志）

- 发 bin 16764B / 1.47s → 4 满扇区 + 尾扇区 380B 全部 `ok+verified` →
  `fw crc32=0x92CA16D6` → `UPDATE FLAG SET`。
- 复位后 Boot：`[apply] update flag: len=16764 crc=0x92CA16D6` → 逐页搬运 →
  `flash crc=0x92CA16D6 expect=0x92CA16D6` 一致 →
  **`[apply] applied OK, flag kept for app confirm`（Boot 不再擦标志，确认制生效）**。
- 跳转后 APP：`[app] w25q init OK` → **`[app] update confirmed, flag erased`（App 擦标志，确认完成）**。
- **交接顺序实测正确**：Boot 保留 → App 擦除，确认制的核心行为跑通。
  至此两级 OTA + 确认制全链路实测闭环；唯一未测分支是"App 跑飞→Boot 重灌"的自愈路径
  （需在 APP main 里插 while(1) 模拟，用户未要求）。

## 2026-11-23 09:47 — W25Q 2MB 备份区 + 真回滚（替代重灌同一份）

### 问题（如有）

确认制只能"重灌同一份新固件"：若 bug 在固件本身，重灌一百次也不会好。
用户要求：擦除前把旧固件保存到 W25Q 2MB 处，新固件跑飞就回滚到旧版本。
用户三项决定：① Boot 去掉 W25Q 自检腾 Flash；② APP 侧备份当前片内固件；
③ 备份区带完整头（magic+长度+CRC+self_crc）。

### 改动

- `APP/Application/BSP/OTA/ota_flag.h/.c` 与 `Recieve/.../ota_flag.h/.c` — 两端同步改。
  `OtaUpdateFlag_t` 加 `state` 字段（PENDING=1 待装 / APPLIED=2 已装未确认），
  sizeof 16→20；self_crc 覆盖 12→16 字节；`OtaFlag_Write` 加 state 参数；
  新增 `OtaFlag_SetState()`（读-改-写，已是目标状态则跳过擦除）。
  旧 16 字节标志因 self_crc 对不上判 BAD，需重刷一轮（可接受）。
  布局订正：固件 0x100000~0x1FFFFF、备份 0x200000~0x2FFFFF，不重叠。
- `APP/Application/BSP/OTA/ota_backup.h/.c` 与 `Recieve/.../ota_backup.h/.c` — 新建且逐字一致。
  备份头 16 字节（magic 'BAKU' + length + crc32 + self_crc），独占 0x200000 扇区；
  数据从 0x201000 起（最多 1MB）。`OtaBackup_SaveFromFlash(flash_addr, length)`：
  擦数据区多扇区 → 逐 256B 从片内 memcpy + 写 W25Q + 累 CRC → 从 W25Q 读回重算 CRC
  核对 → 全过才写头（头存在==备份完整）。另提供 ReadHeader/VerifyData/ReadData
  供 Boot 恢复前校验。副作用：备份一次擦约 5 个扇区（~225ms），仅升级时发生。
- `APP/Application/BSP/OTA/ota_w25q.c` — `OtaFlag_Write(s_total, crc, OTA_FLAG_PENDING)`，
  标志带 PENDING 状态。
- `APP/Core/Src/main.c` — 确认逻辑扩展：include ota_backup.h；启动时若读到标志
  state=APPLIED（刚装的新固件、此刻已证明能跑），先把当前片内固件（0x08005000 起、
  长度取标志里的）`OtaBackup_SaveFromFlash` 到备份区，再擦标志确认。
  备份 FAIL 只打日志不阻断（本次没有回滚能力，但确认照常）。
- `Recieve/Application/BSP/OTA/ota_apply.c` — 重写为双分支状态机：
  `CopyAndVerify()` 抽出公共搬运+CRC 逻辑；`ApplyPending()`：检查备份头（存在则保留
  不覆盖）→ 从 0x100000 搬新固件 → CRC 过 → `OtaFlag_SetState(APPLIED)`；失败标志
  保持 PENDING 下次重试。`ApplyRollback()`：读备份头 → VerifyData 校验（不全过就停
  Boot 等重传，绝不拿半截备份恢复）→ 从 0x201000 搬到片内 → CRC 过 → 擦标志。
  `OtaApply_CheckAndApply()` 按 state 分流，EMPTY 返回 NOFLAG 直接跳。
- `Recieve/Core/Src/main.c` — 去掉 `W25q_Test()` 调用与 w25q_test.h include，腾出约 1.4KB
  Flash 给回滚逻辑（Boot 区原只剩 880B 装不下）。W25Q 绑定由 `Boot_Loader_Init()` 显式完成。
- 验证：APP 35 / Boot 37 文件编译链接零错误。符号：Boot 侧 `OtaApply_CheckAndApply`、
  `OtaBackup_ReadHeader/VerifyData`、`OtaFlag_SetState/Erase` 齐全；APP 侧
  `OtaBackup_SaveFromFlash`、`OtaFlag_Write/Erase`、`OtaW25q_Finish` 齐全。
  Flash：Boot 18900B/20480B（余 1580B），APP 17116B/20480B。RAM：Boot 5080B，APP 8388B。
- **待验证（烧录顺序）**：① 烧新两端 → 发 bin（APP 写 PENDING）→ 复位 →
  Boot 打 `state=PENDING` → 搬新固件 → `state -> APPLIED OK` → 跳 App → App 打
  `backup current fw OK` + `update confirmed, flag erased`；② 制造跑飞（APP main 插
  while(1)）→ 复位 → Boot 应打 `state=APPLIED` → `ROLLBACK: restoring ... from backup`
  → 恢复旧固件 → `rollback done` → 跳回旧版。
- **注意**：首次升级备份区为空，Boot 打 `WARN: no backup`（本次无回滚能力）；
  只有新固件第一次成功启动并由 App 备份后，第二次升级起才有回滚能力。这是设计使然。

## 2026-11-23 10:24 — 回滚测试桩（KEY2 触发 HardFault）

### 问题（如有）

回滚逻辑已实现但只有编译验证，需要一个能稳定制造"新固件跑飞"的测试固件来实测回滚分支。
要求：一个固件能测正常/跑飞两条路径，避免反复改代码重编译。

### 改动

- `APP/Core/Src/main.c` — 改（先完整读过）。`USER CODE BEGIN PD` 加编译期开关
  `OTA_TEST_CRASH`（默认 0 = 正常固件）。`Boot_Loader_Init()` 之后、确认逻辑之前加
  `#if` 测试桩：检测 KEY2(PA1)，按住（浮空输入按下为低 = GPIO_PIN_RESET）则打
  `[TEST] KEY2 held -> inject HardFault`，然后对非对齐地址 0x00000001 做 word 写，
  Cortex-M3 必然 HardFault，停在 `HardFault_Handler` 的 while(1)。
  放确认逻辑之前：标志仍是 APPLIED、备份未被覆盖，不会擦标志 → 下次复位 Boot 走回滚。
  用 KEY2 而非 KEY1：main.h 里 LED_Pin 也是 PA0（与 KEY1 冲突），PA1 才是干净按键。
- `Tools/APP_normal.bin`（17208B）/ `Tools/APP_crash_test.bin`（17296B）— 新建（生成物）。
  分别由 OTA_TEST_CRASH=0/1 编译生成；strings 确认测试串只在测试版里。
  生成后宏已改回 0，工程当前是正常固件状态。
- 验证：APP 35 文件编译链接零错误；正常版 text 17116B（测试桩被 #if 剔除，零开销）。
- **测试步骤**：① 先发 `APP_normal.bin` 正常升级一次，让 App 备份当前固件到 0x200000
  （看日志 `backup current fw OK`）；② 再发 `APP_crash_test.bin`；③ 复位时**按住 KEY2**
  再松开复位键 → 应看到 `[TEST] KEY2 held -> inject HardFault` 后卡死；④ 再复位（不按 KEY2）
  → Boot 应打 `state=APPLIED` → `ROLLBACK: restoring ... from backup` → `rollback done` →
  跳回第①步那份固件。⑤ 测试完把 OTA_TEST_CRASH 保持 0，重新编译发布。
- 注意：HardFault 后卡在 while(1)，看门狗未开不会自动复位，必须手动按复位键/SWD 复位。

### 测试桩 mode 2 + 首次实测澄清（10:26）

- 用户首次发 mode1(KEY2) 测试 bin 后直接 SWD 复位、未按住 KEY2 → 测试桩没触发，
  App 正常确认（`backup current fw OK` + `update confirmed`）。符合设计，非 bug。
- 新增 mode 2：`OTA_TEST_CRASH=2` 无条件崩，绕开按键时序。生成
  `Tools/APP_crash_always.bin`（17268B），源码宏已恢复 0。
- **澄清**：备份区存"当前已确认能跑的固件"是设计使然，测试固件正常启动后备份自己
  不代表"污染"——回滚机制验证只看 Boot 读 APPLIED 后能否从备份恢复并跳转成功，
  与备份里具体是哪一版无关。最简测试路径无需先重发 normal。
- 最简验证三步：① 发 `APP_crash_always.bin` → Boot 搬运→APPLIED→跳转 →
  App 打 `[TEST] inject HardFault (mode=2)` 卡死；② 手动复位 → Boot 应打
  `state=APPLIED` → `ROLLBACK: restoring ... from backup` → `rollback done`；
  ③ 跳回备份区那份固件（带测试桩但 mode=0 时正常运行）→ 看到 `[wota] init`
  即证明回滚机制通了。

### ★ 回滚全链路实机验证通过（2026-11-23 10:32，用户回传日志）

这次时序正确（等到了 `UPDATE FLAG SET` 才复位），完整日志证明每一环：
1. 发 crash_always.bin 17268B → 尾扇区 884B ok → VERIFY OK → crc=0x433C9006 → `UPDATE FLAG SET`
2. 复位 Boot：`state=PENDING` → `backup ready for rollback` → 搬新固件 →
   `flash crc=0x433C9006 expect=0x433C9006` 一致 → `state -> APPLIED OK` → 跳转
3. App：`[wota] init` → `w25q init OK` → **`[TEST] inject HardFault (mode=2)`**（跑飞成功，
   卡在 HardFault_Handler，标志保持 APPLIED）
4. 再复位 Boot：**`state=APPLIED`** → **`ROLLBACK: restoring 17296B from backup`** →
   `flash crc=0x8F8116F0 expect=0x8F8116F0` 一致 → **`rollback done, old fw restored`**
   → 跳转 → `[wota] init` + `w25q init OK`（旧固件恢复成功、能跑）
- **结论：两级 OTA 的全部功能（下载/校验/标志/按需搬运/确认/备份/回滚/跳转）至此全部实机闭环。**
- 关键操作经验（写给用户看的）：上位机打 `--- 发送完毕 ---` 只代表它发完了；必须等 MCU
  打完 `[wota] UPDATE FLAG SET`（约多发完后 2~3 秒，2s idle + 尾扇区擦写 + CRC）才能复位，
  否则尾页和标志都没落盘，Boot 读空标志直接跳旧固件，看起来像"没跑飞/没升级"。
- 小观察：回滚恢复的是 17296B（上一版 mode1 测试固件，备份时 0x200000 里那份），
  虽与本次 17268B 不同版，但正是"恢复到备份的上一版"的正确行为。

## 2026-11-24 09:55 — 阶段 1a：CAN 驱动 + 回环自测（OTA 改走 CAN 的第一步）

### 问题（如有）

`OTA_CAN_升级方案.md` 定下「串口信令 + 按键确认 + CAN 传输」后，阶段 1 是硬前置：
现有 `can.c` 为 CubeMX 默认值、从未验证，且 `main.c` 只调了 `MX_CAN_Init()`——
无过滤器、无 `HAL_CAN_Start`、无接收中断，外设从未进入收发状态。
`stm32f1xx_it.c` 里也没有任何 CAN 中断处理函数。

已拍板参数：波特率 500kbps、单机 node_id=1、暂无 USB-CAN 分析仪（先用回环自测）。

### 改动

- `APP/Application/BSP/CAN/can_drv.h` — 新建。对外只暴露帧结构与函数，不 include 任何 HAL 头，
  上层（ISO-TP / 会话状态机）不必认识 `CAN_HandleTypeDef`。内含报文 ID 定义
  （`0x600/0x580 + node_id`）、`CANDRV_PRESCALER=4`、回环自测开关
  `CANDRV_SELFTEST_LOOPBACK`（默认 1，有分析仪后改 0）。
- `APP/Application/BSP/CAN/can_drv.c::CanDrv_Init` — 新建。在 `HAL_CAN_Init` 之前**全量覆盖**
  时序与行为参数（500kbps / 88.9% 采样点 / ABOM+ARD+RFL 三开关 ENABLE）。
  这样做的原因：`can.c` 的这些字段在 CubeMX 非 USER 区，重新生成代码会退回
  750kbps 的历史默认值；把权威值放代码里，改 `.ioc` 或点一次 Generate Code 都不会让运行时悄悄变坏。
  副作用：`can.c` 中的数值只供阅读，真正生效的是本函数；两处需同步改。
  另外配置 32 位掩码过滤器精确匹配 `0x601`、使能 RX0/SCE 中断、`HAL_CAN_Start`、
  激活 `RX_FIFO0_MSG_PENDING|ERROR|BUSOFF|ERROR_PASSIVE`。
- `APP/Application/BSP/CAN/can_drv.c::CanDrv_Send` — 新建。等空邮箱（不阻塞死等）、
  发完再等邮箱变空才返回 OK ——「递进邮箱」在 BusOff 时会假成功，只有邮箱空了才说明帧真的走完总线。
  另：发送前把数据补零拷进本地 8 字节缓冲，因为 F1 HAL 的 `HAL_CAN_AddTxMessage`
  会无视 DLC 直接读 `aData[0..7]`，短缓冲（如 2 字节）会读越界。
- `APP/Application/BSP/CAN/can_drv.c::HAL_CAN_RxFifo0MsgPendingCallback / HAL_CAN_ErrorCallback`
  — 新建（覆盖 HAL 的 `__weak` 空实现）。收帧入 8 深度环形队列，满时丢新帧并计数
  （ISO-TP 连续帧序号会暴露缺口，比静默覆盖旧帧安全）；错误回调统计 BusOff 与
  FIFO 溢出（`HAL_CAN_ERROR_RX_FOV0`，用于将来验证块级半双工是否真留出了空窗期）。
  因为 ABOM 已开、硬件会自恢复，这里不抢着 `HAL_CAN_Start` 重入。
- `APP/Application/BSP/CAN/can_drv.c::USB_LP_CAN1_RX0_IRQHandler / CAN1_SCE_IRQHandler` — 新建。
  放在本文件而非 `stm32f1xx_it.c`（后者是生成文件，重新生成会覆盖）。
  **踩坑**：F103 中密度的 CAN RX0 向量名是 `USB_LP_CAN1_RX0_IRQHandler`（与 USB 低优先级
  共用，IRQn 20），不是 `CAN1_RX0_IRQHandler`。写错名字链接不报错，中断掉进
  `Default_Handler` 的 `while(1)`，表现为「初始化成功但永远收不到帧」。
- `APP/Core/Src/can.c::MX_CAN_Init` — 改（先完整读过）。原值 `Prescaler=16/BS1=1TQ/BS2=1TQ`
  = 750kbps、采样点 66.7%，改为 `4/15TQ/2TQ` = 500kbps、88.9%；
  `AutoBusOff/AutoRetransmission/ReceiveFifoLocked` 由 DISABLE 改 ENABLE。
  仅让文件内容与实际配置一致，运行时权威值在 `can_drv.c`。
- `APP/Core/Src/main.c::main` — 改（先完整读过）。`USER CODE BEGIN Includes` 加 `can_drv.h`；
  `USER CODE BEGIN 2` 在 `Uart_Init()` 与 `Boot_Loader_Init()` 之后调
  `CanDrv_Init(CANDRV_DEFAULT_MODE)` + `CanDrv_SelfTest()`，串口打印
  `[can] init OK (loopback)` / `[can] loopback self-test PASS (tx=.. ovf=.. err=.. bof=..)`。
  放在确认逻辑之前、`OTA_TEST_CRASH` 测试桩之前，不改变原有回滚测试路径。
- `OTA_CAN_升级方案.md` — 补 §6.4「收发器使能」：`CAN_STB`(PB7) 上电被 `gpio.c` 置高
  （推挽输出，初值 SET），若为待机脚则正常总线模式下收发器一直关着、帧出不去；
  回环不受影响，切正常模式前必须拉低。§11 待确认项标注已拍板结论，新增 §12 实施进展表。
- 验证：用 `~/.eide/tools/gcc_arm` 相同编译选项编译全部 35 个源文件 + 新增 can_drv.c，
  `-Wall` 零错误零警告；链接通过（RAM 8632B/20KB = 42.15%，FLASH 20320B/44KB = 45.10%，
  其中 can_drv 占 1116B text + 149B bss）；`objdump .isr_vector` 核对 IRQn 20/22
  两个向量已指向 `0x080090a0` / `0x080090b0`（即两个中断处理函数），不再落 `Default_Handler`。
- **待上板验证**：串口应打印 `[can] init OK (loopback)` 与 `[can] loopback self-test PASS`。
  PASS 即说明时序配置合法、过滤器放行、中断已接、回调被调、队列搬运正确。

## 2026-11-24 10:01 — 阶段 1a 改造：CAN 参数权威归还 CubeMX，驱动改为读回校验

### 问题（如有）

上一版（09:55）把位时序与 ABOM/ARD/RFL 在 `can_drv.c::CanDrv_Init()` 里全量覆盖，
理由是防止 CubeMX 重新生成代码把参数改回 750k。但这样同一组参数存在两处真相
（`.ioc` 一处、驱动宏一处），比它要解决的问题更难维护。用户明确「CAN 配置直接用 CubeMX 生成」，
改为单一真相来源：**CubeMX / .ioc 负责参数，驱动只负责校验与 CubeMX 做不到的部分**。

### 改动

- `APP/Application/BSP/CAN/can_drv.h` — 改（先完整读过）。
  删除 `CANDRV_PRESCALER`、`CANDRV_SELFTEST_LOOPBACK`、`CANDRV_DEFAULT_MODE`、`can_drv_mode_t`；
  改为 `CANDRV_EXPECT_BAUD=500000` / `CANDRV_EXPECT_SAMPLE_PM=889`（**只用于校验，不参与初始化**）、
  `CANDRV_MODE_NORMAL/LOOPBACK`（值与 HAL 一致，驱动只读不写）；
  新增返回值 `CANDRV_ERR_TIMING(-6)`、`CANDRV_ERR_NO_IRQ(-7)`；
  新增 `CanDrv_ActualBaudrate / ActualSamplePointPermille / GetMode`；
  `CanDrv_Init()` 改为无参（模式由 .ioc 决定）。
- `APP/Application/BSP/CAN/can_drv.c::CanDrv_Init` — 改（先完整读过）。删除全部参数覆盖与
  `HAL_CAN_Start` 前的重新 `HAL_CAN_Init`；改为三条校验：
  ① `HAL_CAN_GetState == READY`，否则 `-2`；
  ② `Measure_Timing()` 从 `hcan.Init`（即 HAL 真正写进 BTR 的值）反算波特率与采样点
     （TS1/TS2 字段存的是 TQ-1，反算时 +1），偏离 500kbps ±1% / 88.9% ±3% 返回 `-6`；
  ③ `NVIC_GetEnableIRQ(CAN1_RX0_IRQn) == 0` 返回 `-7`（CubeMX NVIC 里没勾时，FIFO 会满但回调
     永远不会被调用，症状「发得出去、收不回来」与总线故障极像，值得单列错误码）。
  之后才配过滤器、`HAL_CAN_Start`、`ActivateNotification`，并记下 `hcan.Init.Mode` 供上层查询。
- `APP/Application/BSP/CAN/can_drv.c` — 删（先完整读过）。移除 `USB_LP_CAN1_RX0_IRQHandler` 与
  `CAN1_SCE_IRQHandler` 两个定义、以及 `HAL_NVIC_SetPriority/EnableIRQ`：
  中断函数改由 CubeMX 生成到 `stm32f1xx_it.c`，NVIC 由 CubeMX 配置，避免同名强定义冲突。
- `APP/Application/BSP/CAN/can_drv.c::CanDrv_SelfTest` — 加一道前置判断：非 Loopback 模式直接返回
  `CANDRV_ERR_STATE`。正常模式下没有对端，硬测只会得到误导性的 FAIL。
- `APP/Core/Src/main.c::main` — 改（先完整读过）。`CanDrv_Init()` 无参调用；
  成功时打印实际波特率与采样点（`[can] init OK, 500000 bps, sample 88.8%, loopback`）；
  Loopback 才跑自测，Normal 打印 `self-test skipped (needs a peer)`；
  失败打印 `[can] init FAIL code=%d` 并注释了 -2/-6/-7 的含义。
- `APP/Core/Src/can.c::MX_CAN_Init` — 只改注释：说明权威来源是 `.ioc`，重新生成会覆盖本段注释属正常，
  运行时由 `CanDrv_Init()` 读回校验兜底（数值本身保持 500k/88.9% 不变）。
- `OTA_CAN_升级方案.md` — §6.3 改写为「初始化顺序与职责划分」（CubeMX 管什么、驱动补什么、
  读回校验怎么兜底，并记录 `USB_LP_CAN1_RX0_IRQHandler` 陷阱）；新增 §6.5「CubeMX 配置清单」
  （Mode/Prescaler/BS1/BS2/SJW/三个 Advanced 开关/NVIC 两个中断/PB7 初值），并说明生成后要核对 `can.c`。
- 验证：同前次方法编译全部 35 个源文件 + can_drv.c，`-Wall` 零错误零警告；链接通过
  RAM 8632B/20KB = 42.15%，FLASH **19376B**/44KB = 43.00%（比 09:55 版少 944B，删掉了参数覆盖与中断函数）。
- **当前状态**：`.ioc` 尚未重新生成，`it.c` 里还没有 CAN 中断函数、NVIC 也没使能，
  所以此刻烧录会在串口看到 `[can] init FAIL code=-7` —— 这是**预期行为**，
  按 §6.5 清单在 CubeMX 里配好并生成后即可 `PASS`。

## 2026-11-24 12:55 — 修复 Recieve(Boot) 工程 CubeMX 生成把 main.c 写坏 + 核查 CAN 配置

### 问题（如有）

用户在 CubeMX 里给 **Recieve（Boot，0x08000000/20K）** 配了 CAN，生成后 `Recieve/Core/Src/main.c`
被写坏：文件里出现**两个 `int main(void)`**，第一个在 `/* USER CODE BEGIN 2 */` 之后
直接接上 `/* USER CODE END Includes */` 和整段 Private/PFP 段，再接第二个 `int main(void)`
（C 语言不允许嵌套函数定义，且重复定义 main）→ 工程无法编译。

**根因**：`USER CODE` 标记配对错乱。待生成的 main.c 里 `USER CODE BEGIN 2` 内部残留了一段
`/* USER CODE END Includes */`（更早的手工粘贴遗留），CubeMX 按标记切分插入点时就错位，
把「Includes 之后到文件末尾」整段又复制了一遍。HEAD 版本只有一处 Includes 块（第 28~34 行），
说明这个残留不在已提交版本里。

### 改动

- `Recieve/Core/Src/main.c` — **恢复为 HEAD 版本**（`git checkout --`，212 行，单个 main()）。
  依据：`git diff --stat` 显示该文件只有新增 70 行、无删除，新增的 70 行全是重复段与旧注释，
  不包含任何业务逻辑；HEAD 版本已包含 `MX_CAN_Init()` 调用与全部 USER CODE 2 业务代码。
  恢复后 `USER CODE END Includes` 只剩第 34 行一处，根因一并消除。
- 验证：用 `Recieve/build/Debug/compile_commands.json` 的选项编译全部 37 个源文件，
  零错误零警告；链接通过 —— **RAM 5176B/20KB = 25.27%，FLASH 19684B/20KB = 96.11%（仅剩 796B）**。

### 核查结论（用户问「改没改对」）

- ✅ `CAN.Mode = CAN_MODE_LOOPBACK`：`can.c` 已生效。
- ✅ 三个开关：`CAN.ABOM / CAN.NART / CAN.RFLM = ENABLE` → 生成出
  `AutoBusOff / AutoRetransmission / ReceiveFifoLocked` 全 **ENABLE**（`.ioc` 键名是反向语义，
  但**以生成物为准**，实测正确）。
- ✅ NVIC：`can.c::HAL_CAN_MspInit` 里已生成 `USB_LP_CAN1_RX0_IRQn` / `CAN1_SCE_IRQn`
  的 `SetPriority(0,0)` + `EnableIRQ`；`stm32f1xx_it.c` 生成了两个中断处理函数
  （名字正是 `USB_LP_CAN1_RX0_IRQHandler` / `CAN1_SCE_IRQHandler`）；`stm32f1xx_it.h` 加了声明。
- ❌ **位时序没改**：`can.c` 仍是 `Prescaler=16 / TimeSeg1=CAN_BS1_1TQ / TimeSeg2=CAN_BS2_1TQ`
  = 750kbps、采样点 66.7%。`Recieve.ioc` 里也只有 `CAN.CalculateBaudRate=749999`
  / `CalculateTimeBit=1333` / `CalculateTimeQuantum=444.44`，**没有** `CAN.Prescaler/BS1/BS2/SJW` 键，
  说明 Bit Timings 那一组字段在界面上没被动过。
- ⚠ 注意事项：本次配置的是 Boot 工程，而阶段 1a 的 `can_drv.c` + 回环自测在 **APP 工程**
  （0x08005000/44K）；Boot 里只有 `MX_CAN_Init()`，没有 `HAL_CAN_Start` / `ConfigFilter`，
  CAN 外设配好了也不会收发。若这是为阶段 4 提前铺路，需注意 Flash 只剩 796B，
  而 `can_drv.c` 实测占 1116B text —— **照现状直接搬进 Boot 一定溢出**。

## 2026-11-24 12:54 — 定位 CubeMX 重复生成 main.c 的根因（注释里写了标记原文）

### 问题（如有）

12:55 修复 Recieve/main.c 后，用户在 CubeMX 里改完位时序再生成，该文件**又被写坏成完全相同的
282 行结构**（两个 `int main(void)`），`diff` 显示两级产物**逐字节一致**——说明不是随机故障，
而是确定性的解析错位。

**根因**：`Recieve/Core/Src/main.c` 第 31 行（HEAD 里就有）是一句普通注释：

```c
/* w25q_test.h 已移除：自检代码为回滚逻辑腾 Flash 让路，见下方 USER CODE BEGIN 2 */
```

这句注释**字面写出了「2 号用户代码段开始标记」的原文**。CubeMX 是按字符串查找标记来定位用户
代码段的，扫描时先在第 31 行（Includes 块内）命中，就认为用户代码段从那句注释开始，一直截到
真正的结束标记为止，再整段插回模板 —— 于是 Includes 尾部、Private/PTD/PD/PM/PV/PFP 各段、
直到第二个 `main()` 全部被重复一遍。

这也解释了为什么「恢复 HEAD 再生成」会复发：HEAD 本身就带着这句自指注释，脏数据不在缓存里，
在源文件里。

> 通用教训：**源码注释里永远不要写出 CubeMX 标记原文（段名 + BEGIN/END 的组合）**，
> 哪怕只是在描述它。同理，文档/日志里提标记也应断开写法。

### 改动

- `Recieve/Core/Src/main.c` — 先 `git checkout --` 恢复 HEAD，再把第 31 行注释改写成不包含
  标记原文的措辞（改为「2 号用户代码段的开始标记」描述），并留下踩坑说明。
  验证：`grep -c "^int main(void)"` = 1；全仓扫描「非标记行却含 USER CODE 字样」只剩 0 处；
  单文件编译 `-Wall` 零错误零警告。
- `APP/APP.ioc` — 补齐 CAN 配置（键名与取值**照抄 Recieve.ioc**，即同一版 CubeMX 自己生成的写法，
  不是猜的；行尾保持 CRLF）：
  `CAN.Prescaler=4` / `CAN.BS1=CAN_BS1_15TQ` / `CAN.BS2=CAN_BS2_2TQ` /
  `CAN.Mode=CAN_MODE_LOOPBACK` / `CAN.ABOM=ENABLE` / `CAN.NART=ENABLE` / `CAN.RFLM=ENABLE` /
  `CAN.CalculateBaudRate=500000` / `CAN.CalculateTimeBit=2000` /
  `CAN.CalculateTimeQuantum=111.11111111111111`，并同步更新 `CAN.IPParameters`；
  NVIC 增加 `CAN1_SCE_IRQn` 与 `USB_LP_CAN1_RX0_IRQn` 两条（值同 Recieve.ioc）。
  副作用：APP 工程尚未重新生成，`can.c::MspInit` 里还没有那两行 `HAL_NVIC_EnableIRQ`、
  `it.c` 里还没有 CAN 中断函数 —— 此时烧录 `CanDrv_Init()` 会返回 `-7`（中断未使能），
  需在 CubeMX 打开 APP.ioc 点一次 Generate 才会补齐。
- 说明：此次只改了 `.ioc` 与注释，未手改 APP 的生成文件；保持「生成物由 CubeMX 产出」的原则。

## 2026-11-24 17:28 — 从 Boot(Recieve) 工程彻底移除 CAN，取消阶段 4

### 问题（如有）

用户提出：CAN 接收代码在 APP 里，Boot 不需要 CAN。核对后确认成立：
`OtaW25q_Feed(buf, len)` 只认「连续字节流」，不关心字节来自 CAN 还是串口，传输通道与搬运逻辑本就解耦。
方案 §9 原先要求「Boot 增加最小 CAN 入口」只是针对「App 坏了」场景的冗余保险，而该场景已有
实机验证过的 UART 救砖通道（`Boot_Loader_BootWindow()` 的 3 秒窗口）。

继续保留的代价很具体：Boot 只剩 796B Flash，一个裁剪版 CAN 接收粗估需 600~900B；
且要维护两套行为不一致的协议栈。故决定**取消阶段 4**，Boot 回到纯 UART 通道。

### 改动

- `Recieve/Core/Src/can.c`、`Recieve/Core/Inc/can.h` — **删除**。Boot 不再有 CAN 初始化与句柄。
- `Recieve/Core/Src/main.c::main` — 删 `#include "can.h"` 与 `MX_CAN_Init();`，
  在原位置留注释说明「这里是刻意没有的，不是漏了」。
  注：本文件第 31 行上一轮已改掉含 CubeMX 标记原文的注释（重复生成的根因），本次未动。
- `Recieve/Core/Src/stm32f1xx_it.c` — 删 `USB_LP_CAN1_RX0_IRQHandler`、`CAN1_SCE_IRQHandler`
  两个函数（含上方 doc 注释）与 `extern CAN_HandleTypeDef hcan;`。
- `Recieve/Core/Inc/stm32f1xx_it.h` — 删对应两条声明（删完与 HEAD 一致）。
- `Recieve/Recieve.ioc` — 删全部 `CAN.*` 键、`Mcu.IP0=CAN`（并重编号 `Mcu.IP1..6 → IP0..5`、
  `Mcu.IPNb=7 → 6`）、`PB8.*` / `PB9.*` 的 CAN 复用（PB8/PB9 恢复为未分配引脚）。
- `Recieve/cmake/stm32cubemx/CMakeLists.txt` — 源清单移除 `Core/Src/can.c`（文件已删，
  否则 CMake 构建会找不到文件）。`stm32f1xx_hal_can.c` 与 `HAL_CAN_MODULE_ENABLED` **保留**：
  HAL 驱动留着无害（未被引用，`--gc-sections` 会整个丢弃），将来若恢复 CAN 不必再动 conf。
- `OTA_CAN_升级方案.md` — 状态行标注阶段 4 取消；§9 两行风险表改写为定稿结论（含「现场串口可达」
  这一前置条件）；§10 分期表划掉阶段 4、说明总工期 4 期减为 3 期；§12 增加本次移除记录。
- 验证：编译 36 个源文件（`can.c` 已跳过）`-Wall` 零错误零警告；链接通过
  **RAM 5136B/20KB = 25.08%，FLASH 18460B/20KB = 90.14%**。
  相比移除前（19684B/96.11%）**净省 1224B Flash、40B RAM**，余量从 796B 提到 2020B。
  `nm` 检查镜像内已无 CAN 实现代码；`objdump .isr_vector` 确认 IRQn 20/22 均落回
  `Default_Handler`（`0x080038a0`），即那两个向量只剩启动文件的弱别名，不会执行任何 CAN 逻辑。
- 副作用与注意事项：
  1. `CAN_STB`(PB7) 仍保留为推挽输出（`gpio.c`，初值高=收发器待机）。它是**板级接线**、
     不是 CAN 外设配置，Boot 不管 CAN 时把它保持在待机反而更合适，故未删。
  2. 将来若真要恢复 Boot 侧 CAN：在 CubeMX 里重新勾选 CAN 即可（`.ioc` 已无残留键，
     不会与新配置冲突），但需先解决 §9 记录的 2020B 余量问题。

## 2026-11-24 17:57 — 串口助手移除 OTA 状态面板、修复日志无法复制，重新打包 exe

### 问题（如有）

用户要求把 `Tools/SerialAssist` 界面上的「OTA 状态」卡片整块去掉；并反馈红色失败日志
（如 `[can] init FAIL code=-7`）选不中、复制不了，想直接贴给智能体分析。

根因确认：红色只是 `_classify()` 打上的 `bad` 文本 tag，不是复制障碍；真正原因是
接收区 `tk.Text` 一直处于 `state="disabled"`，Tk 在禁用态下不接受选中操作，
所以包括红字在内的所有日志都选不中，Ctrl+C 自然无效。

### 改动

- `Tools/SerialAssist/serial_assist.py::_build_middle` — 不再构造右侧 OTA 状态卡片，
  右栏只保留「固件发送」；接收区由 `state="disabled"` 改为 `normal`，绑定 `<Key>`
  白名单与右键菜单（复制选中 / 复制全部日志 / 全选）。副作用：接收区无法再被
  用户改写（编辑键由 `_on_rx_key()` 吞掉），但选中、复制、Ctrl+L 清屏均正常。
- `Tools/SerialAssist/serial_assist.py::_build_ota_card` — 整块删除（状态灯、App 基址、
  页大小、已落盘、目标大小、进度条、校验结论标签）。这些结论仍可由日志原文看到，
  发送决策所需数据由解析器继续维护。
- `Tools/SerialAssist/serial_assist.py::__init__` — 删掉纯显示字段 `fw_verified`；
  其余 OTA 字段加注释说明为何保留：`ota_saved / ota_proto / ota_verify / ota_done_ts`
  决定尾部记录发不发、何时等落盘与复位。
- `Tools/SerialAssist/serial_assist.py::_handle_line` — 摘掉全部面板更新调用，只保留
  影响发送决策的四类解析（ready / page / tail / verify）；删除随之失效的
  `_set_state / _flash_lamp / _update_progress / _update_verify` 与 6 个死正则常量。
  副作用：boot / can 等日志不再驱动界面状态，仅按来源着色显示。
- `Tools/SerialAssist/serial_assist.py::_insert / clear_rx / open_port / close_port /
  pick_file / start_file_send / _on_tx_progress` — 同步移除 state 切换与面板标签写入，
  颜色高亮保持不变，复制不再受控件禁用态影响。
- `Tools/SerialAssist/README.md::界面与日志说明` — 示意图去掉 OTA 状态块；新增
  「复制日志」一节；日志解析表改为描述内部作用；同步修正几处引用右侧面板的过时说明。
- `Tools/SerialAssist/dist/STM32_OTA_SerialAssist.exe` — 用现有 PyInstaller 参数重新打包
  （`--onefile --noconsole`，含 `list_ports_windows` 隐藏导入）。自检通过：
  UI self-check OK、串口枚举 7 个、配置路径落在 exe 同目录。
- 验证：源码与 exe 的 `--check` 均通过；自动化实测确认 OTA 卡片控件已不存在
  （`hasattr(app, "pb") == False`）、接收区 `state == normal`、模拟的红色 FAIL 行
  可被选中并复制进剪贴板（`[can] init FAIL code=-7`）。
- 清理：PyInstaller 命令行模式在项目根目录生成的临时 `STM32_OTA_SerialAssist.spec`
  已删除，原有 `Tools/SerialAssist/STM32_OTA_SerialAssist.spec` 保持不变。

## 2026-11-24 18:14 — 修复红字日志选中时看不出高亮

### 问题（如有）

用户实测截图显示：拖选接收区时其它行变蓝底，唯独 `[can] init FAIL code=-7`
那行仍是浅红底，反馈为「红色的字体没办法选择」。

根因：`bad` 标签设了 `background="#FDECEA"`，而 Tk 的 tag 优先级按创建顺序
递增、内置 `sel` 排在用户标签之前，所以红底盖住了选区高亮。程序层面选区
其实已包含该行（剪贴板实测能拿到），缺的是视觉反馈。

### 改动

- `Tools/SerialAssist/serial_assist.py::_build_middle` — 着色配置末尾给 `sel`
  显式设 `background="#0078D8"` 并 `tag_raise("sel")`，让选中高亮压过所有
  来源色标签；不设 `sel` 的 `foreground`，选中后仍按来源着色，便于分辨日志类型。
- `Tools/SerialAssist/README.md::复制日志` — 补充两个 Tk 坑的说明
  （disabled 不能选中、tag 优先级遮挡选区高亮）。
- 验证：Tk 小实验确认 `tag_raise("sel")` 前后 `tag_names` 顺序由
  `('sel', 'bad')` 变为 `('bad', 'sel')`，红字行选中时蓝底生效；
  在真实界面实例上复测 `tag_names` 为 `('bad', 'sel')`、剪贴板仍拿到红字原文。
- `Tools/SerialAssist/dist/STM32_OTA_SerialAssist.exe` — 18:15 重新打包，自检通过
  （UI OK、串口 7 个、配置路径在 exe 同目录）。因当时有两个旧实例正在运行、
  文件被占用，原 exe 先改名为 `STM32_OTA_SerialAssist_old.exe` 作为备份；
  关闭旧实例后可删除该备份文件（Windows 不允许删除运行中的 exe，故未自动清理）。

## 2026-11-24 18:22 — 串口助手彻底移除「新架构」FWUP 路径

### 问题（如有）

用户要求去掉界面上的「新架构：直接发给 App（FWUP 头，免复位）」复选框。
确认范围后（用户选"连发送逻辑一起删干净"，并同意同步删除命令行工具的
app-ota 模式），把整条 FWUP 路径从两个工具中移除，发送固定走 BootLoader 老协议。

### 改动

- `Tools/SerialAssist/serial_assist.py::模块级协议定义` — 删除 `OTA_HDR_MAGIC`、
  `OTA_HDR_VER` 与 `build_fwup_header()`（32 字节槽头），保留 `build_ota_tail()`
  与 OTAE 尾部；注释说明删除原因与连带影响。
- `Tools/SerialAssist/serial_assist.py::_build_fw_card` — 删除 FWUP 复选框、
  固件版本号输入框与「发送前复位」互斥联动 `_sync_send_mode()`；后续控件
  row 重编号为 5~8，布局不变。
- `Tools/SerialAssist/serial_assist.py::start_file_send` — 删除路径分叉，固定启动
  `_tx_worker`；删除 `_read_fw_ver()` 与整个 `_tx_worker_fwup()`。
- `Tools/SerialAssist/serial_assist.py::on_close` — 不再写 `fwup_mode` / `fw_ver`
  两个配置键。
- `Tools/SerialAssist/ota_headless.py` — 删除 `app-ota` 模式（choices、`--ver`
  参数、发送分支、FWUP 头打包调用），只保留 boot-ota / flash-boot / watch；
  import 只留 `build_ota_tail, find_openocd, openocd_reset`。
- `Tools/SerialAssist/dist/STM32_OTA_SerialAssist.exe` — 18:22 重新打包，自检通过
  （UI OK、7 个串口、配置路径在 exe 同目录）；上一轮遗留的 `_old.exe` 备份已删除。
- 验证：两个 Python 文件 `py_compile` 通过；`ota_headless.py --help` 只列三种模式；
  GUI `--check` 通过；全目录 Grep 无 FWUP / 版本号的活跃代码引用，只剩说明性注释。
- 副作用：不再能通过 GUI 或命令行给运行中的 App 发 FWUP 槽头升级；若需恢复，
  须从版本历史取回 `build_fwup_header()` 与 app-ota 分支。

## 2026-11-24 18:31 — 串口助手新增「刷新软件」按钮，换调试器不用重启

### 问题（如有）

用户反馈：每换一次调试器就要重新打开软件，很麻烦。
根因：`detect_debugger()` / `find_openocd()` 只在 `__init__` 调用一次，
中途换 DAP-Link / ST-Link 后 `ocd_iface` 和 SWD 复位按钮仍是启动时的旧值。
确认范围后用户选择"整个软件刷新"（软重启：断串口 + 清日志 + 重置状态 + 重认调试器）。

### 改动

- `Tools/SerialAssist/serial_assist.py::__init__` — 把内联的调试器探测抽成
  `_probe_debugger()`；`_build_topbar` 在「打开串口」右侧新增「刷新软件」按钮。
- `Tools/SerialAssist/serial_assist.py::_probe_debugger` — 2026-11-24 新增。
  重新探测 OpenOCD 与在位调试器；配置里手写的 `ocd_iface` 仍优先于探测结果
  （保留"认错了"的逃生口，刷新不偷偷覆盖用户明确的选择）。
- `Tools/SerialAssist/serial_assist.py::_apply_debugger_ui` — 2026-11-24 新增。
  统一同步复位按钮可用性、提示文字与颜色；label 存为 `lb_ocd_hint` 引用，
  从 `_build_fw_card` 抽出，刷新时不必重建控件。
- `Tools/SerialAssist/serial_assist.py::refresh_software` — 2026-11-24 新增。
  停发送/断串口 → 清屏与解码/解析/计数状态 → 重认调试器 → 重扫串口 → 打回执；
  固件路径、窗口位置、配置勾选保留（等价于真重启后从 json 恢复）。
  副作用（用户已确认）：已连串口会被断开、日志与 OTA 状态清零、发送中的固件被中止。
- `Tools/SerialAssist/README.md::换调试器不用改配置 / 常见问题` — 新增「刷新软件」
  小节说明用法与配置覆盖优先级；SWD 复位排障加第 0 步；界面示意图补按钮。
- `Tools/SerialAssist/dist/STM32_OTA_SerialAssist.exe` — 18:31 重新打包，自检通过
  （UI OK、串口 7 个、调试器 ST-Link V2-1 → interface/stlink.cfg、配置在 exe 同目录）。
- 验证：源码 `py_compile` 与 `--check` 通过；自动化模拟"状态弄脏后点刷新"，
  确认 `ota_saved / ota_verify / rx_count / tx_count` 归零、日志清空且写入两行回执、
  调试器重新识别结果与按钮状态一致。

## 2026-11-24 18:49 — 串口助手新增日志颜色设置窗口

### 问题（如有）

用户问清接收区六种日志颜色的分类逻辑后，要求能在界面上自己设置字体颜色。
确认范围：只改接收区六种日志标签的字色，不碰整个软件主题，也不改 `bad` 的浅红底。

### 改动

- `Tools/SerialAssist/serial_assist.py::常量` — 新增 `LOG_COLOR_DEFS`（六种标签的
  名称/默认色/预览示例/命中规则）、`BAD_BG`、`HEX_COLOR_RE`；`import colorchooser`。
- `Tools/SerialAssist/serial_assist.py::__init__` — 从 `cfg.log_colors` 读字色，
  缺项或非法色值（不匹配 `#RRGGBB`）静默回退默认色。原因：Tk 收到非法颜色会在
  `tag_configure` 抛 TclError 导致界面起不来，不能让手改 json 把软件弄崩。
- `Tools/SerialAssist/serial_assist.py::_apply_log_colors` — 2026-11-24 新增，
  统一应用六种 tag 的字色；改色后重新 `tag_raise("sel")`，防止红字行选中又看不出高亮。
  启动时和设置确定后都走这里。
- `Tools/SerialAssist/serial_assist.py::open_color_settings` — 2026-11-24 新增。
  顶部栏「设置」按钮打开 Toplevel：六种色块按钮 + 系统调色盘 + 实时预览 +
  恢复默认 + 取消/确定。用临时 `pending` 字典承接改动，取消不碰主界面；
  确定后一次性应用、写 json、打日志。
- `Tools/SerialAssist/serial_assist.py::on_close` — 保存配置时带上 `log_colors`。
- `Tools/SerialAssist/README.md::设置日志颜色` — 新增一节，列出六种标签的
  规则与默认色，说明字色可改而 `bad` 浅红底固定、非法色值会回退。
- `Tools/SerialAssist/dist/STM32_OTA_SerialAssist.exe` — 18:49 重新打包，自检通过
  （UI OK、串口 7 个、调试器 ST-Link V2-1 → interface/stlink.cfg、配置在 exe 同目录）。
- 验证：改色应用到 `tag_configure` 后读取确认 `bad` 字色变化、背景仍是 `#FDECEA`；
  配置写入再读回一致；打开设置窗口无异常；红字行 tag 顺序仍为 `('bad','sel')`，
  选中高亮优先级未被改色破坏。测试写入的临时色值已从源码目录配置中清除。

## 2026-11-24 18:57 — 日志颜色升级为可自定义规则表，换工程可带走

### 问题（如有）

用户要求能自己加"哪个字段显示什么颜色"、也能改现有字段的颜色，换工程还能用。
上一版的六种固定标签（只能改字色）不够 —— 需要规则表驱动，且规则随配置走。
确认范围：关键词"包含"匹配（| 分隔多个）；规则跟着 `assist_config.json` 走，
不加单独的导入导出按钮。

### 改动

- `Tools/SerialAssist/serial_assist.py::常量` — 用 `DEFAULT_LOG_RULES` （规则字典
  tag/name/pattern/fg/bg/builtin）替代 `LOG_COLOR_DEFS`；新增 `default_log_rules()`
  与 `load_log_rules()`：校验 tag 名（防 Tk 异常）、拦截非法色值与 `sel` 占用、
  兼容上一版 `log_colors` 字典。**修正一个加载 bug**：配置保存的是完整规则表，
  必须以配置为唯一来源重建，不能"默认 6 条 + 配置全部追加"（否则重开规则翻倍、
  用户调好的优先级顺序失效）；内置身份以代码为准，不信 json 里的 builtin 字段。
- `Tools/SerialAssist/serial_assist.py::__init__` — 改用 `self.log_rules = load_log_rules(cfg)`。
- `Tools/SerialAssist/serial_assist.py::_classify` — 从 @staticmethod 改为实例方法，
  按规则表顺序匹配（子串、| 分隔、首命中即停）；sys 规则 pattern 为空不参与匹配。
- `Tools/SerialAssist/serial_assist.py::_apply_log_colors` — 遍历规则表应用颜色；
  新增 `_clear_all_log_tags` / `_recolor_existing`，让确定设置后屏幕上已有行按新规则
  重新着色（`--- ` / `>>> ` 开头的工具提示行保留挂 sys tag）。
- `Tools/SerialAssist/serial_assist.py::open_color_settings` — 重写为规则编辑器：
  Treeview 列表 + 新增/编辑/删除/上下移 + 实时预览 + 恢复默认 + 取消/确定；
  内置规则不可删；用深拷贝 pending 承接改动，取消不碰主界面。
- `Tools/SerialAssist/serial_assist.py::on_close` — 持久化 `log_rules` 完整列表。
- `Tools/SerialAssist/README.md::自定义日志颜色规则` — 重写该节：规则语法、
  首命中优先级、换工程拷 json、非法值回退与旧配置兼容。
- `Tools/SerialAssist/dist/STM32_OTA_SerialAssist.exe` — 18:57 重新打包，自检通过
  （UI OK、串口 7 个、调试器 ST-Link V2-1 → interface/stlink.cfg、配置在 exe 同目录）。
- 验证：默认六条分类正确；自定义 `[can]` 规则命中且"首命中"优先于 FAIL；
  改规则后已有行重着色、sys 提示行的 tag 保留；保存→重载后 7 条不重复、顺序稳定；
  非法 fg/bg 回退默认色、`sel` 占用被拦截、内置标记以代码为准；设置窗口可打开。
  测试写入的临时规则已从源码目录配置中清除。

## 2026-11-24 20:01 — 颜色规则全部可删可改，取消"内置规则"保护

### 问题（如有）

用户点「删除」内置规则时被弹窗「内置规则不能删除，改关键词或颜色即可」拦住，
明确要求"全部规则都能修改、随意更改"。

### 改动

- `Tools/SerialAssist/serial_assist.py::规则常量` — 移除所有规则的 `builtin` 字段，
  默认六条降级为"初始值"而非特权规则；注释说明删光/删 sys 的后果与恢复方式。
- `Tools/SerialAssist/serial_assist.py::load_log_rules` — 删除"补齐缺失默认规则"
  逻辑。这是关键：否则用户删掉的规则在关窗重开后会被自动补回，删除形同虚设。
  现在 `log_rules` 键存在就以它为唯一来源（空列表也保持为空），
  只有键不存在（首次运行/老版本）才回退 `default_log_rules()`。
- `Tools/SerialAssist/serial_assist.py::open_color_settings::del_rule` — 移除内置
  规则拦截弹窗，所有规则都能删；不需要确认弹窗（取消/关窗不落盘，误删可恢复默认）。
  `add_rule` 不再写 `builtin` 字段。
- `Tools/SerialAssist/serial_assist.py::_sys_line` — 加注释说明：sys 规则被删后
  该 tag 未配置只是不着色（Tk 宽容），不会抛异常，无需在此补规则。
- `Tools/SerialAssist/README.md::自定义日志颜色规则` — 改为"默认六条也能删"，
  说明删光不崩、删 sys 后提示行变默认色、恢复默认可找回。
- `Tools/SerialAssist/dist/STM32_OTA_SerialAssist.exe` — 20:01 重新打包，自检通过
  （UI OK、串口 7 个、调试器 ST-Link V2-1 → interface/stlink.cfg、配置在 exe 同目录）。
- 验证：删 `bad` + `sys` 两条内置规则后保存→重载，仍是剩下 4 条、被删的不复活、
  规则里不再有 `builtin` 字段；删光全部规则后 `_apply_log_colors` / `_recolor_existing`
  / `_classify` 均不崩、日志正常显示只是不着色、重载空表仍为空；设置窗口打开显示 6 条。
  测试写入的临时规则已从源码目录配置中清除。

## 2026-11-24 20:41 — 固件路径即时记忆 + 「检查更新」自动装填新 bin

### 问题（如有）

用户要求选 bin 时能记住上次路径，并在旁边加按钮检查固件有没有更新、有的话自动装填。
确认范围：更新 = 当前选中的同一文件被重新编译（mtime/大小变化），不是扫目录找最新；
检测到变化自动装填 + 接收区提示，不弹窗打断。

### 改动

- `Tools/SerialAssist/serial_assist.py::_build_fw_card` — 「选择 .bin 文件」与新增的
  「检查更新」按钮并排；检查按钮初始 disabled，没选文件时不可用。
- `Tools/SerialAssist/serial_assist.py::__init__` — 新增 `self.fw_mtime`（选文件时
  记录的 mtime 基准，初值 0）。
- `Tools/SerialAssist/serial_assist.py::pick_file` — 选完调用 `_adopt_fw()`，
  抽出的装填逻辑被 pick_file 和检查按钮共用，避免两处显示不一致。
- `Tools/SerialAssist/serial_assist.py::_adopt_fw` — 2026-11-24 新增。更新 fw_path /
  fw_size / fw_mtime / 文件名显示，启用检查按钮，**立即写 last_file 到 json**
  （原来只在关窗时写，程序崩溃就丢路径），重置解析状态；announce=True 时打装填日志。
- `Tools/SerialAssist/serial_assist.py::check_fw_update` — 2026-11-24 新增。
  对比 mtime + size（只比 size 会漏掉"大小没变但重新编译"），变化则 `_adopt_fw`
  自动装填并打"旧大小→新大小"；未变化/文件被删也各打一行提示。
  不读内容算 CRC —— 发送时本来会读一遍，检查没必要重复。
- `Tools/SerialAssist/serial_assist.py::_guess_fw_dir` — last_file 指向的文件被删时
  退回其父目录，再不行才回退工程 build/Debug。
- `Tools/SerialAssist/README.md::固件路径记忆与检查更新` — 新增一节说明路径记忆、
  四种检查结果与不检测 CRC 的取舍；OTA 流程步骤补充检查更新用法。
- `Tools/SerialAssist/dist/STM32_OTA_SerialAssist.exe` — 20:41 重新打包，自检通过
  （UI OK、串口 7 个、调试器 ST-Link V2-1 → interface/stlink.cfg、配置在 exe 同目录）。
- 验证：按钮初始 disabled；装填后大小/mtime 正确且 last_file 立即落盘；
  未变化打"固件未变化"；改写 bin 后自动装填（100 B → 250 B）并打两行提示；
  删掉 bin 后按钮置灰。测试用的临时路径已从配置恢复为原工程路径。

## 2026-11-24 18:54 — 修复模式判定：F1 的 CAN_MODE_LOOPBACK 是位掩码，不是 2

### 问题（如有）

APP 按 CubeMX 生成后首次实机运行，串口打印：

```
[can] init OK, 500000 bps, sample 88.8%, normal
[can] normal mode: self-test skipped (needs a peer)
```

但 `APP.ioc` 与 `can.c` 都是 `CAN_MODE_LOOPBACK`，实际就是回环模式，**回环自测被整段跳过**。

根因（驱动自身的 bug）：F1 HAL 里 `CAN_MODE_LOOPBACK` 是位掩码 `CAN_BTR_LBKM`（0x40000000），
`CAN_MODE_NORMAL` 是 0；而 `can_drv.h` 按 F4 的习惯把它写成 `NORMAL=1 / LOOPBACK=2`，
`CanDrv_Init()` 又用 `(uint8_t)hcan.Init.Mode` 强转 —— 0x40000000 截断成 **0**，
既不等于 1 也不等于 2，于是走 else 分支打印 "normal" 并跳过自测。

危险之处在于**不报错**：`CanDrv_Init()` 仍返回 OK，只是少跑了一段最该跑的验证。

### 改动

- `APP/Application/BSP/CAN/can_drv.h` — `CANDRV_MODE_NORMAL` 1U→**0U**，`CANDRV_MODE_LOOPBACK`
  2U→**1U**，并改写注释：明确这是驱动自己的语义值，**故意与 HAL 数值不同**，
  不再声称「必须与 HAL 一致」（那句是错的）。
- `APP/Application/BSP/CAN/can_drv.c::Map_HalMode` — 新增。按位判断 `hal_mode & CAN_BTR_LBKM`，
  命中即回环（同时覆盖 `CAN_MODE_SILENT_LOOPBACK` 组合），否则 normal。按位而不是比数字，
  换 HAL 系列/版本都不会静默错。
- `APP/Application/BSP/CAN/can_drv.c::CanDrv_Init` — `s_mode` 赋值由 `(uint8_t)hcan.Init.Mode`
  改为 `Map_HalMode((uint32_t)hcan.Init.Mode)`。
- 验证：36 个源文件（编译数据库此时已含 `can_drv.c`）编译零错误零警告；链接通过
  **RAM 8632B/20KB = 42.15%，FLASH 20628B/44KB = 45.78%**。
- 待实机确认：重烧后应打印 `[can] init OK, ..., loopback` 与
  `[can] loopback self-test PASS (tx=1 ovf=0 err=0 bof=0)`。
- 附带记录（同日实机日志，非本次改动）：20608B 固件经 CAN 改造前的 UART 链路完整升级成功——
  4 扇区 + 尾 128B 全部 `ok+verified`，`crc32=0xD46B092D`，Boot 搬运后 `flash crc` 一致、
  状态转 APPLIED、App 确认擦标志。两级 OTA 底座未受本次 CAN 改造影响。

## 2026-11-24 19:42 — 阶段 2：OTA 会话层（串口信令 + 按键确认 + READY 广播）

### 问题（如有）

阶段 1a 的 CAN 驱动就绪后，固件传输仍然是「串口有字节就开始写 Flash」——
缺一道「这次真的要升级吗」的闸门，也缺一个上位机能看见的「我准备好了」信号。
按方案 §10 阶段 2 补上：串口信令 → LED 快闪 → 操作者按 KEY2 → CAN 广播 READY。

难点在于**串口是共用的**：现有 UART 收 bin 的路径已经实机验证过，不能为了加信令把它改坏。

### 改动

- `APP/Application/BSP/OTA/ota_session.h/.c` — 新建。状态机 `IDLE → ARMED → READY`
  （阶段 3 再接 DOWNLOAD/VERIFY）；`OtaSession_Poll()` 每圈调一次，
  **返回 0 才表示"什么都没发生"，主循环照旧走 bin 落盘路径**——现有升级流程零改动。
  含：ASCII 命令解析（`+OTA:REQ` / `+OTA:CANCEL`）、BUSY 防重入、ARMED 30s / READY 10s 超时、
  LED 语义（IDLE 慢闪由 main 管 / ARMED 100ms 快闪 / READY 常亮）、按键 40ms 去抖
  （触发一次须松手才能再触发）、READY 帧广播（8 字节，ID = 0x581，含块大小 1024）。
  非 IDLE 期间串口的非命令字节直接 `Uart_FlushRx` 丢弃 —— 避免误发的 bin 堆满环缓冲、
  或取消后被当成固件写进 Flash。
- `APP/Application/BSP/UART/uart_app.h/.c::Uart_Peek` — 新增。**只看不取**，读指针与
  idle_flag 都不动。没有它就只能先 `Uart_Read` 再判断，而环形缓冲不支持回退：
  取走的字节要么被当固件写进 Flash，要么丢进垃圾桶，两条都错。
- `APP/Application/BSP/Board/board.h/.c::Board_Key2Pressed` — 新增。KEY2(PA1) 收进 board 层，
  会话层不必 include main.h 去读引脚号，符合本层「上层只认函数名」的定位。不做去抖，
  去抖窗口由调用方定（会话层用 40ms）。
- `APP/Core/Src/main.c` — 改（先完整读过）。`USER CODE BEGIN Includes` 加 `ota_session.h`；
  `USER CODE 2` 在确认/备份逻辑之后调 `OtaSession_Init()`；
  主循环改为 `if (OtaSession_Poll() == 0U) { LED 500ms 慢闪 + Boot_Loader_Getlen() }`，
  即非 IDLE 时串口与 LED 都交给会话层。
  新增编译期护栏：`#if (OTA_TEST_CRASH == 1) #error` —— 该测试桩 mode 1 也读 KEY2，
  与按键确认抢同一引脚（按一下既确认升级又制造 HardFault），必须阻止；回滚测试改用 mode 2。
- `OTA_CAN_升级方案.md` — 新增 §5.0「串口信令」（命令表、与 bin 流的区分方式、已知限制）；
  §12 进展表补阶段 2 一行。
- 验证：37 个源文件（含新文件，EIDE 的编译数据库已自动收录 `ota_session.c`）
  编译零错误零警告；链接通过 **RAM 8656B/20KB = 42.27%，FLASH 22184B/44KB = 49.24%**。
- 待实机验证：
  1. 串口发 `+OTA:REQ` → `[sess] REQ accepted -> ARMED, press KEY2 within 30000ms`，LED 快闪
  2. 按 KEY2 ≥40ms → `[sess] KEY2 confirmed -> READY` + `[sess] CAN READY sent id=0x581 blk=1024B`，LED 常亮
  3. 不按键等 30s → `[sess] ARMED timeout 30000ms -> IDLE`；READY 后等 10s → `READY timeout`
  4. ARMED 时再发 `REQ` → `[sess] BUSY (state=1), REQ ignored`
  5. `+OTA:CANCEL` → 回 IDLE
  注：回环模式下 READY 帧会自己回环进接收队列（tx 计数 +1），属正常，阶段 3 由 ISO-TP 消费。

## 2026-11-24 19:47 — 回退阶段 2（会话层），回到只有阶段 1a 的固件

### 问题（如有）

阶段 1a 的「模式判定」修复（`Map_HalMode`）尚未在硬件上验证通过，就叠加了阶段 2 的会话层。
用户要求先做回环测试：底座没验穿就往上叠，出问题时无法区分是 CAN 驱动的问题还是状态机的问题。

### 改动（全部为回退，无新增逻辑）

- `APP/Application/BSP/OTA/ota_session.h`、`ota_session.c` — **删除**（本轮新增的文件）。
- `APP/Application/BSP/UART/uart_app.h`、`uart_app.c` — `git checkout` 还原到 HEAD，
  撤销新增的 `Uart_Peek()`。
- `APP/Application/BSP/Board/board.h`、`board.c` — `git checkout` 还原到 HEAD，
  撤销新增的 `Board_Key2Pressed()`。
- `APP/Core/Src/main.c` — 撤掉 `#include "ota_session.h"`、`OtaSession_Init()` 调用、
  `OTA_TEST_CRASH == 1` 的 `#error` 护栏，主循环还原为原来的
  「500ms 慢闪 + `Boot_Loader_Getlen()`」。**阶段 1a 的 CAN 初始化与自测块保留不动。**
- `OTA_CAN_升级方案.md` — §5.0 标题改为「设计草案，未实施」并注明回退原因；
  §12 进展表把阶段 2 改回「未开始，曾实现后回退」。
- 验证：编译 36 个源文件零错误零警告；链接 **RAM 8632B/20KB = 42.15%，
  FLASH 20628B/44KB = 45.78%** —— 与加阶段 2 之前完全一致，证明回退干净。
- 现固件行为：上电 → `[app] w25q init OK` → `[can] init OK, 500000 bps, sample 88.8%, loopback`
  → `[can] loopback self-test PASS/FAIL` → 确认/备份逻辑 → 主循环串口收 bin。

## 2026-11-24 19:55 — 阶段 1a 实机验证通过（CAN 回环自测 PASS）

### 问题（如有）

本条无代码改动，仅记录实机验证结果。此前「模式判定」修复（`Map_HalMode`）只在编译层面验证过，
回环自测能否真正跑通未知。

### 改动

- 无代码改动。`OTA_CAN_升级方案.md`：状态行改为「阶段 1a 已实机验证通过」，
  §12 进展表补上实机日志与「已证明 / 未证明」的边界。

### 实机结论

用户回传串口日志（固件 20628B，`crc=0xC3FBDE7B`，与本地编译产物一致）：

```
[app] w25q init OK
[can] init OK, 500000 bps, sample 88.8%, loopback
[can] loopback self-test PASS (tx=1 ovf=0 err=0 bof=0)
[app] backup current fw OK (20628B)
```

由此确认以下环节全部正确：
1. 位时序被硬件接受（否则外设进不了收发态，`CanDrv_Send` 直接失败）；
2. 帧完整发出（`tx=1`，邮箱变空）；
3. 帧被收进 FIFO0（否则 `RxPending()` 恒 0、超时）；
4. **CAN1 RX0 中断与向量表正确**（这是 `USB_LP_CAN1_RX0_IRQHandler` 那个命名坑的真正闭环验证）；
5. 回调取帧入队、队列搬运正确（8 字节逐字节比对一致）；
6. `ovf=0 err=0 bof=0`：无 FIFO 溢出、无错误帧、未进 BusOff。

**边界（写清楚免得后面误判）**：
- 回环是 CAN 内核内部 TX→RX 短接，**不经过引脚**，故 PB8/PB9 的复用、`AFIO_REMAP_CAN1_2`、
  收发器 STB(PB7) 待机状态、120Ω 终端、对端 ACK —— 全部未验证，留给阶段 1b。
- 本测试只覆盖「帧被接受并送达」的成功路径；「过滤器会挡住别的 ID」这一拒收行为未验证，
  因为即使回环帧完全绕过验收滤波，本测试同样会 PASS。

同日三次完整 OTA 循环（收 bin → 落盘 → CRC → 标志 → 搬运 → 确认 → 备份）均正常，
说明阶段 1a 的改造没有影响原有传输链路。

## 2026-11-24 20:22 — ISO-TP 分帧/拼装 + 大数据回环自测（三步对照）

### 问题（如有）

阶段 1a 只验证了单帧收发。真实链路要传 20KB 固件，必须走 ISO-TP 多帧分包；
而分帧是整条链路里最容易出 off-by-one 的地方（12 位长度、4 位序号、尾块不满 7 字节）。
在接真实总线之前，先把这一层在回环里跑穿——否则一旦上总线，数据错了分不清是分包错还是线错。

### 改动

- `APP/Application/BSP/OTA/isotp.h/.c` — 新建（**正式代码，长期保留**）。
  纯逻辑层：`IsoTp_TxInit/TxNext` 编码、`IsoTp_RxInit/RxFeed/RxDone` 解码。
  不调 CAN、不碰 Flash、不打印，因此可脱机测试。
  关键约定：12 位长度（max 4095）；**CF 序号从 1 开始**（1..15,0,1…，按直觉写 0 会全错一位）；
  尾帧 `dlc` 按实际有效字节给，不为凑满 8 而填垃圾。
  本版**不含 FC 流控**（回环下发送接收同在主循环，天然不灌爆），方案 §7 的块级流控留到接真总线时补。
- `APP/Application/BSP/OTA/isotp_test.h/.c` — 新建（**夹具，用完可编译掉**）。
  开关 `ISOTP_SELFTEST`（当前 1）。三步对照，一次烧录看全：
  1) 139B / 20 帧，**一次全发完再收** → 预期 FAIL（接收队列 8 深度，实际可用 7 → 溢出丢帧）
  2) 139B / 20 帧，每 4 帧收一次 → 预期 PASS
  3) 1024B / 147 帧，每 4 帧收一次 → 预期 PASS
  1 与 2 的数据、帧数、分帧逻辑完全相同，**只有节奏不同**，所以结果差异只能由节奏解释——
  这是方案 §7「块级半双工」必须存在的现场证明。
  数据源是内存递增图案（`i & 0xFF`），不接串口、不接 Flash；全零图案会掩盖错位，故不用。
- `APP/Core/Src/main.c` — 改（先完整读过）。加 `#include "isotp_test.h"`；
  在 CAN 单帧自测通过之后、同一分支内调用 `IsoTp_SelfTest()`。
  顺序有意为之：单帧不通就不跑多帧，否则分帧错与链路错混在一起，日志全是 FAIL 却不知先修哪个。
- 验证：38 个源文件编译零错误零警告；链接 **RAM 10680B/20KB = 52.15%（含 2KB 自测缓冲），
  FLASH 22616B/44KB = 50.20%**。
- 副作用：烧录后上电会多打 8~12 行 `[isotp]` 自测日志，全在启动阶段、不进入主循环；
  验完把 `isotp_test.h` 的 `ISOTP_SELFTEST` 改 0，夹具整段消失（RAM 回落约 2KB）。

## 2026-11-24 20:30 — 串口字节经 CAN 往返后再落盘（传输选择器接入）

### 问题（如有）

`isotp_test` 已证明分帧/拼装在回环下正确（20 帧与 147 帧均 PASS）。下一步要把拼回来的
字节流真正接进 `OtaW25q_Feed`，让数据通路闭环，并保留旧路径作救砖。
用户确认三点：① 攒够再发；② 2 秒 idle 判尾保留；③ 新老路径用编译开关二选一。

### 改动

- `APP/Application/BSP/OTA/ota_can_relay.h/.c` — 新建。**传输选择器**：
  对外只暴露 `OtaRelay_Feed/Flush/Pending/Init`，开关 `OTA_LINK_VIA_CAN` 收在本模块内。
  开 = 字节先攒成 1024 一块 → 分帧发到 CAN → 收回来拼装 → `OtaW25q_Feed`；
  关 = 直接转调 `OtaW25q_Feed`（与改造前逐字节一致）。
  这样 `Boot_Loader.c` 里**不出现任何 #if/#else**，两条路径不可能走岔。
  节奏 `OTA_RELAY_PACE=4`（每发 4 帧收一次），等于 FC 的 BS 参数——取 4 的依据是
  `CANDRV_RXQ_DEPTH=8` 判满后实际只有 7 帧可用，留一半余量；数据来自 isotp_test 的实测对照。
  错误码 `RELAY_ERR_CAN(-20)` / `RELAY_ERR_ASSEMBLE(-21)` 与 `OTAW_ERR_*` 错开，便于分辨来源。
- `APP/Application/App/Boot_Loader/Boot_Loader.c` — 改（先完整读过）。三处：
  1) `Boot_Loader_Init()`：W25Q 绑定成功后调 `OtaRelay_Init()`（清暂存块 + 打一行传输方式）；
  2) `Boot_Loader_Getlen()`：`OtaW25q_Feed(s_rx, rx_len)` → `OtaRelay_Feed(s_rx, rx_len)`；
  3) idle 判尾条件增加 `|| (OtaRelay_Pending() > 0U)`。
     **这一条不加就会丢尾**：暂存块里可能正躺着 800 字节没发出去，而此刻
     `OtaW25q_Fill()` 还是 0，只看 Fill 永远等不到 Finish。
- `APP/Application/App/Boot_Loader/Boot_Loader.c::Boot_Loader_Finish` — 在 `OtaW25q_Finish()`
  之前加 `OtaRelay_Flush()`。放这里而不是 Getlen 里：Finish 是公开接口，任何调用点都必须
  保证"最后那点零头不会凭空消失"。漏了它的典型现象是 VERIFY FAIL 但前面每个扇区都显示 ok。
- 验证：39 个源文件编译零错误零警告；链接 **RAM 12736B/20KB = 62.19%，
  FLASH 23272B/44KB = 51.65%**。
  RAM 涨到 62% 是两块各 1024B 的暂存/拼装缓冲（relay 2KB）+ isotp_test 的 2KB 夹具。
  把 `isotp_test.h` 的 `ISOTP_SELFTEST` 改 0 可回落约 2KB。
- 待实机验证（预期日志）：
  `[relay] CAN 往返已启用（块 1024B, pace 4）` → 传输中每 4KB 一行
  `[wota] sector … ok+verified` → 收尾 `[wota] tail …` / `VERIFY OK` / `UPDATE FLAG SET`。
  失败签名：`[relay] assemble FAIL` = 丢帧（pace 或队列问题）；
  `[wota] TAIL FLUSH FAIL` = 尾块发送失败；`VERIFY OK` 但 CRC 与 bin 不符 = 数据错位。

## 2026-11-24 21:22 — 回退到「阶段 1a 回环成功」的代码状态

### 问题（如有）

用户要求把代码退回到「回环自测第一次 PASS」那个时间点，去掉此后叠加的 ISO-TP 分帧、
大数据自测夹具与传输选择器（relay）。理由：那一版是唯一在实机上被完整验证过的状态，
其上叠加的多层在后续调试中反复干扰判断。

### 改动（全部为回退，无新增逻辑）

- `APP/Application/BSP/OTA/isotp.c`、`isotp.h` — **删除**（分帧/拼装核心）。
- `APP/Application/BSP/OTA/isotp_test.c`、`isotp_test.h` — **删除**（大数据自测夹具）。
- `APP/Application/BSP/OTA/ota_can_relay.c`、`ota_can_relay.h` — **删除**（串口字节经 CAN 往返再落盘）。
- `APP/Application/App/Boot_Loader/Boot_Loader.c` — `git checkout` 还原到 HEAD。
  撤销三处：`OtaRelay_Init()` 调用、`OtaW25q_Feed` → `OtaRelay_Feed`、
  idle 判尾条件里的 `OtaRelay_Pending()`，以及 `Boot_Loader_Finish()` 里的 `OtaRelay_Flush()`。
- `APP/Core/Src/main.c` — 撤掉 `#include "isotp_test.h"` 与 `IsoTp_SelfTest()` 调用块；
  阶段 1a 的 CAN 初始化与单帧回环自测**原样保留**。
- 验证：编译 36 个源文件零错误零警告；链接 **RAM 8632B/20KB = 42.15%，
  FLASH 20628B/44KB = 45.78%**；转成 bin 后 crc32 = **0xC3FBDE7B**，
  与「回环第一次 PASS」那次实机固件**完全一致**（字节级相同，不只是体积相同）。
- 保留未动：`APP/Application/BSP/CAN/can_drv.c/.h`、`APP/APP.ioc`、`APP/Core/Src/can.c`、
  `APP/Core/Src/stm32f1xx_it.c/.h`、以及 Recieve（Boot）侧移除 CAN 的全部改动。
- 注意：芯片上目前烧的是 **0x0FDD4E32（23272B，带 relay 的版本）**，与本次回退后的
  0xC3FBDE7B（20628B）**不是同一份**。要用新状态需重新烧录。
