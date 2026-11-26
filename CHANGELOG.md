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

## 2026-11-25 09:33 — ZET6 上位机工程补齐 CAN 回环自测（APP 阶段 1a 移植）

### 问题（如有）

用户确认下一步目标是「ZET6 作上位机、C8T6 作下位机走 CAN 通信」，但 `host_computer_ZET6`
工程此前完全没有 CAN：`.ioc` 无 CAN 条目、HAL 配置里 `HAL_CAN_MODULE_ENABLED` 被注释、
没有 `can.c/h`、没有 HAL CAN 驱动源文件、中断向量仍是启动文件的弱别名。
按用户确认的范围，本轮**只做 ZET6 本地回环自测**，C8T6 侧一行不动，串口链路也不建
（用户澄清前一句"串口通信"是口误，实际说的是 CAN 通信）。

### 改动

- `host_computer_ZET6/Boot_Loader.ioc` — 按 APP.ioc 原样补 CAN 配置：
  `Prescaler=4 / BS1=15TQ / BS2=2TQ / Loopback / ABOM+NART+RFLM=ENABLE`
  （500kbps、采样点 88.9%），PB8=CAN_RX / PB9=CAN_TX（`__HAL_AFIO_REMAP_CAN1_2`），
  NVIC 勾 `USB_LP_CAN1_RX0_IRQn` + `CAN1_SCE_IRQn`，IP 列表与初始化顺序同步。
  用脚本按 CRLF 原格式写入，行数 179→201。
- `host_computer_ZET6/Core/Inc/can.h`、`Core/Src/can.c` — 从 APP 复制的 CubeMX 生成物，
  参数与本工程 `.ioc` 一致；`can.c` 中 `MX_CAN_Init()` 只做 HAL 初始化，不启动外设。
- `host_computer_ZET6/Drivers/STM32F1xx_HAL_Driver/Inc/stm32f1xx_hal_can.h`、
  `Src/stm32f1xx_hal_can.c` — 从 APP 复制。复制前用 sha256 对比 `stm32f1xx_hal_spi.c/h`
  与 `stm32f1xx.h`，确认两工程 HAL/CMSIS 同版本，避免混版。
- `host_computer_ZET6/Core/Inc/stm32f1xx_hal_conf.h` — 打开 `HAL_CAN_MODULE_ENABLED`；
  否则 HAL CAN 的 include 与驱动实现都会被条件编译掉，链接找不到符号。
- `host_computer_ZET6/Core/Src/stm32f1xx_it.c` / `Core/Inc/stm32f1xx_it.h` — 增加
  `extern CAN_HandleTypeDef hcan;` 与 `USB_LP_CAN1_RX0_IRQHandler` / `CAN1_SCE_IRQHandler`
  入口（都只调 `HAL_CAN_IRQHandler`），并补原型。向量名照高密度 F103 启动文件，
  不能写成 `CAN1_RX0_IRQHandler`——写错链接不报错、中断落进 Default_Handler，
  症状是「初始化成功但永远收不到帧」。处理函数放在 CubeMX 生成区，
  将来在图形界面重新生成时会被同名生成物替换，不会重复定义。
- `host_computer_ZET6/Application/BSP/CAN/can_drv.h`、`can_drv.c` — 从 APP 已实机验证版
  移植，结构、读回校验、8 帧接收队列、发送邮箱等待、错误计数、自测图案全部保留。
  **唯一实质差异**：ZET6 是上位机，过滤器与自测帧改用 `CANDRV_ID_RSP`(0x581)
  而不是 APP 的 `CANDRV_ID_REQ`(0x601)。理由：真实总线上 ZET6 要收的是 C8T6 的应答
  0x581；若照抄下位机的 0x601，回环照样 PASS，但接上总线后收不到对端应答，
  而且自己发出的命令会回灌进接收队列。两处已加注释说明，改回 1:1 照抄只需换宏。
- `host_computer_ZET6/Core/Src/main.c` — `USER CODE` 区加 `can_drv.h`，生成区加 `can.h`；
  在 `Uart_Init()` 之后、OTA 搬运/窗口之前插入初始化与自测块，打印
  `[can] init OK, %lu bps, sample %u.%u%%, loopback` 和
  `[can] loopback self-test PASS/FAIL (tx=.. ovf=.. err=.. bof=..)`。
  放在 UART 之后是为了日志能出来；放在 OTA 之前是为了让「CAN 通不通」和
  「升级流程对不对」两类问题不混在一起。非回环模式明确跳过自测，避免误导性 FAIL。
- `host_computer_ZET6/cmake/stm32cubemx/CMakeLists.txt` — 源清单补 `Core/Src/can.c`
  与 `stm32f1xx_hal_can.c`，CMake 构建路径不会漏文件。
- 验证：按 EIDE 的 `compile_commands.json` 同款参数编译全部 **37 个源文件，零错误零警告**；
  完整链接通过（**RAM 8408B/64KB = 12.83%，FLASH 26972B/512KB = 5.14%**）。
  `nm` 确认 `CanDrv_Init/Send/SelfTest`、两个 CAN 中断入口、HAL CAN 实现都在 ELF 中；
  导出 `.isr_vector` 核对 IRQn 20/22 分别指向 `0x08002615`/`0x08002621`
  （Thumb 位已置位，对应两个处理函数），不再落 Default_Handler。
  生成 `build/Debug/host_computer_ZET6_CAN_loopback.bin`：26972B，crc32=0xEA270E24。
- 待实机验证（**本轮未上板**）：烧到 ZET6 的 `0x08000000` 后，串口应打印
  `[can] init OK, 500000 bps, sample 88.8%, loopback` 与
  `[can] loopback self-test PASS (tx=1 ovf=0 err=0 bof=0)`。
  边界与 APP 阶段 1a 相同：回环在 CAN 内核内部完成，**不经过 PB8/PB9、AFIO remap、
  收发器、120Ω 终端、对端 ACK**；这些要等接上 C8T6 后另测。
- 注意：`.ioc` 是脚本按 CubeMX 格式手改的，首次在 CubeMX 打开时请确认 CAN 配置项与上面一致；
  图形界面重新生成会覆盖手写的 `can.c/h` 和中断入口，但参数同源、不会打架。

## 2026-11-25 10:25 — 修复 ZET6 回环自测 init FAIL code=-2

### 问题（如有）

用户烧录 09:33 版 bin 后，串口打印 `[can] init FAIL code=-2`，其余启动日志正常。
`CANDRV_ERR_STATE(-2)` 在 `CanDrv_Init()` 里只有一个来源：`HAL_CAN_GetState(&hcan)`
不是 `HAL_CAN_STATE_READY`。查 `main.c` 确认：`MX_CAN_Init()` 有定义、`.ioc` 的
functionlistsort 也排了顺序，但外设初始化序列里**漏掉了对它的调用**，`hcan` 一直停在
RESET。这不是位时序或过滤器问题——那两种情况分别返回 -6 和 -7。
编译链接能通过是因为函数存在、符号能解析，只有上板才暴露。

### 改动

- `host_computer_ZET6/Core/Src/main.c` — 在 `MX_USART1_UART_Init()` 之后、`MX_SPI2_Init()`
  之前补 `MX_CAN_Init();`，顺序与 `.ioc` 一致。加注释说明漏掉时的症状，避免以后
  看到 -2 又去查时序或 NVIC。这一处位于 CubeMX 生成区，图形界面重新生成时会自动带上。
- 验证：37 个源文件按 EIDE 同款参数重编译，零错误零警告；完整链接通过
  （RAM 8408B/64KB，FLASH 27464B/512KB）。`nm` 确认 ELF 中存在 `MX_CAN_Init`。
  重新生成 `build/Debug/host_computer_ZET6_CAN_loopback.bin`：27464B，crc32=0x5A6D4D36。
- 仍需上板确认：预期日志为 `[can] init OK, 500000 bps, sample 88.8%, loopback`
  与 `[can] loopback self-test PASS (tx=1 ovf=0 err=0 bof=0)`。
- 另外：同一份日志里的 `W25Q 芯片ID=0x000000`、两个槽全 0、参数页丢失，属于 SPI2/W25Q
  那条链路的独立问题，不影响 CAN 回环，**本轮不动**。

## 2026-11-25 10:24 — ZET6 CAN 回环自测实机 PASS（验证记录，无代码改动）

### 问题（如有）

确认补上 `MX_CAN_Init()` 后的 bin（27464B / crc32 0x5A6D4D36）在 ZET6 板上实机通过，
为后续 ZET6 ↔ C8T6 的真实 CAN 通信建立可信基线。

### 改动

- 无代码改动，仅记录实机日志：
  `[can] init OK, 500000 bps, sample 88.8%, loopback`
  `[can] loopback self-test PASS (tx=1 ovf=0 err=0 bof=0)`
- 结论：ZET6 侧 CAN 内核的位时序、过滤器放行、RX0 中断、回调、收帧队列整条路径已通；
  与 C8T6/APP 阶段 1a 的验收标准一致。
- 仍未覆盖：PB8/PB9 引脚复用、外部收发器、120Ω 终端、对端 ACK、真实总线上的
  过滤器拒收行为——这些要接上 C8T6 后另测。
- 同一份日志里 W25Q ID=0x000000 / 槽全 0 / 参数页丢失仍是 SPI2/W25Q 的独立问题，
  本轮未处理，也不影响上述 CAN 结论。

## 2026-11-25 10:38 — ZET6 板载 Flash 改接 SPI1(PA4~PA7)

### 问题（如有）

用户把野火 `STM32F103ZET6 核心板双 USB 款` 资料放进 `参考/`，要求按规格书改外部 Flash 引脚。
引脚分配表 Sheet1 第 8~11 行原文：`PA4/SPI1_NSS=连接SPI_FLASH的CS引脚`、
`PA5/SPI1_SCK=连接SPI_FLASH的CLK引脚`、`PA6/SPI1_MISO=连接SPI_FLASH的DO/IO1引脚`、
`PA7/SPI1_MOSI=连接SPI_FLASH的DI/IO0引脚`。
工程原来配的是 SPI2 + PB12(CS)/PB13/14/15，与实物不符，所以 JEDEC ID 一直读回 0。

### 改动

- `host_computer_ZET6/host_computer_ZET6.ioc` — IP 列表 `SPI2→SPI1`，引脚定义
  PB12~PB15 换成 PA4~PA7（PA4 仍是软件片选 `W25Q128_CS`），
  functionlistsort 改为 `MX_SPI1_Init`；分频 `SPI2/4` 改 `SPI1/8`。
- `host_computer_ZET6/Core/Src/spi.c` — `hspi2/SPI2→hspi1/SPI1`，句柄实例改 `SPI1`，
  MspInit 时钟改 `__HAL_RCC_SPI1_CLK_ENABLE()`、GPIO 由 GPIOB/PB13~15 改 GPIOA/PA5~7；
  **分频由 4 改 8**：SPI1 在 APB2(72MHz)、原 SPI2 在 APB1(36MHz)，
  只有 72/8=36/4=9MHz 才能保持原来的 FLASH 时钟不超频。
- `Core/Inc/spi.h` — 同步 `extern hspi1` 与 `MX_SPI1_Init()` 原型。
- `Core/Inc/main.h` — `W25Q128_CS_Pin` 由 `GPIO_PIN_12/GPIOB` 改 `GPIO_PIN_4/GPIOA`。
- `Core/Src/main.c` — 初始化序列改 `MX_SPI1_Init()`；自检里 `W25q_Init(&hspi1, ...)`；
  注释里的排查路径同步改为 PA4 / SPI1（PA4=CS、PA5=CLK、PA6=DO、PA7=DI）。
- `Application/App/Boot_Loader/Boot_Loader.c` — 跳转 App 前 `HAL_SPI_DeInit(&hspi1)`，
  注释同步；否则 App 重新初始化时仍可能在活着的 SPI 上改配置。
- `Application/BSP/W25Q/w25q.h` — 文件头/参数注释改成 SPI1 + PA4，并写明板上接线来源；
  软件片选的理由改成"PA4 同时是 SPI1_NSS，硬件 NSS 会在多字节周期中途释放"。
- 验证：37 个源文件按 EIDE 参数重编译零错误零警告，链接通过
  （RAM 8408B/64KB、FLASH 27452B/512KB）；ELF 中有 `MX_SPI1_Init`/`hspi1`/`MX_CAN_Init`/
  `W25q_Init`。新 bin `build/Debug/host_computer_ZET6_CAN_loopback.bin`：27452B，
  crc32=0x4CB00D65。**未上板**，待用户回传 W25Q ID 日志。
- 未改、需确认：该板 FLASH 实装 W25Q64（8MB，JEDEC 应为 0xEF4017），
  而 `w25q.h` 仍按 W25Q128（16MB）写 `W25Q_SIZE` 与 `W25Q_JEDEC_ID`。
  本轮只按用户要求改引脚；上板后若读到 0xEF4017，那句"异常，应为 0xEF4018"
  是期望值没同步，不是接线问题，是否改容量/ID 由用户决定。

## 2026-11-25 10:43 — ZET6 Flash 按 W25Q64 同步容量与 JEDEC ID

### 问题（如有）

上一版改引脚后上板，日志变为 `[boot] W25Q 芯片ID=0xEF4017 异常，应为 0xEF4018`，
证明 SPI1/PA4~PA7 接线已通；剩余问题只是驱动仍按 W25Q128 写容量和期望 ID。
用户贴出该日志，按上一轮提出的方案同步为板上实装的 W25Q64。

