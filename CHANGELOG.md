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