### 改动

- 边界核对：OTA 布局最高只到 `0x101000`（确认标记 `0x100000` + 日志扇区），
  双槽在 `0x000000~0x0FFFFF`，全部落在 8MB 内，缩小 `W25Q_SIZE` 不影响现有布局。
- `Application/BSP/W25Q/w25q.h` — `W25Q_SIZE` 由 16MB 改 8MB、`W25Q_JEDEC_ID` 由
  `0xEF4018` 改 `0xEF4017`；文件头与 `W25q_ReadId` 注释同步；补充"布局只用 1MB"的依据。
- `Application/BSP/W25Q/w25q.c` — 文件头 `W25Q128` 改 `W25Q64`。
- `Core/Src/main.c` — 自检日志的期望值不再写死字面量，改为从 `W25Q_JEDEC_ID` 打印：
  `[boot] W25Q 芯片ID=0x%06lX %s，应为 0x%06lX`；注释里的判定值同步为 0xEF4017。
  这样以后换器件只改驱动头文件，日志不会继续报一个作废的 ID。
- `Application/BSP/OTA/ota_slot.h` — 三处注释里的 W25Q128/16MB/15MB 改成
  W25Q64/8MB/7MB（纯注释，布局常量没动）。
- 验证：37 个源文件按 EIDE 参数重编译零错误零警告，链接通过（RAM 8408B、
  FLASH 27452B）；新 bin `build/Debug/host_computer_ZET6_CAN_loopback.bin`：
  27452B，crc32=0x12C88CC6。**未上板**，待用户确认日志显示 `正常` 且槽/参数页行为符合预期。
- 注意：槽里之前留下的乱码头部是旧固件/未擦除 Flash 的残留，与本次容量改动无关；
  是否擦除重来由用户决定，本轮没有动任何 Flash 内容。

## 2026-11-25 10:58 — ZET6 删除跳 App，串口数据改存外部 Flash 槽 A

### 问题（如有）

用户明确不再需要跳 App，新目标是「串口接收 → 写外部 Flash → 需要时经 CAN 发出」。
经确认，本轮范围：① 连跳转配套逻辑一起删；② 数据写槽 A、从数据区起点开始、
保留尾部记录 + CRC；③ CAN 发送本轮先不做。
原流程里串口数据经 `Ota_Feed()` → `Ota_PushPage()` 写的是**片内 App 区**
（`APP_ADDR=0x08008000`），跳转只是上电窗口的出口；两者都必须改。

### 改动

- `host_computer_ZET6/Core/Src/main.c` — 整体重写用户区：
  - 删除上电 3 秒窗口、`Boot_Loader_JumpToApp()` 调用、`CheckAppBoot()`、`TrialGuard()`、
    `Bkp_Init/MarkJump/JumpUnacked` 与 BKP 魔术字、参数页读写与试用状态相关调用、
    `OtaApply_FromSlot/RecoverParam/RestoreFromB` 调用；`ResetCause_Name()` 也随之删除
    （只保留复位原因快照与打印、黑匣子记录）。
  - `FinishOta()` 改为：Flush → 长度比对 → `OtaSlot_BuildHeader()` →
    `OtaSlot_VerifyData()` 读回校验 → `OtaSlot_WriteHeader(OTA_SLOT_A)`。
    不再校验/写片内 Flash，不再存参数页，不再跳转。头部最后写。
  - `HandleRx()` 越界判断从 `OTA_APP_MAX`（片内）改为 `OTA_SLOT_DATA_MAX`（508KB）。
  - 新增 `s_ota_started`：本次上电第一包数据到达时才 `OtaSlot_EraseHeader(OTA_SLOT_A)`
    并重设 `Ota_Init(OtaSlot_DataAddr(OTA_SLOT_A))`；没有新数据时，
    上次保存好的槽 A 内容不会被开机动作破坏，便于后续经 CAN 发出。
  - 就绪日志改为 `[recv] 就绪，外部 Flash 槽A 数据起点=0x%08lX，容量=%luB，协议=2`。
- `Application/BSP/OTA/ota_app.h` / `ota_app.c` — 落盘后端从片内 Flash 换成 W25Q：
  `Ota_PushPage()` 改用 `W25q_EraseSector()` + `W25q_Write()`；`OTA_PAGE_BYTES`
  由 2048（片内页）改为 4096（外部扇区），攒页缓冲 4KB；
  `Ota_Init()` 注释改为传 `OtaSlot_DataAddr(OTA_SLOT_A)=0x001000`。
  仍保留 256B 编程页由 `W25q_Write()` 自动切分。
- `Application/App/Boot_Loader/Boot_Loader.c` / `.h` — 删除 `Boot_Loader_AppValid()`
  与 `Boot_Loader_JumpToApp()` 及相应 include；只保留 `Boot_Loader_Getlen()`。
- 保留：CAN 回环自测、W25Q ID 自检、槽状态只读打印、OTA 黑匣子、12 字节尾部记录解析
  （`Ota_ParseTail` 仍在 `ota_param.c`，参数页函数虽保留在源文件但已无调用者，
  链接时被 `--gc-sections` 剔除）。
- 验证：37 个源文件按 EIDE 参数编译零错误零警告；完整链接通过
  （RAM 8376B/64KB = 12.78%，FLASH 20940B/512KB = 3.99%，比改前少 6512B）。
  `nm` 确认 `Boot_Loader_JumpToApp` / `Boot_Loader_AppValid` / `OtaApply_FromSlot` /
  `Ota_Param_Load` / `Ota_Param_Save` 均已不在 ELF 中，
  `OtaSlot_WriteHeader` / `OtaSlot_VerifyData` / `W25q_Write` / `W25q_EraseSector` 在。
  新 bin `build/Debug/host_computer_ZET6_CAN_loopback.bin`：20940B，crc32=0xF002D6C5。
  **未上板**，待用户验证串口收数据后槽 A 头部显示正常、长度/CRC 与尾部一致。
- 边界：单包数据上限 508KB；要写出可发送的槽头必须让上位机补发 12 字节尾部记录，
  只靠 2 秒 idle flush 写出的内容不会生成有效头部（数据本身仍在 Flash 里）。
- 下一步（本轮未做）：经 CAN 把槽 A 内容发出，含分帧、块级 ACK 与触发方式。

## 2026-11-25 11:20 — 上板验证：删除跳转 + 落盘改外部 Flash 槽 A 通过

### 验证结果

烧 `build/Debug/host_computer_ZET6_CAN_loopback.bin`（20940B / crc32 0xF002D6C5）到
0x08000000 后，串口输出：

```
[can] init OK, 500000 bps, sample 88.8%, loopback
[can] loopback self-test PASS (tx=1 ovf=0 err=0 bof=0)
[rst] PIN  / 复位引脚
[rst] SFT  / 软件复位
[boot] W25Q 芯片ID=0xEF4017 正常，应为 0xEF4017
[recv] 就绪，外部 Flash 槽A 数据起点=0x00001000，容量=520192B，协议=2
```

- CAN 回环在重写 main 之后仍 PASS，说明拆跳转链路没有波及 CAN 初始化顺序。
- `[boot] App 无效：向量表不对` 与 `[boot] 没有可用 App，等待串口下载` 两行已消失，
  说明 `Boot_Loader_JumpToApp()` / 有效性检查确实从执行路径上移除了，不只是符号被 gc 掉。
- 就绪行从 `[ota] 就绪，App=0x08008000` 变成 `[recv] 就绪，外部 Flash 槽A …`，
  起点 0x001000、容量 520192B（508KB）与 `OTA_SLOT_DATA_MAX` 一致，落盘目标已切到外部 Flash。
- 本轮**没有**串口数据下发，因此槽 A 头未被擦除，`[slot] A/B 不可信` 与那 32 字节乱码
  仍是旧固件/未擦除 Flash 的残留——符合"第一包数据到达才擦头"的设计，不是回归。
- 黑匣子里的 `#6/#8 无可用App` 是旧固件时代写入的历史条目，不是本次启动记录。

### 改动

- 无（仅验证与记录）。

### 待办

- 下一轮：经 CAN 分帧发送槽 A 内容，触发方式与块级 ACK 流控待定。

## 2026-11-25 17:30 — 删除开机日志的 [rst] 与 [slot] 两块打印

### 问题（如有）

用户要求去掉开机日志里两段输出：`[rst] PIN / 复位引脚`、`[rst] SFT / 软件复位` 这类复位原因行，
以及 `[slot] A/B：不可信…` 连同 32 字节十六进制转储。经确认是**两块都去掉**。

### 改动

- `host_computer_ZET6/Core/Src/main.c::PrintResetCause` — 整个函数（含文档注释）删除，
  `main()` 里 `PrintResetCause();` 调用行一并删除。
  **只删打印，不动 `ResetCause_Snapshot()`**：`s_rst_cause` 仍要给后面的黑匣子
  `OtaLog_Append()` 记录"这次是怎么起来的"用，删掉快照会让黑匣子只剩 `RST_OTHER`。
  `-Os` 下该静态函数被内联进 `main`（`nm`/`.map` 里查不到符号属正常，不是被误删），
  反汇编确认 `main` 仍在读 `RCC->CSR`(0x40021000+0x24) 并写 `s_rst_cause`。
- `host_computer_ZET6/Core/Src/main.c::main` — 「槽状态只读打印」整块删除
  （遍历 A/B 两槽的 `[slot]` 行与十六进制转储共约 45 行）。槽头部读写函数本身没动，
  `FinishOta()` 里的 `OtaSlot_BuildHeader/VerifyData/WriteHeader` 照旧；
  `OtaSlot_ReadHeader()` 因此失去唯一调用者，被 `--gc-sections` 剔除（源码保留）。
- 副作用：开机日志变短，`[slot]` 只剩收尾时的 `[ota] 槽 A 已保存：%luB CRC=0x…` 一处可看槽内容；
  复位原因只能从黑匣子 `[log] #N 复位键/…` 反查，串口不再直接说"为什么被复位"。
- 验证：37 个源文件按 EIDE 参数编译零错误零警告；链接通过
  （RAM 8376B/64KB = 12.78%，FLASH 20128B/512KB = 3.84%，比改前 20940B 少 812B）。
  `nm` 确认 `PrintResetCause`、`OtaSlot_ReadHeader` 已不在 ELF，
  `OtaSlot_WriteHeader/VerifyData`、`OtaLog_Append`、`Ota_Flush`、`W25q_Write`、
  `CanDrv_SelfTest`、`MX_CAN_Init` 仍在。
  新 bin `build/Debug/host_computer_ZET6_CAN_loopback.bin`：20128B，crc32=0x826014FA。**未上板**。
- 注意：黑匣子 `[log] #5~#9` 里的「复位键 / 无可用App」是 Flash 里早已写下的历史条目，
  不是本次打印，也不是本次启动产生的；要它们消失得清日志扇区或忽略，与本次改动无关。

## 2026-11-25 17:36 — 删掉黑匣子倒出与复位记录

### 问题（如有）

用户要求把开机的 `[log] 最近 N 条` 那几行也删掉，并且"直接不记录复位记录了"。

### 改动

- `host_computer_ZET6/Core/Src/main.c::main` — 删除 `OtaLog_DumpRecent(5U)` 调用
  及紧随其后的"追加本次复位事件"块（`ev` 判定 + `OtaLog_Append`）。开机不再有 `[log]` 任何输出。
- `host_computer_ZET6/Core/Src/main.c` — 复位原因快照随之成为死代码，一并删除：
  `RST_CAUSE_POR/PIN/SFT/IWDG/WWDG/LPWR` 六个宏、`static uint8_t s_rst_cause`、
  `ResetCause_Snapshot()` 函数体、`main()` 里 `USER CODE BEGIN Init` 段的调用。
  这些代码的唯一消费者就是刚删掉的那段黑匣子记录和更早已删的 `PrintResetCause()`，
  留着会变成 `-Os` 下的未使用警告。
  副作用：`RCC->CSR` 的复位标志不再被清除、也无人读取——对运行无影响（标志只是累积原地）。
- `host_computer_ZET6/Core/Src/main.c` — `#include "ota_log.h"` 的行尾注释同步，
  说明现在只剩接收循环那条 `OTA_MODE` 事件。
- **保留** `OtaLog_Append(OTA_LOG_EVT_OTA_MODE, …)`（接收循环里，开始新传输时记一笔）：
  用户只要求不记复位，这条没动。代价是它仍会在每次新传输开始时写一次外部 Flash 日志扇区，
  但已没有任何地方打印——是否一并去掉待用户决定。
- 验证：37 个源文件按 EIDE 参数编译零错误零警告；链接通过
  （RAM 8376B/64KB = 12.78%，FLASH 19468B/512KB = 3.71%，比改前 20128B 少 660B）。
  `nm` 确认 `OtaLog_DumpRecent`、`OtaLog_Count`、`PrintResetCause` 已不在 ELF，
  `OtaLog_Append`、`OtaSlot_WriteHeader/VerifyData`、`Ota_Flush`、`W25q_Write`、
  `CanDrv_SelfTest`、`MX_CAN_Init` 仍在。
  新 bin `build/Debug/host_computer_ZET6_CAN_loopback.bin`：19468B，crc32=0x677163BD。**未上板**。
- 注意：Flash 日志扇区（0x101000）里已有的历史条目不会被删除，只是不再打印；
  槽 A/B 头部里的旧乱码同理，与本轮无关。

## 2026-11-25 17:46 — ZET6 侧 CAN 由回环改为 Normal，周期发送测试帧

### 问题（如有）

用户 ZET6 一侧的 CAN 物理端（CAN_H/CAN_L、收发器）已接好，要求把原来的
`CAN_MODE_LOOPBACK` 自测改成真正把数据发到总线上。经确认本轮范围：
**只改 ZET6，发送内容为周期测试帧**（先验物理层，槽 A 的 OTA 数据分帧留到下一步；
C8T6 侧本次不动）。

### 改动

- `host_computer_ZET6/host_computer_ZET6.ioc::CAN.Mode` — `CAN_MODE_LOOPBACK` → `CAN_MODE_NORMAL`。
  位时序（PSC=4/BS1=15TQ/BS2=2TQ）、NART/ABOM/RFLM 均未动，仍是 500kbps / 88.9%。
- `host_computer_ZET6/Core/Src/can.c::MX_CAN_Init` — `hcan.Init.Mode` 同步改为
  `CAN_MODE_NORMAL`，并注明必须与 .ioc 保持一致，否则 CubeMX 重新生成会把 LOOPBACK 写回来。
- `host_computer_ZET6/Application/BSP/CAN/can_drv.c` — 新增 `static volatile uint32_t s_last_err`：
  在 `HAL_CAN_ErrorCallback()` 清零 `ErrorCode` **之前**存档一份错误位掩码
  （HAL 约定不清零就不会再回调，清完上层就看不到错在哪）；`CanDrv_Init()` 里一并清零。
- `host_computer_ZET6/Application/BSP/CAN/can_drv.c::CanDrv_BusDiagnostics` — 新增：
  只读 `CAN->ESR` 取出 TEC/REC，并输出最近一次错误位掩码，传 NULL 表示不需要该项。
  目的：区分「帧没出引脚」（TEC 恒 0）和「帧在总线上但没人 ACK」（TEC 持续增长）。
- `host_computer_ZET6/Application/BSP/CAN/can_drv.h` — 补充上面这个接口的声明与用法说明。
  `CanDrv_SelfTest()` 未改，Normal 下仍返回 -2（main 不再在该模式下调用它）。
- `host_computer_ZET6/Core/Src/main.c` — 新增两个宏：`CAN_TEST_TX_PERIOD_MS`(1000)、
  `CAN_TEST_TX_TIMEOUT_MS`(20)；开机 CAN 段的 Normal 分支改为打印
  `[can] normal mode: 每 1000ms 发送一帧 id=0x601，等对端 ACK`（回环分支与自测原样保留）。
- `host_computer_ZET6/Core/Src/main.c::main` — 主循环尾部新增周期发送块：
  仅 Normal 模式执行，8 字节帧 `A5 5A | 序号(小端 4B) | 01 | 00`，StdId = `CANDRV_ID_REQ`(0x601)。
  成功打印 `[can] tx #N OK, 累计 tx/err/bof`，失败打印
  `[can] tx #N FAIL code err tec rec`。`can_tx_tick` 初始回退一个周期，使第一帧立刻发出。
- 副作用/注意：
  1. Normal 模式下 `AutoRetransmission=ENABLE`，对端不回 ACK 时帧会被无限重传、邮箱不空，
     `CanDrv_Send` 走 20ms 超时返回 -4；三个邮箱都被占满后返回 -3。**这是没有应答，不是故障。**
  2. 发送失败时每秒一行日志、每帧最多阻塞 20ms，相对 1 秒周期不影响串口接收落盘。
  3. **C8T6 侧本轮未改**，仍在 LOOPBACK 且 `CAN_STB`(PB7) 置高（收发器待机），
     既不参与总线也不回 ACK —— 因此单独烧这块固件时预期全部 tx FAIL（tec 递增）。
     要看到 `tx OK`，总线上必须有一个 Normal 模式且在接收的对端。
- 验证：38 个源文件按 EIDE `compile_commands.json` 参数编译，**零错误零警告**；链接通过
  （RAM 8376B/64KB = 12.78%，FLASH 19872B/512KB = 3.79%，比改前 19468B 多 404B）。
  `nm` 确认 `CanDrv_BusDiagnostics` / `CanDrv_Send` / `CanDrv_SelfTest` / `MX_CAN_Init` 均在 ELF。
  新 bin `build/Debug/host_computer_ZET6_CAN_tx.bin`：19872B，crc32=0x798BABD9。**未上板**。
- 下一步（本轮未做）：C8T6 侧改 Normal + 拉低 STB 形成闭环；槽 A 数据经 CAN 分帧发送。

## 2026-11-25 17:59 — C8T6 侧改 Normal + 拉低 STB，主循环接收并打印 0x601

### 问题（如有）

ZET6 侧 1b 上板后串口持续输出
`[can] tx #N FAIL code=-4/-3 err=0x00000002 tec=128 rec=0`：
TEC 顶到 128（Error Passive 阈值，等于 16 次无 ACK），err=0x02 是 `HAL_CAN_ERROR_EPV`。
说明帧在发、没人应答。C8T6 收不到有三道闸：仍在 LOOPBACK（F103 回环模式**断开 RX 引脚**）、
`CAN_STB`(PB7) 置高使收发器 Standby、`main.c` 里根本没有读接收队列的代码。
用户确认后本轮只做「C8T6 接收」，不改发送逻辑、不做应答。

### 改动

- `APP/APP.ioc::CAN.Mode` — `CAN_MODE_LOOPBACK` → `CAN_MODE_NORMAL`。
- `APP/APP.ioc::PB7.PinState` — `GPIO_PIN_SET` → `GPIO_PIN_RESET`。
  改在 .ioc 里而不是只改 gpio.c，是为了让 CubeMX 重新生成时也保持低电平。
- `APP/Core/Src/can.c::MX_CAN_Init` — `hcan.Init.Mode` 同步改 `CAN_MODE_NORMAL`，
  注明与 .ioc 的对应关系。位时序未动（500kbps / 88.9%，与 ZET6 一致）。
- `APP/Core/Src/gpio.c::MX_GPIO_Init` — 把 `CAN_STB_Pin` 从
  `HAL_GPIO_WritePin(GPIOB, W25Q_CS_Pin|LED2_Pin|CAN_STB_Pin, GPIO_PIN_SET)`
  这一组里拆出来，单独 `HAL_GPIO_WritePin(CAN_STB_GPIO_Port, CAN_STB_Pin, GPIO_PIN_RESET)`。
  写法与 .ioc 改后的生成结果一致，避免下次 regen 出现无意义 diff。
- `APP/Core/Src/main.c` — CAN 初始化段的大注释改写为 1b 现状（说明回环为什么收不到、
  两道闸分别在哪）；Normal 分支的打印从 `self-test skipped` 改为
  `[can] normal mode: 接收就绪，等待上位机 0x601`。回环分支与 `CanDrv_SelfTest()` 调用原样保留。
- `APP/Core/Src/main.c::main` — 主循环末尾新增接收轮询：`while (CanDrv_RxPop(&f) == CANDRV_OK)`
  逐帧打印 `[can] rx id=0x%03lX dlc=%u data=...`（固定 8 字节）。只打印、不回帧。
- 副作用/注意：
  1. 收发器从上电起常通，比待机多几 mA，这是接收必须的代价。
  2. 每帧日志约 80 字节，115200 下约 7ms；对 `Boot_Loader_Getlen()` 的轮询节奏影响可忽略。
  3. 接收队列深度只有 8，必须在主循环每圈取干净；被 Flash 擦写阻塞过久会导致 ovf 计数上涨。
  4. 本侧过滤器认 `CANDRV_ID_REQ`(0x601)，正好是 ZET6 发的测试帧；不做应用层应答，
     总线 ACK 由 CAN 控制器硬件完成，所以 ZET6 会直接看到 tx OK。
- 验证：36 个源文件按 EIDE `compile_commands.json` 参数编译，**零错误零警告**；链接通过
  （RAM 8632B，text 20704 + data 92 = FLASH 20796B，比改前 20628B 多 168B）。
  `.isr_vector` VMA = 0x08005000，烧录地址不变；`nm` 确认 `CanDrv_RxPop` / `CanDrv_Init` 在 ELF。
  新 bin `APP/build/Debug/APP_can_rx.bin`：20796B，crc32=0x9E4633D8。**未上板**。
- 下一步（本轮未做）：槽 A 数据经 CAN 分帧发送；C8T6 侧对 0x601 做应用层应答（0x581）。

## 2026-11-25 18:07 — C8T6 加 CAN 状态心跳（物理层排查，临时）

### 问题（如有）

两侧 Normal 都已跑起来（ZET6 `tx #266 FAIL tec=152`、C8T6 `[can] init OK ... normal`
但一帧未收），软件层已无从下手：只打印"收到了什么"无法区分
「总线上没有电平活动」和「有活动但解不出帧」，而这两者指向完全相反的硬件排查方向。
故在接收方加 ESR 诊断，用 REC 判断它到底有没有"听到"总线。

### 改动

- `APP/Application/BSP/CAN/can_drv.c` — 加 `static volatile uint32_t s_last_err`，
  在 `HAL_CAN_ErrorCallback()` 清零 `ErrorCode` 前存档；`CanDrv_Init()` 里清零；
  新增 `CanDrv_BusDiagnostics(tec, rec, last_err)` 读 `CAN->ESR`（与 ZET6 侧同实现）。
- `APP/Application/BSP/CAN/can_drv.h` — 声明 `CanDrv_BusDiagnostics()` 并写明接收方判据。
- `APP/Core/Src/main.c` — `can_rx_cnt` 在接收循环里累加；主循环新增 5 秒一次的心跳
  `[can] stat: rx= ovf= err= bof= tec= rec= lasterr=`，注释里给出四种读数各自对应的结论。
  **这是临时诊断代码**，定位完成后连同 `CanDrv_BusDiagnostics` 一起删除。
- 副作用：每 5 秒多一行约 80 字节日志（115200 下约 7ms），不影响串口收 bin；
  读 ESR 是只读操作，不改任何状态。
- 验证：36 个源文件零错误零警告；链接通过（RAM 8640B，FLASH 21040B，比上一版多 244B）。
  新 bin `APP/build/Debug/APP_can_rx.bin`：21040B，crc32=0x99600A17。**未上板**。
- 判据（下一轮按此分流）：
  `rec` 恒 0 → 线断 / 收发器 Standby / 不共地 / CAN_H、CAN_L 接反；
  `rec` 递增 → 听到位流但解不出帧 → 波特率、采样点或极性；可试降到 125k（Prescaler=16）。

## 2026-11-25 18:15 — 串口助手 SWD 复位可选目标（多调试器/多板子）

### 问题（如有）

电脑上同时插两块板（各带一个调试器）、开两个串口助手实例时，两个实例的
SWD 复位都只作用于同一块板，另一块完全没反应。根因：探测只取第一个匹配的
调试器，OpenOCD 命令里也没有 `adapter serial`，所以谁都没有"指定哪一块"的能力。

### 改动

- `Tools/SerialAssist/serial_assist.py::detect_debuggers` — 新增。枚举**全部**
  在位调试器，返回 `{iface, name, serial, vidpid, prio}` 列表。序列号取 Windows
  devnode id 末段（`USB\VID_0483&PID_374B\AE5A...`）；带 `&MI_` 的复合设备接口
  节点必须跳过，其末段是总线拓扑编号（`8&12225435&0&0000`），同型号会重名、
  重插还会变。按 `DEBUGGERS` 表顺序排序，保证"第一个"与旧行为一致。
- `Tools/SerialAssist/serial_assist.py::detect_debugger` — 改为调 `detect_debuggers()`
  取第一个，签名与行为保持不变（`--check` 等旧调用不受影响）。
- `Tools/SerialAssist/serial_assist.py::openocd_reset` — 新增 `serial=""` 参数；
  非空时在 `-f interface/*.cfg` 之后、`init` 之前插入 `-c "adapter serial <sn>"`。
  顺序不能提前，否则 adapter driver 还没注册会报 unknown command。
- `Tools/SerialAssist/serial_assist.py::_probe_debugger` — 改为枚举全部设备并存
  `self.ocd_debuggers` / `self.ocd_serial`；新增 `self.ocd_serial_missing`。
  计算 iface 的优先级变为：手写 `ocd_iface` > 选中设备类型 > 第一个设备 > 猜。
- `Tools/SerialAssist/serial_assist.py::_refresh_debugger_ready` — 新增，从
  `_probe_debugger` 抽出的 iface 脚本存在性/就绪计算，切换目标时单独调用。
- `Tools/SerialAssist/serial_assist.py::_apply_debugger_ui` — 增加下拉框同步；
  新增三种异常提示：锁定的调试器不在位、手写 `ocd_iface` 与设备类型不符、多台在位。
- `Tools/SerialAssist/serial_assist.py::_current_debugger` / `_dbg_labels` /
  `_sync_debugger_combo` — 新增，下拉框与序列号的映射与反查。
- `Tools/SerialAssist/serial_assist.py::_on_pick_debugger` — 新增。选完立刻
  `save_config` 落盘并切 iface。**同时更新内存 `self.cfg`**：`_probe_debugger`
  从 `self.cfg` 读 `ocd_serial`，只写磁盘会导致"刷新软件"把刚选的目标覆盖回去
  （验证时实测踩到，已修）。
- `Tools/SerialAssist/serial_assist.py::_rescan_debuggers` — 新增，绑定在
  `cb_dbg` 的 `<Button-1>`：点开下拉前重扫 USB，只更新设备列表，不动串口/日志。
- `Tools/SerialAssist/serial_assist.py::_build_fw_card` — 新增「复位目标」下拉
  （row 8），调试器状态提示行从 row 8 让到 row 9。
- `Tools/SerialAssist/serial_assist.py` 三处 `openocd_reset` 调用（手动复位、
  发送前复位、发完复位）统一传 `serial=self.ocd_serial`。
- `Tools/SerialAssist/serial_assist.py::on_close` — **修历史 bug**：原来是整份
  字典覆盖写，`ocd_iface` / `ocd_target` 这些不由界面维护的键会在关窗时被静默
  丢掉（"手写配置自己就没了"）。改为 `load_config()` 后 `update()` 再写。
- `Tools/SerialAssist/serial_assist.py::main`（`--check`）— 多打印两行：
  全部在位调试器清单与当前复位目标，打包后不接 GUI 也能核对。
- 副作用/注意：
  1. 锁定的调试器被拔掉时，按钮仍可点，但会报 `unable to find a matching
     CMSIS-DAP device` / `open failed` —— 有意不静默换成另一块板，避免复位错目标。
  2. 两个实例共用一份 `assist_config.json`，`ocd_serial` 也是共用的；运行期间
     各按内存里的选择走，重启后读到的是最后写入的那个。
  3. 不同设备类型必须用各自的 iface 脚本；若在 json 里手写了 `ocd_iface` 且与
     所选设备不符，提示行会红字警告（复位会失败）。
- 验证：
  1. 本机实测枚举到 2 台：`DAP-Link@3e5441d77b0dad97`、
     `ST-Link V2-1@AE5A5BB50000000000078851`，无 MI 伪串、无重复。
  2. 假序列号实测：DAP-Link 报 `unable to find a matching CMSIS-DAP device`、
     ST-Link 报 `open failed`，证明 `adapter serial` 确实在过滤。
  3. 真序列号实测（`init; shutdown`，不复位）：DAP-Link 侧
     `CMSIS-DAP: FW Version = 2.0.0`；ST-Link 侧 `STLINK V2J48M35 (API v2)
     VID:PID 0483:374B`。两个序列号各自锁定到不同调试器。
  4. 界面模拟两台设备：下拉三项、选 ST-Link 后 `ocd_name/ocd_iface/ocd_serial`
     同步切换、内存与磁盘配置一致；拔掉锁定的那台 → `ocd_serial_missing=True`
     且红字提示；插回 → 恢复；切回自动 → 清空序列号。
  5. `on_close` 测试：预先塞入 `ocd_target` 和自定义键，关窗后仍在（修复前会丢）。
  6. 源码 `py_compile` 与 `--check` 通过（9 个串口、2 台调试器）；
     exe 重新打包 18:14 并自检通过，`debuggers(2)` 两台都认到。
- 测试写入的 `ocd_serial` / `ocd_target` / 自定义键已从两份配置
  （`Tools/SerialAssist/assist_config.json`、`dist/assist_config.json`）中清除。

## 2026-11-25 19:33 — C8T6 诊断版：过滤器放宽成全通过 + 打印 LEC/FMP

### 问题

实机联调仍不通。C8T6 侧 stat 全 0（`rx=0 ovf=0 err=0 tec=0 rec=0`），ZET6 侧
`tec` 涨到 160、`code=-3`（三邮箱全堵）。但 `rec=0` 有两种完全不同的成因，只看计数器分不开：

1. 总线上真的没有差分跳变（发送端未驱动 / 线不通 / 共模超范围）；
2. **帧好好到了，但 ID 不匹配被硬件过滤器静默丢弃** —— CAN 的验收过滤器不匹配时
   直接丢弃且不报任何错，`REC` / `err` / `rx` 三个计数器全是 0，与「没信号」完全同形。

二者要修的东西天差地别（一个查线，一个查过滤器/ID），所以必须加一个能证伪的手段。

另外上一轮的判断也需要修正：上一版日志 `err=1` 而 `rec=255`，32 次以上接收错误只换了
1 次错误中断，说明那些错误集中在极短瞬间（更像上电瞬态），不是「每秒跟着 ZET6 一帧」
持续失败；且 `REC` 不会被软件清零、只有成功接收才 -1，所以 255 一直挂着是正常的「化石值」，
不能据此断言「位流到过 C8T6」。

### 改动

- `APP/Application/BSP/CAN/can_drv.h` — 新增开关 `CANDRV_DIAG_ACCEPT_ALL`（当前 =1）。
  置 1 时验收过滤器放宽成全通过（ID/掩码全 0），用来把「被过滤器丢掉」从「没信号」里摘出来。
  副作用：调试期间任意 ID 的帧都会进队列，定位完必须改回 0。只影响过滤器，
  不碰位时序/模式/中断，不引入新变量。
- `APP/Application/BSP/CAN/can_drv.c::CanDrv_Init` — 过滤器 ID/掩码赋值按开关分两路，
  其余（FilterBank/Mode/Scale/FIFO 分配/激活）不变。
- `APP/Application/BSP/CAN/can_drv.c::CanDrv_BusDiagnostics` — 签名扩为
  `(tec, rec, lec, fmp, last_err)`，新增两项只读读数：
  - `lec` = `ESR[6:4]`，最后一次错误类型。1=填充错(位时序) 2=格式错(极性) 3=ACK错(仅发送侧)
    4/5=位错 6=CRC错(噪声/终端)，0/7=无错。注意它会保持上一次的值直到软件写 111。
  - `fmp` = `RF0R[1:0]`，RX FIFO0 待读帧数。持续 >0 = 帧进了 FIFO 但中断没取走（中断路径问题）。
- `APP/Core/Src/main.c` — stat 打印新增 `lec=%u(名字)` 与 `fmp=%u`，LEC 数值直接翻成短名，
  日志里不用再对照手册；同时更新上方「只放行 0x601」的注释，说明当前处于全通过诊断模式。

### 验证

- APP 工程 36 个文件编译，零错误零警告；链接通过。
- FLASH 21212B（text 21120 + data 92）/ RAM 8548B；`.isr_vector` 仍在 `0x08005000`，烧录地址不变。
- 产物 `APP/build/Debug/APP_can_diag.bin`，21212B，crc32 `0x646C53F4`。**未上板。**

### 注意事项

- 这是临时诊断版本，定位结束后要同时做两件事：`CANDRV_DIAG_ACCEPT_ALL` 改回 `0`；
  删掉 `CanDrv_BusDiagnostics` 与 main 里的 stat 心跳。两者都不清会留下永久性的调试开销。
- `lec` 一旦非 0 会一直保持，**不能**用它判断「现在还在不在出错」；判断是否仍在出错要看
  `rec` / `err` 有没有继续增长。

## 2026-11-25 20:03 — 两侧 CAN 降速 500k→125k + 打印 BTR/MCR/PCLK1 实况

### 问题

CAN 物理层联调至今不通，现象是三方证据互相矛盾地"都正常但就是不通"：

- ZET6：`tx #N FAIL code=-3 err=0x02 tec=128~160 rec=0`，即帧在发、永远拿不到 ACK；
- C8T6：`rx=0 lec=1(stuff) rec=255 fmp=0`，即收到位流但一帧都解不出；
- 硬件：万用表实测两端 `CANH≈3.0V / CANL≈1.7V`（隐性 2.5/2.5、显性 3.5/1.5 的均值，
  对应 55~80% 显性占比 → 总线确实在被持续驱动）；C8T6 模块 RXD 对地 1.53V
  （说明位流已到达 PB8）；断电测两端各 120Ω、接上并联 60Ω（终端齐全）。
  供电、STB、极性、TX/RX 交叉、过滤器、终端电阻因此全部排除。

剩下两个可能，且方向完全不同，因此本轮用**一次烧录同时验证**：

1. 位时序寄存器是否真的一致 —— 日志里的 "500000 bps / 88.8%" 是拿 `hcan.Init`
   的**变量值**反算的，不是读寄存器；写入没生效时这行打印看不出来。
2. 500k 下沿口裕量是否不足 —— 终端正确不代表没有振铃（长分支、非双绞、收发器边沿慢
   都会让采样点踩在振铃上），其表现正是 stuff error。

### 改动

降速（两侧对称，各 3 处，缺一处 `CanDrv_Init()` 就返回 -6）：

- `APP/APP.ioc` / `host_computer_ZET6/host_computer_ZET6.ioc` — `CAN.Prescaler` 4→16，
  `CAN.CalculateBaudRate` 500000→125000，`CAN.CalculateTimeBit` 2000→8000，
  `CAN.CalculateTimeQuantum` 111.11→444.44。改 .ioc 是为了下次 CubeMX 重新生成不回退。
- `APP/Core/Src/can.c::MX_CAN_Init` / `host_computer_ZET6/Core/Src/can.c::MX_CAN_Init` —
  `hcan.Init.Prescaler = 16`。BS1/BS2/SJW 不动，仍是 18tq、采样点 88.9%，
  36MHz/(16×18) = 125kbps。**副作用：OTA 带宽约 11KB/s，20KB 固件约 2 秒，够用。**
- `APP/Application/BSP/CAN/can_drv.h` / `...ZET6/.../can_drv.h` — `CANDRV_EXPECT_BAUD`
  500000→125000（该校验基准不写寄存器，但决定 `CanDrv_Init()` 的读回校验是否通过）。

新增寄存器实况打印（只读，不改任何行为）：

- 两侧 `can_drv.h` — 新声明 `void CanDrv_TimingRegs(uint32_t *btr, uint32_t *mcr, uint32_t *pclk1)`。
- 两侧 `can_drv.c` — 实现该函数：读 `CAN->BTR`（含 bit30=LBKM 回环、bit31=SILM 只听）、
  `CAN->MCR`（bit0=INRQ 卡在初始化模式）、`HAL_RCC_GetPCLK1Freq()`。
  三个值必须一起看：BTR 只是分频系数，脱离 PCLK1 算不出波特率。
  预期（125k/36MHz）：`btr=0x001E0010 mcr=0x00010000 pclk1=36000000`。
- `APP/Core/Src/main.c` — stat 心跳行尾追加 `btr=0x%08lX mcr=0x%08lX pclk1=%lu`。
- `host_computer_ZET6/Core/Src/main.c` — init OK 之后补一行 `[can] regs: ...`，
  与 C8T6 的数值直接对照。

### 验证

- APP：36 文件编译零错误零警告，链接通过；FLASH 21216B + data 92 / RAM 8548B；
  `.isr_vector` 仍在 `0x08005000`。产物 `APP/build/Debug/APP_can_125k.bin`，
  21308B，crc32 `0xF7414D71`。
- ZET6：38 文件编译零错误零警告，链接通过；FLASH 19900B + data 96 / RAM 8280B。
  产物 `host_computer_ZET6/build/Debug/ZET6_can_125k.bin`，19996B，crc32 `0x78ABC2BC`。
- **两侧均未上板。**

### 上板判读

- 若 125k 通了（ZET6 出现 `tx #N OK`、C8T6 出现 `[can] rx id=0x601`）→ 原问题是
  500k 下的信号裕量；可在 125k 上继续 OTA，或后续再逐步提速试 250k。
- 若 125k 仍报 `lec=1(stuff)` → 与速率无关，看新打印的 `btr`：
  两侧不一致 = 写入没生效（旧二进制 / MX_CAN_Init 早于时钟配置）；
  `btr` 的 bit30/bit31 被置位 = 外设实际不在 Normal 模式；
  `mcr` bit0=1 = 卡在初始化模式。三种都能一眼定性。
- 收尾必做：定位完成后删 `CanDrv_TimingRegs`、`CanDrv_BusDiagnostics` 与 main 的 stat 心跳，
  并把 `CANDRV_DIAG_ACCEPT_ALL` 改回 0。

## 2026-11-25 20:22 — 两侧加引脚电平采样（IDR）+ ZET6 补 LEC，12.5k 下仍不通的定位手段

### 问题

125k 联调仍不通，正向与反向接线都试过，均无改善（因此 CAN_H/CAN_L 交叉
这一假设已被排除）。此时事实清单：

- 位时序层完全对齐：两侧 `btr=0x001E000F`、`pclk1=36000000`，
  `SystemClock_Config` 逐字相同（HSE 8M×9、APB1 DIV2）；
- `mcr=0x00010048`，INRQ=0，未卡在初始化模式；
- 重映射正确：`__HAL_AFIO_REMAP_CAN1_2()` 注释即 `CAN_RX→PB8, CAN_TX→PB9`
  （HAL 编号反直觉，`CAN1_1` 才是不重映射的 PA11/PA12），
  `HAL_MspInit` 里已使能 AFIO 时钟，写 MAPR 不会静默失败；
- 终端 60Ω、供电正常、模块 RXD 有活动（1.53V）；
- C8T6：`rx=0 rec=255 lec` 在 stuff/crc/form 间轮换；ZET6：`tec` 涨到 218、`rec=0`。

于是只剩一个环节从未被**直接**测过：CAN 控制器的引脚上到底有没有位流。
此前全靠推理（模块 RXD 有电压 → 以为线通），但模块 RXD 与 MCU 的 PB8
是两回事，中间那一段断没断从未验证。

附带发现一个此前一直在误导排查的缺陷：`Uart_Printf` 内部是 `char String[100]`，
超长会被 vsnprintf 静默截断。上一版 stat 行 170+ 字符，`btr` 只打出一半
（`0x001E00`），导致读数需要靠猜。

### 改动

- 两侧 `Application/BSP/CAN/can_drv.h` — 新增 `void CanDrv_SamplePins(uint32_t samples, uint16_t *lo8_pm, uint16_t *lo9_pm)`。
- 两侧 `Application/BSP/CAN/can_drv.c::CanDrv_SamplePins` — 在采样窗口内逐次读
  `GPIOB->IDR`，统计 PB8/PB9 的低电平千分比。原理：引脚交给 CAN 外设后，
  IDR 读的仍是**引脚本身**的电平（不是 ODR），因此不改引脚模式、不影响收发，
  就能判断该引脚上有没有跳变。循环体约 6~8 周期，50000 次 ≈ 5ms
  （8 字节标准帧 @125k 约 1ms，5ms 覆盖多帧，不会正好踩进帧间隙）。
  `IDR` 声明为 `__IO`，逐圈真读；若缓存到循环外会被优化成只读一次。
- `host_computer_ZET6/Application/BSP/CAN/can_drv.h/.c::CanDrv_BusDiagnostics` —
  签名由 `(tec, rec, last_err)` 扩为 `(tec, rec, lec, last_err)`，新增 `lec = ESR[6:4]`。
  理由：只看 `tec` 涨 + `rec=0` 无法区分「没人应答」和「位流本身坏了」
  （错误主动阶段的 ACK 错误计入 TEC，两者都会让 TEC 涨），而这两者的修复方向相反。
- `APP/Core/Src/main.c` — stat 心跳拆成四行（`stat` / `stat2` / `reg` / `pin`），
  每行都控制在 100 字节内；新增 `CanDrv_SamplePins(50000U, ...)`，输出 `pb8=NNNpm pb9=NNNpm`。
- `host_computer_ZET6/Core/Src/main.c` — FAIL 分支新增 `lec`、`pb8`、`pb9` 三个字段，
  并在发送超时后立即采样（此时 ARD 仍开着，总线通常还在重传，5ms 窗口几乎必然落在活动期）。

### 验证

- APP：36 文件零错误零警告，链接通过；FLASH 21428B + data 92 / RAM 8548B；
  `.isr_vector` 仍在 `0x08005000`。产物 `APP/build/Debug/APP_pinprobe.bin`，21520B，crc32 `0x1C8DE233`。
- ZET6：38 文件零错误零警告，链接通过；FLASH 20044B + data 96 / RAM 8280B；
  `.isr_vector` 在 `0x08000000`。产物 `host_computer_ZET6/build/Debug/ZET6_pinprobe.bin`，
  20140B，crc32 `0xC507C149`。
- **两侧均未上板。**

### 上板判读（这次是直接测量，不再是推理）

`pm` = 千分比。恒 0 或 1000 表示该引脚在 5ms 内一次都没变过。

| ZET6 pb9 | ZET6 pb8 | C8T6 pb8 | 结论 |
|---|---|---|---|
| 有值 | 有值 | 有值 | 两根线上都有真实位流 → 不是连线问题，落在速率/相位/边沿质量，下一步只能示波器或缩短并绞线 |
| 有值 | 有值 | 0/1000 | C8T6 的 RX 段没通（模块 RXD 到 PB8 之间）→ 查那一段线/焊点 |
| 有值 | 0/1000 | — | ZET6 收发器回读段断了（PB8 收不到自己发的）→ 板载 U7 到 PB8 的走线或 RXD 输出 |
| 0/1000 | — | — | 帧连 MCU 引脚都没出 → 控制器/邮箱层面异常，与总线无关 |

配合 `lec`：`3(ack)` 且两根线都有值 = 纯粹缺少应答（对端没解出帧）；
`1(stuff)/2(form)/6(crc)` = 位流被破坏，按上表继续分流。

### 收尾必做

`CanDrv_SamplePins`、`CanDrv_BusDiagnostics`、main 里的 stat 心跳与
`CANDRV_DIAG_ACCEPT_ALL` 全是临时诊断设施，定位结束后一并删除 / 改回 0。

## 2026-11-25 20:49 — 删除全部 CAN 诊断设施，只保留接收数据打印

### 问题

物理层排查期间在两侧堆了一批临时诊断（状态心跳、引脚采样、寄存器实况、全通过过滤器）。
其中一部分结论已经拿到（`btr=0x001E000F` 证明位时序同源、`pb8` 证明引脚状态），
继续留在树上有两个害处：全通过过滤器会让**总线上任何 ID 的帧都进队列**，
将来做 OTA 时别人的帧会被当成本机请求；四行 5 秒一次的心跳会持续占用串口带宽。
因此按用户要求全部删除，只保留"收到就打印"这一条功能。

### 改动

C8T6（`APP/`）：

- `Application/BSP/CAN/can_drv.h` — 删除 `CANDRV_DIAG_ACCEPT_ALL` 开关，以及
  `CanDrv_BusDiagnostics` / `CanDrv_TimingRegs` / `CanDrv_SamplePins` 三个声明。
- `Application/BSP/CAN/can_drv.c` — 删除上述三个实现、`s_last_err` 存档变量
  与其在 `HAL_CAN_ErrorCallback` 里的赋值；过滤器去掉 `#if` 诊断分支，
  恢复成只放行 `CANDRV_ID_REQ`(0x601) 的精确匹配。
  ⚠ 副作用：`can_drv.c` 因此回到与上次提交**完全一致**的内容（git status 已不再列出该文件），
  等于诊断代码从未存在过 —— 这是删除彻底的一个旁证。
- `Core/Src/main.c` — 删除 `can_rx_cnt` / `s_can_stat_t` 两个变量和整段 5 秒心跳
  （stat / stat2 / reg / pin 四行 print）。`CanDrv_RxPop` 循环与 `[can] rx ...` 打印保留，
  这是本阶段唯一需要的输出。

ZET6（`host_computer_ZET6/`）：

- `Application/BSP/CAN/can_drv.h/.c` — 同样删除 `BusDiagnostics`（含 lec）、
  `TimingRegs`、`SamplePins` 与 `s_last_err`。
- `Core/Src/main.c` — 删除开机 `[can] regs:` 打印；FAIL 分支退回
  `"[can] tx #%lu FAIL c=%d err=%lu bof=%lu"`，不再采样引脚与 LEC；
  OK 分支保持原样（它用的是保留的计数器，属于运行信息而非诊断）。

### 验证

- APP：36 文件零错误零警告，链接通过；FLASH 20700B + data 92 / RAM 8540B；
  `.isr_vector` 仍在 `0x08005000`。产物 `APP/build/Debug/APP_can_rxonly.bin`，
  20792B，crc32 `0x0279096D`。
- ZET6：38 文件零错误零警告，链接通过；FLASH 19672B + data 96 / RAM 8280B。
  产物 `host_computer_ZET6/build/Debug/ZET6_can_txonly.bin`，19768B，crc32 `0x53552E72`。
- 用 `nm` 确认两侧 ELF 里 `CanDrv_BusDiagnostics` / `CanDrv_TimingRegs` /
  `CanDrv_SamplePins` 三个符号**均已不存在**，`CanDrv_RxPop` 与 `HAL_CAN_ErrorCallback` 仍在。
- **两侧均未上板。**

### 注意事项

- 诊断代码从未提交过 git，删掉后无法用 `git checkout` 找回。若将来又要做物理层定位，
  按本条与 20:03 / 20:22 两条的说明重建即可（三处读数各有明确的判据）。
- 上板后若出现"上位机 tx OK 但本侧没有 `[can] rx`"，说明帧到了并被 ACK、但没进队列，
  问题在验收过滤器或 RX0 中断这一段，与总线物理层无关 —— 那时只需在
  `HAL_CAN_RxFifo0MsgPendingCallback` 里加一个计数，不必再恢复整套诊断。

## 2026-11-25 21:15 — CAN 波特率 125k → 50k（两侧同步，纯判别试验）

### 问题（如有）

上板后现象：ZET6 侧 `[can] tx #N OK` 连续成功（判据是 `TSR` 的 `TME`，且 `NART=0` 自动重传开着，
所以 OK 等价于"收到 ACK"），而 C8T6 侧一行 `[can] rx` 都没有。

用 SWD 热连接（`mode=hotplug`，不复位、不改代码）直读 C8T6 的 CAN1 寄存器（基址 `0x40006400`，
顺序 **MCR/ MSR/ TSR/ RF0R/ RF1R/ IER/ ESR/ BTR**）与驱动静态变量，得到：

| 项 | 值 | 判读 |
|---|---|---|
| `RF0R` | `0` | FIFO0 空、无溢出 |
| `IER` | `0x00008602` | `FMPIE0=1`，RX0 中断确实开着 |
| `ESR` | `0xFF000023` / `0xFF000063` | REC=255、TEC=0、EPVF=1，LEC 在 form(2)/crc(6) 间跳 |
| `BTR` | `0x001E000F` | 125k、88.9%，与 ZET6 逐位一致 |
| `MSR` | `0x00000C00` | bit11 RX=1（隐性）、RXM=1 |
| `AFIO_MAPR` | `0x02004000` | CAN remap=10 → PB8=RX / PB9=TX |
| `GPIOB CRH/IDR` | PB8=input floating、PB9=AF_PP；PB8=1、PB7=0 | 引脚与 STB 全对 |
| RAM `s_rx_head/tail` | `0/0` | 软件队列一帧都没进过；`s_err_count=1`、`s_rx_overflow=0` |

配套的物理实验：
- 拔掉 C8T6 的 CAN 线 → ZET6 **立刻变 FAIL** ⇒ ACK 100% 来自 C8T6，锁定故障在 C8T6 接收端。
- 模块只有 `3V3/GND/CTX/CRX/CH/CL` 六个脚（3.3V 收发器，SN65HVD230 一类），供电型号排除。
- PB8 空闲实测 3.3V；**ZET6 断电 + 复位 C8T6 后 13s，ESR 仍是 `0x00000000`（REC=0、LEC=none）**
  ⇒ PB8 上干净、CRX→PB8 线是通的，噪声/断线假设排除；错误只由"真的来帧"触发。

结论：C8T6 的接收端在解码真帧时位级出错（CRC 通过时还能正常发 ACK，所以 ZET6 一直 OK，
但帧随后被丢弃、进不了 FIFO）。500k 与 125k 下都完全收不到 —— 降 10 倍速度没有改善。
因此做一次单纯的速度判别：**两侧同步降到 50k**。

### 改动

- `APP/APP.ioc`、`host_computer_ZET6/host_computer_ZET6.ioc` —
  `CAN.Prescaler` 16→40，`CalculateBaudRate` 125000→50000，`CalculateTimeBit` 8000→20000，
  `CalculateTimeQuantum` 444.44→1111.11。`BS1=15TQ` / `BS2=2TQ` 不动，采样点仍 88.9%。
- `APP/Core/Src/can.c`、`host_computer_ZET6/Core/Src/can.c` — `hcan.Init.Prescaler = 40`，
  注释写明这是判别试验而非最终配置。
- `APP/Application/BSP/CAN/can_drv.h`、`.../can_drv.h` — `CANDRV_EXPECT_BAUD` 125000→**50000**。
  ⚠ 三处必须同步，少一处 `CanDrv_Init()` 直接返回 -6。
- 未改任何其他逻辑：过滤器仍只放行 `0x601`，主循环仍只打印 `[can] rx ...`。

### 验证

- 两侧均编译通过（APP 36 文件、ZET6 38 文件，FAIL 0），链接零错误。
- `objdump` 确认 `movw #50000`（`CANDRV_EXPECT_BAUD`）与 `MX_CAN_Init` 里的 `#40`（Prescaler）
  都已编入两侧固件。
- 产物：
  - `APP/build/Debug/APP_can_50k.bin` — **20788B，crc32 `0xB27FF921`**，FLASH 20696B + data 92，
    烧 `0x08005000`。
  - `host_computer_ZET6/build/Debug/ZET6_can_50k.bin` — **19768B，crc32 `0x434630DB`**，
    FLASH 19672B + data 96，烧 `0x08000000`。
- **两侧均未上板。**

### 注意事项

- 这是**判别试验**，不是最终配置。判读方式：
  - 50k 能收到 → 速度相关（带宽、上升沿、反射、串扰、终端位置），回物理层查；
  - 50k 表现完全一样（REC 仍冲 255、LEC 仍 crc/form）→ 速度无关，查晶振/时钟树
    （读 `RCC_CFGR` 的 `SWS/PLLSRC/PLLMUL` 确认没有跑在 HSI 上）与 BTR。
- 测完必须按结论回改速率（最终大概率回 125k），三处照上表一起改回。
- SWD 读数命令可直接复用（不装代码也能看现场）：
  `STM32_Programmer_CLI -c port=SWD freq=4000 mode=hotplug -u 0x40006400 0x20 out.bin`

## 2026-11-25 21:20 — 判别试验：C8T6 改 SILENT（只听）模式

### 问题（如有）

50k 联调后 C8T6 侧 `ESR=0xFF000053`，**`LEC=5`（bit dominant error）**：自己发了显性位，
却在自己的接收端读到隐性。C8T6 在这条总线上唯一会发的显性位就是 ACK，因此这条错误码说明
**ACK 确实发出去了（ZET6 因此判 `tx OK`），但这一位没回到自己的接收端，整帧随即被丢弃**，
FIFO 永远为空、一行 `[can] rx` 都没有。为把"发 ACK 时破坏自身接收"与"接收链本身坏"分开，
把 C8T6 改成 SILENT 模式：硬件既不发 ACK、也不发错误帧，只接收。

### 改动

- `APP/APP.ioc` — `CAN.Mode` `CAN_MODE_NORMAL` → **`CAN_MODE_SILENT`**。
- `APP/Core/Src/can.c` — `hcan.Init.Mode = CAN_MODE_SILENT`，注释写明是判别试验及判读方式。
- `APP/Core/Src/main.c` — `[can] ... mode:` 那行把固定字符串 `"normal"` 改成按
  `hcan.Init.Mode & CAN_BTR_SILM` 动态选 `"silent"` / `"normal"`。
  原因：`CanDrv_GetMode()` 只看 `LBKM` 位，区分不了 SILENT 与 NORMAL，改这里才能从串口一眼
  确认"烧进去的是哪一版"。**副产物：`[can] normal mode: 接收就绪…` 这行的匹配串以后会随模式变化。**
- ZET6 侧**一行未改**（仍是 NORMAL / 50k），因为试验只要求单侧改变。
- 位时序、过滤器（仍只放行 `0x601`）、主循环接收打印逻辑均未动。

### 验证

- `python Tools/verify_build.py APP` → COMPILE 36 files，FAIL 0，LINK rc=0。
- `objdump -d --disassemble=MX_CAN_Init` 确认 `mov.w r3, #2147483648 (0x80000000)` 已写入
  `Init.Mode`；`.rodata` 里出现 `silent\0` 字符串。
- 产物：`APP/build/Debug/APP_can_silent.bin` — **20820B，crc32 `0x7BB8A8EC`**，烧 `0x08005000`。
- **已烧入 C8T6 实机并复位运行**，SWD 读回确认：
  `MCR=0x00010048`、`IER=0x00008602`、`BTR=0x801E0027`（**bit31 SILM=1、BRP=39→PSC40**）、
  `ESR=0x00000000`、`RF0R=0`（连续 5 拍采样完全静止）。

### 注意事项

- ⚠ **烧录后 `-c port=SWD -d ... -v` 结束时芯片可能停在复位早期**（实测读到
  `RCC_CFGR=0`、`APB1ENR=0`、`BTR=0x01230000` 等复位默认值，会被误判成"CAN 没初始化"）。
  遇到这种读数先执行一次 `-c port=SWD freq=4000 -rst`，等 12s 再读。
- 判读方式（见下一轮结论）：SILENT 下若打印出 `[can] rx ...` → 病因是"发 ACK 破坏自身接收"，
  换收发器模块；若仍无帧且 `ESR=0` → 接收链本身无信号，同样指向模块/CRX 走线，与固件无关。
- 试验结束必须把 `CAN.Mode` 与 `can.c` 一起改回 `CAN_MODE_NORMAL`（两处）。

## 2026-11-25 21:28 — C8T6 回退 NORMAL（SILENT 结论已定，实机复现闭环）

### 问题（如有）

21:20 的 SILENT 判别已给出结论（C8T6 在 SILENT 下连续收到 14 帧且数据全对，NORMAL 下必定
`LEC=5`、`REC→255`、整帧丢弃）。用户手上无备件模块，要求改回 NORMAL 再烧一次做确认。

### 改动

- `APP/APP.ioc` — `CAN.Mode` `CAN_MODE_SILENT` → **`CAN_MODE_NORMAL`**。
- `APP/Core/Src/can.c` — `hcan.Init.Mode = CAN_MODE_NORMAL`，注释改为记录 SILENT 试验的结论
  与"换模块后直接跑这版即可"。
- `APP/Core/Src/main.c` — 那行动态取名的日志**保留**（`silent`/`normal` 按 `SILM` 位判断），
  便于以后再做单侧 SILENT 判别时从串口直接确认固件版本。其它逻辑一行未动。
- ZET6 侧仍未改动（NORMAL / 50k / 1s 一帧 / ARD 开）。

### 验证

- 编译通过（36 文件，FAIL 0，LINK rc=0）；`objdump` 确认 `MX_CAN_Init` 里 `Init.Mode`
  写的是 `movs r3, #0`（= `CAN_MODE_NORMAL`）。
- 产物：`APP/build/Debug/APP_can_normal50k.bin` — **20816B，crc32 `0x0DAF7A03`**，烧 `0x08005000`。
- **已烧入实机 + 复位，12 秒后 SWD 读回**：
  `MCR=0x00010048`、`IER=0x00008602`、`BTR=0x001E0027`（SILM=0、PSC40 ✓）、
  `RF0R=0`、**`ESR=0xFF000053`（REC=255、TEC=0、LEC=5 bit dominant error）**。
  与 21:20 之前 NORMAL 的表现逐位一致 —— **换回 NORMAL 后故障 100% 复现**，
  而 SILENT 下同一硬件能全收，两次观测只差 `Mode` 一个变量，结论闭环。

### 注意事项

- 当前这版**不能用**：C8T6 会持续丢帧，ZET6 因收到 ACK 而误判 `tx OK`。
- 未解决，硬件层面待办：换 3.3V CAN 收发器模块；或量模块 CRX 脚对地稳态电压
  （若 ≈1.5V 这种中间值，说明 RXD 被别的东西拉着/推挽冲突，属于接线或板上器件问题）。

## 2026-11-25 21:36 — 判别试验二：两侧降到 25k（位宽 20µs → 40µs）

### 问题（如有）

用户确认 **CRX 与 PB8 直连、中间无串阻、无并联输出，模块引脚与 PB8 均为 3.3V**，
"两个推挽输出打架"的猜测被排除。而 ZET6 与 C8T6 的采样点完全相同（同为 88.9%），
ZET6 在 17.8µs 处读到了 ACK 显性（所以 `tx OK`），C8T6 在同一时刻读到的却是隐性。
总线显性 + 直连 + 电平正常 ⇒ 只能是**模块在自己驱动期间把 RXD 抑制/滞后了，且滞后 > 17.8µs**。
为区分"抑制窗口是固定时长"与"整个发送位都被屏蔽"，把位宽拉长一倍。

### 改动

- `APP/APP.ioc`、`host_computer_ZET6/host_computer_ZET6.ioc` —
  `CAN.Prescaler` 40→**80**、`CalculateBaudRate` 50000→**25000**、
  `CalculateTimeBit` 20000→**40000**、`CalculateTimeQuantum` 1111.11→**2222.22**。
  `BS1=15TQ` / `BS2=2TQ` 不动，采样点仍 88.9%（绝对时间从 17.8µs 变为 35.6µs）。
- 两侧 `Core/Src/can.c` — `hcan.Init.Prescaler = 80`，注释写明本次试验的假说与判读方式。
- 两侧 `Application/BSP/CAN/can_drv.h` — `CANDRV_EXPECT_BAUD` 50000→**25000**。
- 未改任何逻辑：过滤器仍只放行 `0x601`，`Mode` 保持 NORMAL，主循环打印不变。

### 验证

- 两侧编译通过（APP 36 文件、ZET6 38 文件，FAIL 0，LINK rc=0）。
- `objdump` 确认两侧 `MX_CAN_Init` 里都有 `movs #80 (0x50)`。
- 产物：
  - `APP/build/Debug/APP_can_25k.bin` — **20816B，crc32 `0x561B419F`**，烧 `0x08005000`。
  - `host_computer_ZET6/build/Debug/ZET6_can_25k.bin` — **19768B，crc32 `0x6197147E`**，烧 `0x08000000`。
- C8T6 **已烧入并复位**，SWD 读回 `BTR=0x001E004F`（BRP=79 → PSC=80 ✓）。
  此时 ZET6 仍是 50k，两侧速率不匹配，`ESR=0xFF000053` 属预期，**无判定意义**。

### 注意事项

- ⚠ **两侧必须同时是 25k**，一侧 50k 一侧 25k 会完全不通且 `REC` 直接顶满。
- 判读：25k 下 C8T6 能打印 `[can] rx ...` 且 `LEC` 不再固定为 5 → 抑制窗口是固定时长（情况 A），
  可先用 25k 跑 OTA（约 2.2KB/s，20KB 固件约 9s）；若仍是 `LEC=5` → 整位屏蔽（情况 B），只能换模块。
- 测完按结论回改速率（最终大概率回 125k 或换模块后重定），三处照上表一起改。

## 2026-11-25 21:40 — 25k 判定结果：情况 B（整位屏蔽），速率无效

### 结果

两侧均烧 25k 并复位运行，ZET6 侧 `[can] init OK, 25000 bps` 且持续 `tx #N OK err=2`（有 ACK），
C8T6 侧 `[can] init OK, 25000 bps` + `接收就绪`，但**没有任何一行 `[can] rx`**。

C8T6 SWD 读回（连采 3 拍，完全静止）：

| 项 | 50k | **25k** |
|---|---|---|
| `BTR` | `0x001E0027` | `0x001E004F`（BRP=79 → PSC=80 ✓） |
| `RF0R` | 0 | **0** |
| `ESR` | `0xFF000053` | **`0xFF000053`** |
| `LEC` | 5 | **5（一字未变）** |
| `REC` | 255 | **255** |

**位宽从 20µs 拉到 40µs，错误码完全没有变化** ⇒ 抑制/滞后与时间无关，不是固定时长的模拟延迟，
而是"驱动期间 RXD 被完全屏蔽/强制隐性"（情况 B）。25k 这条路走不通，已回退结论。

### 结论

这块收发器模块**无法用于任何需要发送的 CAN 节点**（ACK 是接收方必发的位）。它能收（SILENT 下
14 帧全对）、能驱动总线（对端收到 ACK），唯独不能回读自己驱动的位。

### 待办（按优先级）

1. **确认模块上 8 脚芯片的丝印型号**。若为 `TJA1050` / `PCA82C250` / `TJA1042` 等 **5V 器件**，
   则此前"3V3 供电"就是根因 —— 5V 芯片在 3.3V 下驱动差压只有 1.3V（用户实测 H≈3.0 / L≈1.7，
   正常应为 2.0V），完全符合"能发不能回读"的退化表现。此时改接 5V 供电（PB8 是 FT 引脚，
   可容忍 5V）大概率直接解决。**但若是 SN65HVD230 / VP230 这类 3.3V 器件，切记不可接 5V（会损坏）。**
2. 换一块确认可用的 3.3V 收发器模块。
3. 权宜：C8T6 常驻 SILENT + ZET6 关自动重传（`AutoRetransmission = DISABLE`），
   做单向传输，先跑通 OTA 数据通道（无块确认）。

### 21:42 补充：模块芯片丝印确认为 `VP230`（SN65HVD230，3.3V 器件）

- 供电 3.3V 是**正确**的，5V 方案作废（接 5V 会烧毁该器件，绝对禁止）。
- 因此"5V 芯片被 3.3V 供电"的根因假说排除，剩下的解释只有：这颗 `VP230` 是仿制/降级品
  （市面上大量用 5V die 打磨后标 VP230），其"驱动显性时回读自身 RXD"的模拟功能失效。
  这与全部观测一致：能收（SILENT 14 帧全对）、能驱动总线（对端收到 ACK）、差压偏低（1.3V，
  低于 3.3V 器件典型 2.0V）、`LEC=5` 且对位宽不敏感（25k/50k/125k 均如此）。
- **下一步零成本验证：把 C8T6 与 ZET6 两块模块互换。** 关键理由：ZET6 那块模块**从未被验证过
  "发送时回读"**——它全程只做发送（C8T6 从没回过帧），所以它很可能是好的。
  互换后若 C8T6 能正常收帧 → 确认原 C8T6 模块损坏；若仍不能 → 两块同批都有问题。
  （21:44 用户说明 ZET6 是**板载**收发器，无法互换，此项作废。）

## 2026-11-25 21:55 — 结案：C8T6 用飞线接到另一块板子的收发器后，25k 下接收正常

### 做法

用户把 C8T6 的 CAN 信号**飞线**到另一块板子上的 CAN 收发器（弃用原 `VP230` 模块），
两侧仍为 25k / NORMAL，未改动任何代码。

### 结果

C8T6 串口连续输出：

```
[can] init OK, 25000 bps, sample 88.8%, normal
[can] normal mode: 接收就绪，等待上位机 0x601
[can] rx id=0x601 dlc=8 data=A5 5A 05 00 00 00 01 00
[can] rx id=0x601 dlc=8 data=A5 5A 06 00 00 00 01 00
[can] rx id=0x601 dlc=8 data=A5 5A 07 00 00 00 01 00
```

SWD 读回（连采 3 拍，全部一致）：

| 项 | 坏模块（25k） | **飞线后（25k）** |
|---|---|---|
| `BTR` | `0x001E004F` | `0x001E004F` ✓ |
| `ESR` | `0xFF000053` | **`0x00000000`** |
| `TEC/REC` | 0 / **255** | **0 / 0** |
| `LEC` | 5（bit dominant） | **0（none）** |
| `RF0R` | 0 | 0（帧被主循环及时取走） |

### 结论（结案）

**故障件就是那颗 `VP230` 模块，固件、接线、位时序、过滤器、时钟、终端电阻全部正常，无一需要修改。**
`REC` 从 255 常驻变成 0 并稳定保持，是"发送/回读路径恢复"的最直接证据。

### 后续

- 当前两侧均为 **25k / NORMAL**，通信已通。25k 下 OTA 约 2.2KB/s（20KB 固件约 9s）。
- 若要恢复原定速率，需三处 × 两侧同步改回 125k（`PSC=16`）：`.ioc` 的 `Prescaler`/`CalculateBaudRate`、
  `can.c` 的 `Prescaler`、`can_drv.h` 的 `CANDRV_EXPECT_BAUD`。
- 飞线属临时接线，升速前建议缩短走线并尽量双绞，125k 对信号质量比 25k 敏感。

## 2026-11-25 21:59 — 恢复 125k（两侧同步），兼作飞线边沿裕量验证

### 问题（如有）

`VP230` 模块换掉后，25k 下通信已正常（`ESR=0`、`REC=0`、连收多帧正确）。但 25k 只是诊断期的
降速试验值，OTA 吞吐约 2.2KB/s（20KB 固件约 9s），偏慢；原定目标速率是 125k（约 11KB/s）。

### 改动（三处 × 两侧，共 6 个文件，逻辑一行未动）

| 位置 | 旧 → 新 |
|---|---|
| `APP/APP.ioc`、`host_computer_ZET6/host_computer_ZET6.ioc` | `Prescaler` 80→**16**、`CalculateBaudRate` 25000→**125000**、`CalculateTimeBit` 40000→**8000**、`CalculateTimeQuantum` 2222.22→**444.44** |
| `APP/Core/Src/can.c`、`host_computer_ZET6/Core/Src/can.c` | `hcan.Init.Prescaler = 16`，并把堆积的多段降速试验注释合并为一段结论式说明 |
| `APP/Application/BSP/CAN/can_drv.h`、`host_computer_ZET6/Application/BSP/CAN/can_drv.h` | `CANDRV_EXPECT_BAUD` 25000→**125000UL** |

`BS1=15TQ / BS2=2TQ` 不变 → 采样点仍 88.9%（`36MHz / (16×18) = 125kbps`）。
`CAN.Mode` 两侧均为 `CAN_MODE_NORMAL`；过滤器仍只放行 `0x601`；ZET6 仍每 1s 发一帧 `0x601`。

### 验证

- `python Tools/verify_build.py`：两侧全过（APP 36 文件 / ZET6 38 文件，FAIL 0，LINK rc=0）。
- `objdump` 复核：两侧 `MX_CAN_Init` 内均为 `movs #16`（分频），`125000` 常量各 1 处。
- C8T6 已烧 `0x08005000` 并 `-rst`；SWD 读回 `BTR=0x001E000F`（BRP=15 → PSC=16）✓。
  读到的 `ESR=0xFF000013`（TEC=19、REC=0、LEC=1 stuff）是因为此刻 **ZET6 还跑在 25k**、
  两侧速率不匹配，**无判定意义**，烧完 ZET6 后两侧都复位再看。

### 产物

| 文件 | 大小 | crc32 | 烧录地址 |
|---|---|---|---|
| `APP/build/Debug/APP_can_125k.bin` | 20820B | `0xFB159A69` | `0x08005000` |
| `host_computer_ZET6/build/Debug/ZET6_can_125k.bin` | 19768B | `0x53552E72` | `0x08000000` |

`ZET6_can_125k.bin` 的 crc32 与更早的 `ZET6_can_txonly.bin` 完全一致，符合预期（ZET6 侧
除了 Prescaler 回到 16 之外没有任何改动，等于回到 125k 版本的同一份二进制）。

### 判读与回退

- 125k 下 `ESR=0`、`REC=0`、C8T6 持续打印 `[can] rx id=0x601 ...` → 定在 125k。
- 出现 `LEC≠0`、`REC` 上涨或丢帧 → 说明临时飞线撑不住 125k 边沿，回退 25k（`PSC=80`），
  等收发器模块走正规接线后再提速。

### 实机验证（22:05，用户烧完 ZET6 后）

C8T6 串口连续输出 `[can] rx id=0x601 dlc=8 data=A5 5A 05 00 00 00 01 00`（序号递增循环），
SWD 连采 3 拍全部为：

| 项 | 值 | 含义 |
|---|---|---|
| `BTR` | `0x001E000F` | PSC=16 → 125k ✓ |
| `ESR` | **`0x00000000`** | TEC=0、REC=0、LEC=0，**零错误** |
| `RF0R` | `0` | 帧被主循环及时取走，无积压 |

**结论：125k 在临时飞线上可用，零错误码。** 但注意这只验证了 **1 帧/秒** 的低速场景；
OTA 是连续背靠背突发（125k 下 8 字节帧约 0.72ms 间隔），FIFO 只有 3 级、
且 C8T6 主循环每帧打一行串口（115200 下约 5ms）会严重阻塞 —— **连续突发必须单独压测**，
见下一步计划。

## 2026-11-26 16:15 — ZET6 移除 CAN 周期测试帧（不再每秒发一帧）

### 问题（如有）

用户要求关掉 CAN 总线上持续发送的测试代码：ZET6 主循环每 1 秒发一帧 `0x601`，
既一直占着总线，又不断往串口刷 `[can] tx #N` 日志，干扰串口落盘观察。
物理层已在 11-25 22:05 验证通过（125k，`ESR=0` 连采 3 拍），不再需要常驻心跳。
用户同时明确 **阶段 1c 突发压测不做了**，因此不在代码里预留压测专用的节拍/序号变量。

### 改动

- `host_computer_ZET6/Core/Src/main.c` — 删除 4 处周期发送相关代码，共约 70 行：
  1. 宏 `CAN_TEST_TX_PERIOD_MS` / `CAN_TEST_TX_TIMEOUT_MS` 及其注释块 → 改为一段删除说明；
  2. normal 模式分支的 `每 %ums 发送一帧 id=...` 日志 → 改为 `normal mode: 接收就绪，不主动发帧`；
  3. 变量 `can_tx_tick` / `can_tx_seq` 及 tick 回退初始化 → 删除；
  4. 主循环末端的整段 `if(CanDrv_GetMode()==NORMAL){ Timer_Expired→CanDrv_Send→打印 OK/FAIL }` → 删除，
     换成一行删除说明；`last_rx` 保留供串口空闲 Flush 使用。
- `host_computer_ZET6/Core/Src/main.c::main` — `can_drv.h` 的 include 注释同步更新
  （"1b 起 Normal 模式周期发测试帧" → "Normal 模式就绪但不主动发帧"）。

**保留不动**：`MX_CAN_Init()` / `CanDrv_Init()` / `CanDrv_Send()` / 回环自测分支 / 125k 全部参数。
CAN 外设仍在就绪态，只是主循环不再自动发帧；将来若需要主动发帧，由调用方显式传超时参数。

### 副作用与注意

- 上电后 ZET6 日志不再出现 `[can] tx #N ...`，C8T6 侧也不再收到周期 `0x601` 帧
  （其 `[can] rx` 行会停）——这是预期行为，不是链路故障。
- `Timer_Expired()` 在 ZET6 工程里已无调用方，但保留在 `board.c` 未删（公共工具函数，无警告）。
- `CANDRV_ID_REQ` 宏仍定义在 `can_drv.h`，未被引用，保留给以后的上层协议使用。

### 验证

`python Tools/verify_build.py host_computer_ZET6`：38 文件编译 FAIL 0，LINK rc=0。
**未上板**（本轮只改日志/发送节拍，不影响串口落盘；如需实机确认开机日志，烧 `0x08000000`）。

## 2026-11-26 16:26 — 阶段 1：按键查询是否有新程序（仅握手，不下载）

### 问题（如有）

用户要求完成 CAN OTA 的第一步：C8T6 按 KEY1 向上位机发"是否有新程序"查询帧，
ZET6 若有完整固件则回"有 + 长度 + CRC"。后续步骤（更新请求 / 分帧下发 / 写 W25Q /
Boot 搬运）本轮不实现。用户明确"先做到这一步"。

### 改动

- `APP/Core/Src/main.c::PD 区` — 新增阶段 1 协议宏：`OTA_CAN_OP_QUERY`(0x10)、
  `OTA_CAN_OP_REPLY`(0x11)、`OTA_CAN_PROTO_VER`(0x01)、查询超时 100ms、按键防抖 50ms。
  帧方向复用现有 ID：下位机发 0x581、上位机回 0x601，两侧过滤器天然匹配，驱动零改动。
  附帧格式注释（QUERY 8B / REPLY 长度 uint16 小端 + CRC 低 32 位）。
- `APP/Core/Src/main.c::PV 区` — 新增 `s_key1_prev/s_key1_fired/s_key1_t`，
  用于 KEY1(PA0) 电平变化检测 + 50ms 防抖 + 按住只发一帧的边沿锁存。
- `APP/Core/Src/main.c::main while` — 主循环加按键扫描：按下且稳定 50ms 后
  `CanDrv_Send(0x581, QUERY帧, 8, 100ms)`，打 `[can] query sent/FAIL`；
  CAN 接收保留原十六进制行，新增 REPLY 帧的可读行 `[can] reply YES/NO len=.. crc=..`。
  **只解析打印，不发更新请求、不写 Flash。**
- `host_computer_ZET6/Core/Src/main.c::PD 区` — 同一套协议宏（两侧逐字一致），
  附"有新程序 = 槽 A 头有效"的判据说明。
- `host_computer_ZET6/Core/Src/main.c::main while` — 主循环末尾加 CAN 应答段：
  收到 0x581 QUERY 时调 `OtaSlot_ReadHeader(OTA_SLOT_A)`，有效则回 0x601 REPLY
  （B1=1 + 长度 + CRC），无效则 B1=0；打 `[can] query from host: ... reply YES/NO ...`。
  周期测试帧段保留删除说明，改为"收到查询才应答"。

### 副作用与注意

- `can_drv.h` 里 `CANDRV_ID_REQ`(0x601)/`CANDRV_ID_RSP`(0x581) 的语义注释仍是
  "上位机请求 / 下位机响应"，本阶段起消息方向反转（下位机主动查询），靠 opcode 区分，
  未改驱动注释以免动到共用文件；后续抽协议头文件时一并订正。
- 长度字段只取 16 位（当前 bin 约 20KB，够用）；固件超过 64KB 时阶段 2 要扩字段。
- KEY1=PA0 为浮空输入，按下为低；若板子实际接法相反（按下为高），只需把
  `key1_now` 的三元判断翻转，其余逻辑不变。**未上板确认按键接法。**
- ZET6 侧 REPLY 的 CRC 来自槽头（`FinishOta` 回读校验通过后写），若槽 A 从未收过
  完整 bin，头无效 -> 回"无"，不会误报。
- 阶段 1 纯收发、不碰 W25Q 写入路径，串口落盘链路不受影响。

### 验证

`python Tools/verify_build.py APP`：36 文件 FAIL 0，LINK rc=0。
`python Tools/verify_build.py host_computer_ZET6`：38 文件 FAIL 0，LINK rc=0。
**未上板**；实机判据：按 KEY1 后 C8T6 打 `[can] query sent`、ZET6 打
`[can] query from host: slot A valid, reply YES ...`、C8T6 再打 `[can] reply YES len=..`。

## 2026-11-26 16:42 — ZET6 增加空闲自动收尾，解决串口助手不发尾部导致 CAN 查询恒为 NO

### 问题

实机按键查询时 ZET6 恒回 `reply NO`。日志显示 21204B 数据全部落盘、累计正确，
但缺 `[ota] 尾部：长度=..` / `[ota] 校验通过` / `[ota] 槽 A 已保存`，槽头从未写入。
根因：串口助手 `Tools/SerialAssist/serial_assist.py::RE_READY` 只认
`[ota] ready, app=.., page=..B, proto=2`，而 ZET6 打的是 `[recv] 就绪，外部 Flash 槽A ...`，
匹配失败 -> `ota_proto` 留在默认 1（老协议）-> 发完 bin 不追加 12 字节 `OTAE` 尾部
（日志里没有 `--- 已发尾部记录 ... ---` 可佐证）-> `FinishOta()` 不执行 -> 槽头空。

### 改动

- `host_computer_ZET6/Core/Src/main.c::Includes/PV` — 加 `#include "crc32.h"` 与
  `s_tail_seen`（本次上电是否已收尾的互斥锁）；带注释说明为什么需要它。
- `host_computer_ZET6/Core/Src/main.c::HandleRx` — 识别到合法 OTAE 尾部即置 `s_tail_seen=1`，
  **无论 FinishOta 成功与否**。防止尾部校验失败提前 return 后，空闲路径又把坏数据扶正成有效槽。
- `host_computer_ZET6/Core/Src/main.c::main while 空闲分支` — 触发条件由 `Ota_Fill()>0`
  改为 `s_ota_started && !s_tail_seen && Ota_Total()>0 && 2s 超时`；
  分支内先 `Ota_Flush()`，再 `Crc32_Flash(槽 A 数据区, Ota_Total())` 算 CRC，
  用 `OtaSlot_BuildHeader` + `OtaSlot_WriteHeader` 写槽头，打
  `[ota] 自动收尾：槽 A 已保存 N B CRC=...`，末尾置 `s_tail_seen=1` 只做一次。
  原条件在"最后一段恰好凑满 4096"时 `Ota_Fill()==0` 会漏写槽头，一并修正。

### 副作用与注意

- **CRC 不再是独立校验源**：改为从 W25Q 读回自己算，只能证明落盘内容稳定，
  证明不了串口传输过程没丢字节；丢字节会在后续 CAN 下发时由下位机 CRC 比对发现。
- **传一半断线会被当成完整固件**（2 秒无数据即认为传完）；要堵需增加长度预声明，
  本轮未做。
- 不调 `OtaSlot_VerifyData()`：CRC 就是从 Flash 读回算的，再比对必然一致，无意义。
- 带尾部的脚本（`ota_headless.py` / 修好的助手）走原 `FinishOta` 路径，行为不变。
- 串口助手本身的协议判定未改（在 `Tools/`，不属固件）；若以后要长期用 GUI 传，
  需另改 `serial_assist.py` 并重新打包 exe。

### 验证

`python Tools/verify_build.py host_computer_ZET6`：38 文件 FAIL 0，LINK rc=0。
**未上板**。预期：用现有助手发裸 bin -> 2 秒后打 `[ota] 自动收尾：槽 A 已保存` ->
按 KEY1 得到 `reply YES`。

## 2026-11-26 16:55 — 修复自动收尾 CRC 算错：Crc32_Flash 不能读 W25Q

### 问题

上条改动的实机日志：握手三段全通（`query sent` / `reply YES sent` / C8T6 `reply YES len=21204`），
但槽头 CRC 报 `0x9C69185F`，与源 bin（`APP.bin` 21204B）的 `0x91C2B6C8` 不符。
根因：自动收尾里调用了 `Crc32_Flash(OtaSlot_DataAddr(OTA_SLOT_A), n)`，
而该函数用 `*(volatile const uint8_t*)addr` 直接解引用，只对能 XIP 的片内 Flash 有效；
W25Q 是 SPI 外挂、不支持内存映射，`0x001000` 只是 W25Q 内部地址，
直接解引用实际读到 STM32 地址空间别处（片内 Flash 别名区），CRC 与固件内容无关。
`ota_slot.c::OtaSlot_VerifyData` 早有正确示范（用 `W25q_Read` 分块读，注释明确写了
"W25Q 不支持 XIP"），照搬时误用了错的函数。

### 改动

- `host_computer_ZET6/Core/Src/main.c::CalcSlotDataCrc`（新增，USER CODE BEGIN 0 区）—
  经 `W25q_Read` 按 `W25Q_PAGE_SIZE`(256B) 分块读回槽数据，用 `Crc32_Update` 累计、
  末尾取反，与 `OtaSlot_VerifyData` 和上位机 zlib.crc32 口径一致；带注释说明为什么
  不能用 `Crc32_Flash` 及这次踩坑的实机数值。静态 256B 缓冲不进栈。
- `host_computer_ZET6/Core/Src/main.c::main while 空闲分支` — 把
  `Crc32_Flash(...)` 换成 `CalcSlotDataCrc(..., &crc)`；读回失败时不写槽头（宁可 CAN 回"无"，
  也不广播错 CRC），打 `[ota] 自动收尾：读回 CRC 失败，槽头未写`。同步更新注释。

### 副作用与注意

- 阶段 1 的"有无"判断不受影响（槽头 magic/长度仍有效），但**修复前那份槽头的 CRC 是错的**；
  烧新固件后必须重新传一次 bin 覆盖槽头，否则下位机拿 CRC 比对会失败。
- 用户明确：以后无需我代编译/生成 bin，用户自己编译。本轮只跑 `verify_build.py` 验证编译，
  未生成烧录用 bin。

### 验证

`python Tools/verify_build.py host_computer_ZET6`：38 文件 FAIL 0，LINK rc=0。**未上板**。
验收判据（比上轮更严）：自动收尾日志里的 CRC 必须等于 `zlib.crc32(APP.bin)`，
且 C8T6 `reply YES` 帧里的 CRC 与之逐字节一致。

## 2026-11-26 19:30 — 阶段 2：按键触发更新请求 + CAN 分帧下发 + 下位机落盘

### 问题（如有）

阶段 1 按键查询已实机通过（reply YES len=21204 crc=0x91C2B6C8）。用户要求继续做到
"下位机发更新请求 -> 上位机分帧下发 bin -> 下位机写外部 Flash -> 走正常升级流程"，
并明确三项决策：去重、块级 ACK 流控、收到回应准备发 bin 后才屏蔽串口喂数（日志 TX 照常）。

### 改动

- `APP/Core/Src/main.c::协议宏/会话状态` — 新增 0x12 UPDATE_REQ / 0x13 DATA / 0x14 ACK
  三个 opcode；5B/帧载荷、16 帧/块；会话三态 IDLE->WAIT_REPLY->RECEIVING。
  注释写明 CAN 帧 8B 的分配（B0 opcode + B1B2 seq + B3~B7 5B 数据）。
- `APP/Core/Src/main.c::OtaCan_IsCurrentFw` — 去重：用 `Crc32_Flash(0x08005000, len)`
  比对 REPLY 的 CRC；长度 0 或超 App 区 44KB (`OTA_CAN_APP_MAX`) 时判非法拒收，
  防止读到片内 Flash 末尾之外。
- `APP/Core/Src/main.c::OtaCan_SendAck` — 统一 ACK 出口（B1B2=最后 seq，B3=0 正常/非0 中止）。
- `APP/Core/Src/main.c::主循环 KEY1 段` — 只在 IDLE 态允许按键；查询发成功才进 WAIT_REPLY。
- `APP/Core/Src/main.c::主循环串口段` — RECEIVING 态不调 `Boot_Loader_Getlen()`（屏蔽串口喂 W25Q），
  但日志 `Uart_Printf` 照常。避免串口与 CAN 两路数据交错写同一固件区。
- `APP/Core/Src/main.c::主循环 CAN 状态机` — REPLY 回显长度+CRC 发 UPDATE_REQ，
  经 `Boot_Loader_Init()` 干净交接 W25Q；DATA 帧攒块缓冲 `s_ota_block[80]`，
  收满一块才 `OtaW25q_Feed`（擦写发生在回 ACK 前、上位机停发的静默窗），
  收齐后 `Boot_Loader_Finish()`（内部补尾+回读比对+写 PENDING 标志）。
  收齐与块满改互斥分支，避免末帧双 ACK；收尾 ACK 的 seq 用 `expect_seq-1` 对齐上位机校验。
  新增 `s_ota_fed` 累计已 Feed 字节数，末帧按它裁掉填充字节（用 `OtaW25q_Total()` 会多算
  扇区里未落盘的尾巴、把补 0 写进固件）。收包超时 1.5s 防会话挂死。
- `host_computer_ZET6/Core/Src/main.c::协议宏/发送状态` — 同套 opcode/参数（两侧逐字一致）；
  发送态 IDLE->SENDING，槽 A 数据区边读边发。
- `host_computer_ZET6/Core/Src/main.c::OtaCan_SendDataFrame` — 从 `OtaSlot_DataAddr(A)` 用
  `W25q_Read` 取 5B/帧，末帧不足补 0（下位机按长度裁）；读失败返回错误不发全 0 假数据。
- `host_computer_ZET6/Core/Src/main.c::OtaCan_OnAck` — 单帧 ACK 处理，校验 seq=本块最后一帧，
  错误 ACK 立即中止。刻意不在此取队列——CAN 只有一个接收队列，两处取会互相吞帧。
- `host_computer_ZET6/Core/Src/main.c::主循环 CAN 统一分发` — 所有 0x581 帧一处取干净，
  按 opcode 分发 QUERY/UPDATE_REQ/ACK。UPDATE_REQ 校验回显长度+CRC 与槽头一致才启动，
  防止查询与请求之间槽被重传覆盖。分帧推进每圈最多 1 帧、块内 1ms 间隙，
  满 16 帧或全部发完进等 ACK 态，1.5s 超时中止。

### 副作用与注意

- 这是块级 ACK 的首次实机尝试（用户选择"赌一把"），突发 FIFO 风险靠"收满一块才 Feed +
  回 ACK 前完成擦写"规避；若实测仍丢帧，退路是缩块或逐帧 ACK。
- 分帧数按 5B/帧：21204B ≈ 4241 帧，125k 约 3.8s（不含擦写空窗）。
- 下位机去重意味着同一份 bin 再次按键会被跳过（打"已是当前固件"）——这是预期行为。
- 未做重传：seq 错位/写入失败回 err ACK 让上位机停，靠用户重跑一轮恢复。

### 验证

`python Tools/verify_build.py APP`：36 文件 FAIL 0 / LINK rc=0。
`python Tools/verify_build.py host_computer_ZET6`：38 文件 FAIL 0 / LINK rc=0。
**未上板**。验收判据：按键后 C8T6 打 `[ota] 开始 CAN 接收`，ZET6 打 `[ota] 开始下发`，
分帧完成后 ZET6 打 `[ota] 下发完成`、C8T6 打 `Finish=0`，且 `[wota] UPDATE FLAG SET` 出现，
复位后 Boot 搬运、App 运行。

## 2026-11-26 19:44 — KEY2(PA1) 人工触发回滚

### 问题（如有）

阶段 2 端到端闭环已实机通过（按键查询→分帧下发→落盘→Boot 搬运→App 确认→flag erased），
去重分支也实机 PASS（同 bin 再按键跳过下载）。用户要求加"按下 KEY2 回滚"，用于主动
触发回滚链路，不必等 App 跑飞。

### 改动

- `APP/Core/Src/main.c::PV` — 新增 s_key2_prev / s_key2_fired / s_key2_t，与 KEY1 各自
  独立锁存，避免共用 fired 吞掉第二次按键。
- `APP/Core/Src/main.c::主循环 KEY2 段` — PA1 按下低电平、防抖 50ms、与 KEY1 同款边沿检测；
  先 `OtaBackup_ReadHeader` + `OtaBackup_VerifyData` 校验备份有效性，仅当有效才
  `OtaFlag_Write(hdr.length, hdr.crc32, OTA_FLAG_APPLIED)` + `HAL_NVIC_SystemReset()`。
  Boot 读 APPLIED 走已实机验证的 ApplyRollback：从 0x201000 恢复→CRC 校验→擦标志→跳 App。
- 只在 `s_ota_st == IDLE` 时响应 KEY2，避免下载中两路抢 W25Q。

### 副作用与注意

- 不在 App 里直接擦写片内 Flash：F103 单 bank，App 在 0x08005000 取指时擦同 bank 必 HardFault；
  写标志+复位把恢复动作交给 Boot 是唯一安全路径。
- 备份区存的是"最近一次确认过的固件"（App 启动时备份当时的片内）。刚升级完就按 KEY2，
  备份区=当前这份新固件，回滚是把同一份重搬一遍 —— 验证的是回滚链路通不通，
  **不是退回更早版本**；要退更早版本需改备份时机（本轮未做）。
- 备份无效时不写标志、不复位，fail-safe 优先。
- 与 `OTA_TEST_CRASH==1`（上电按住 KEY2 制造 HardFault）共用 PA1 但时机不同，测试桩开启时
  其 crash 注入在确认逻辑之前，本段仍可工作；默认宏为 0。

### 验证

`python Tools/verify_build.py APP`：36 文件 FAIL 0 / LINK rc=0。**未上板、未生成 bin**（用户自编译）。
验收判据：按 KEY2 后打 `[app] KEY2 rollback: flag APPLIED OK, reset...`，复位后 Boot 打
`[apply] flag: state=APPLIED` → `[apply] ROLLBACK: restoring ...` → `[apply] rollback done`。

## 2026-11-26 20:18 — ZET6 上位机工程纳入本仓版本管理

### 问题（如有）

整套 CAN OTA 的另一半代码（`host_computer_ZET6/`）此前按约定整目录忽略，只存在于本地磁盘，
没有版本控制备份。本轮先上传 GitHub 时发现这个覆盖缺口；用户明确改为纳入本仓。

### 改动

- `.gitignore` — 删除 `host_computer_ZET6/` 整目录忽略，改为注释说明纳入理由。
  `build/` 与 `*.elf` 已被全局规则挡住，编译产物不会上传（已用 `git status --untracked-files=all` 核对，
  127 个待纳入文件中无 bin/elf/hex/exe）。
- `host_computer_ZET6/.gitignore` — 新增忽略 `.eide/env.ini`（本机的空模板环境变量文件），
  与 APP 工程保持一致（APP 侧从未跟踪该文件）。保留 `.eide/eide.yml` 与 `files.options.yml`。

### 副作用与注意

- 纳入的是 ZET6 上位机工程的**全部当前代码**，包括阶段 1 查询应答、阶段 2 分帧发送、
  CRC 修复（CalcSlotDataCrc 走 SPI 读回）与空闲自动收尾——这些改动此前没有 git 备份。
- 用户说明：后续硬件方案改为**两块 C8T6** 做 OTA，届时再改代码；本轮只做版本归档，不动任何源码。
