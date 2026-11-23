/**
  * W25Q64 SPI Flash 驱动实现（Recieve 侧）
  *
  * 【来源】从 Boot_Loader 工程的 W25Q 驱动复制后按 W25Q64 参数调整。
  *
  * 【本次改动原因 / 改了什么】
  * 原版把「发命令」和「收数据」拆成 HAL_SPI_Transmit + HAL_SPI_Receive 两步。
  * SPI 是全双工：发 0x9F 命令的那 8 个时钟里，Flash 就已经在 MISO 上送出第一个 ID 字节，
  * 而 HAL_SPI_Transmit 把这段 MISO 数据丢了；接着的 HAL_SPI_Receive 只能读到错位后的数据，
  * 实测读 ID 得到 0x000000。读数据命令 0x03 同理，会丢掉第一个数据字节。
  * 本次把「命令 + 应答」统一改成 HAL_SPI_TransmitReceive 一次收发，用 dummy byte 换回数据。
  * 副作用：无。纯只写的命令（写使能/擦除/页编程）仍用 HAL_SPI_Transmit，二者不混用。
  * 注意：这是独立副本，若以后改动命令时序，需评估是否同步到 Boot_Loader。
  *
  * 所有函数都靠 CS 拉低 → 先发命令 → 再收发数据 → CS 拉高 这个套路。
  * 每一步失败都先把 CS 拉回高电平再 return —— 片选一直低着，芯片会认为
  * 上一帧命令还没结束，下一帧数据直接接在后面解释，症状是"第一次对，之后全乱"。
  */
#include <stddef.h>
#include "w25q.h"

/* W25Q 指令集（只列用到的，W25Q64 与 W25Q128 一致） */
#define CMD_WRITE_ENABLE    0x06U
#define CMD_READ_STATUS1    0x05U
#define CMD_READ_DATA       0x03U
#define CMD_PAGE_PROGRAM    0x02U
#define CMD_SECTOR_ERASE    0x20U
#define CMD_JEDEC_ID        0x9FU

#define SPI_TX_TIMEOUT_MS   100U
#define SPI_RX_TIMEOUT_MS   1000U
#define SPI_DUMMY_BYTE      0xFFU   /* 全双工收数据时发出的占位字节，MOSI 上发什么 Flash 不关心 */

static SPI_HandleTypeDef *s_hspi    = NULL;
static GPIO_TypeDef      *s_cs_port = NULL;
static uint16_t           s_cs_pin  = 0U;

static void Cs_Low(void)
{
	HAL_GPIO_WritePin(s_cs_port, s_cs_pin, GPIO_PIN_RESET);
}

static void Cs_High(void)
{
	HAL_GPIO_WritePin(s_cs_port, s_cs_pin, GPIO_PIN_SET);
}

/* 只发不收：写使能、擦除、页编程这类命令用。
   保留原样是因为 Flash 不返回有效数据，用 Transmit 最省事。 */
static int8_t Tx(const uint8_t *buf, uint16_t len)
{
	if(HAL_SPI_Transmit(s_hspi, (uint8_t *)buf, len, SPI_TX_TIMEOUT_MS) != HAL_OK)
	{
		return W25Q_ERR_SPI;
	}
	return W25Q_OK;
}

/* 只收不发：仅在「命令已用 Transceive 完整发过、Flash 正持续输出数据」的后续块中使用。
   注意：绝不能用它紧跟在 HAL_SPI_Transmit 后面读第一个字节——那会丢掉命令期间已移出的应答。 */
static int8_t Rx(uint8_t *buf, uint16_t len)
{
	if(HAL_SPI_Receive(s_hspi, buf, len, SPI_RX_TIMEOUT_MS) != HAL_OK)
	{
		return W25Q_ERR_SPI;
	}
	return W25Q_OK;
}

/*【新增】全双工收发：边发 tx 边把 MISO 上的数据收进 rx。
   W25Q 的所有「发命令→读应答」场景都必须走这里：命令字节发出去的同时，
   Flash 已经在 MISO 上放应答的第 1 个字节，只有 TransmitReceive 能接住它。
   tx/rx 可以指向同一缓冲（调用方保证长度足够）。 */
static int8_t Transceive(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	if(HAL_SPI_TransmitReceive(s_hspi, (uint8_t *)tx, rx, len, SPI_RX_TIMEOUT_MS) != HAL_OK)
	{
		return W25Q_ERR_SPI;
	}
	return W25Q_OK;
}

/* 轮询状态寄存器 BUSY 位，直到芯片内部操作结束
   【改动】读状态寄存器由 Tx+Rx 改为 Transceive，否则读回的 sr 是错位的，
   可能永远读到 BUSY=1 而超时，擦/写全部报失败。 */
static int8_t WaitBusy(uint32_t timeout_ms)
{
	uint8_t  tx[2] = { CMD_READ_STATUS1, SPI_DUMMY_BYTE };
	uint8_t  rx[2];
	uint32_t t0  = HAL_GetTick();

	do
	{
		Cs_Low();
		if(Transceive(tx, rx, 2U) != W25Q_OK)
		{
			Cs_High();
			return W25Q_ERR_SPI;
		}
		Cs_High();

		if((rx[1] & 0x01U) == 0U)		/* rx[0] 是命令期间的空转，rx[1] 才是状态寄存器 */
		{
			return W25Q_OK;
		}
	} while((HAL_GetTick() - t0) < timeout_ms);

	return W25Q_ERR_TIMEOUT;
}

/* 每次擦/写之前都必须发一次，否则芯片拒绝执行 */
static int8_t WriteEnable(void)
{
	uint8_t cmd = CMD_WRITE_ENABLE;

	Cs_Low();
	if(Tx(&cmd, 1U) != W25Q_OK)
	{
		Cs_High();
		return W25Q_ERR_SPI;
	}
	Cs_High();

	return W25Q_OK;
}

int8_t W25q_Init(SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin)
{
	GPIO_InitTypeDef init = {0};

	if((hspi == NULL) || (cs_port == NULL) || (cs_pin == 0U))
	{
		return W25Q_ERR_ARG;
	}

	s_hspi    = hspi;
	s_cs_port = cs_port;
	s_cs_pin  = cs_pin;

	/* 片选默认拉高：W25Q 的 CS 是低有效，上电浮空会被当成命令起始 */
	HAL_GPIO_WritePin(cs_port, cs_pin, GPIO_PIN_SET);

	init.Pin   = cs_pin;
	init.Mode  = GPIO_MODE_OUTPUT_PP;
	init.Pull  = GPIO_PULLUP;			/* 内部上拉，防止悬空被干扰成低电平 */
	init.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(cs_port, &init);

	return W25Q_OK;
}

/*【新增·诊断用】读 JEDEC ID 的 3 个原始字节，供自检打印，便于区分接线/时序问题。
   【改动原因】出问题时只看到一个合成的 32 位值，无法判断是哪个字节错、是否全 0。
   副作用：无，不改变器件状态。 */
int8_t W25q_ReadIdRaw(uint8_t id[3])
{
	uint8_t tx[4] = { CMD_JEDEC_ID, SPI_DUMMY_BYTE, SPI_DUMMY_BYTE, SPI_DUMMY_BYTE };
	uint8_t rx[4];
	int8_t  st;

	if((s_hspi == NULL) || (id == NULL))
	{
		return W25Q_ERR_ARG;
	}

	Cs_Low();
	st = Transceive(tx, rx, 4U);
	Cs_High();
	if(st != W25Q_OK)
	{
		return st;
	}

	/* rx[0] 是发命令那 8 个时钟的空转（Flash 此时才刚收到命令），
	   rx[1..3] 才是 0xEF / 0x40 / 0x17 */
	id[0] = rx[1];
	id[1] = rx[2];
	id[2] = rx[3];
	return W25Q_OK;
}

uint32_t W25q_ReadId(void)
{
	uint8_t id[3] = {0U, 0U, 0U};

	if(W25q_ReadIdRaw(id) != W25Q_OK)
	{
		return 0xFFFFFFFFUL;
	}

	return ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | (uint32_t)id[2];
}

uint8_t W25q_ReadStatus(void)
{
	uint8_t tx[2] = { CMD_READ_STATUS1, SPI_DUMMY_BYTE };
	uint8_t rx[2];

	if(s_hspi == NULL)
	{
		return 0xFFU;
	}

	Cs_Low();
	if(Transceive(tx, rx, 2U) != W25Q_OK)
	{
		Cs_High();
		return 0xFFU;
	}
	Cs_High();

	return rx[1];
}

int8_t W25q_EraseSector(uint32_t addr)
{
	uint8_t cmd[4];

	if((addr % W25Q_SECTOR_SIZE) != 0U)		/* 必须扇区对齐，否则会擦错 4KB */
	{
		return W25Q_ERR_ADDR;
	}
	if(addr >= W25Q_SIZE)					/* W25Q64 上限 8MB，比源版 W25Q128 的 16MB 小一半 */
	{
		return W25Q_ERR_ADDR;
	}

	if(WriteEnable() != W25Q_OK)
	{
		return W25Q_ERR_SPI;
	}

	cmd[0] = CMD_SECTOR_ERASE;
	cmd[1] = (uint8_t)(addr >> 16);
	cmd[2] = (uint8_t)(addr >> 8);
	cmd[3] = (uint8_t)(addr);

	Cs_Low();
	if(Tx(cmd, 4U) != W25Q_OK)
	{
		Cs_High();
		return W25Q_ERR_SPI;
	}
	Cs_High();

	return WaitBusy(1000U);					/* 擦除典型 45ms，最坏 400ms，给足余量 */
}

int8_t W25q_WritePage(uint32_t addr, const uint8_t *buf, uint16_t len)
{
	uint8_t cmd[4];

	if((buf == NULL) || (len == 0U) || (len > W25Q_PAGE_SIZE))
	{
		return W25Q_ERR_ARG;
	}
	/* 不能跨页：当前页内偏移 + 长度不能超过 256，否则芯片会回卷覆盖页首 */
	if(((addr & (W25Q_PAGE_SIZE - 1U)) + len) > W25Q_PAGE_SIZE)
	{
		return W25Q_ERR_ADDR;
	}
	if((addr + len) > W25Q_SIZE)
	{
		return W25Q_ERR_ADDR;
	}

	if(WriteEnable() != W25Q_OK)
	{
		return W25Q_ERR_SPI;
	}

	cmd[0] = CMD_PAGE_PROGRAM;
	cmd[1] = (uint8_t)(addr >> 16);
	cmd[2] = (uint8_t)(addr >> 8);
	cmd[3] = (uint8_t)(addr);

	Cs_Low();
	if((Tx(cmd, 4U) != W25Q_OK) || (Tx(buf, len) != W25Q_OK))
	{
		Cs_High();
		return W25Q_ERR_SPI;
	}
	Cs_High();

	return WaitBusy(100U);					/* 页编程典型 0.7ms，最坏 3ms */
}

int8_t W25q_Write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
	uint32_t done = 0U;
	int8_t   st;

	if((buf == NULL) || (len == 0U))
	{
		return W25Q_ERR_ARG;
	}
	if((addr + len) > W25Q_SIZE)
	{
		return W25Q_ERR_ADDR;
	}

	while(done < len)
	{
		uint32_t cur   = addr + done;
		uint32_t left  = len - done;
		uint32_t space = W25Q_PAGE_SIZE - (cur & (W25Q_PAGE_SIZE - 1U));
		uint32_t n     = (left < space) ? left : space;

		st = W25q_WritePage(cur, &buf[done], (uint16_t)n);
		if(st != W25Q_OK)
		{
			return st;
		}
		done += n;
	}

	return W25Q_OK;
}

int8_t W25q_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
	uint8_t  tx[5] = { CMD_READ_DATA, 0U, 0U, 0U, SPI_DUMMY_BYTE };
	uint8_t  rx[5];
	uint32_t done;

	if((buf == NULL) || (len == 0U))
	{
		return W25Q_ERR_ARG;
	}
	if((addr + len) > W25Q_SIZE)
	{
		return W25Q_ERR_ADDR;
	}

	tx[1] = (uint8_t)(addr >> 16);
	tx[2] = (uint8_t)(addr >> 8);
	tx[3] = (uint8_t)(addr);
	done  = 1U;     /* rx[4] 就是第 1 个数据字节，先落位 */

	/*【关键改动】命令 4 字节 + 1 个 dummy 一起全双工发出：
	   发完地址的那一个时钟里 Flash 已经把第 1 个数据字节放到 MISO 上，
	   必须在这次 TransmitReceive 里接住（rx[4]），否则后续 Rx 整体错位一格。 */
	Cs_Low();
	if(Transceive(tx, rx, 5U) != W25Q_OK)
	{
		Cs_High();
		return W25Q_ERR_SPI;
	}
	buf[0] = rx[4];

	/* 后续字节 Flash 已连续输出，用分块 Receive 即可，CS 全程保持低电平。
	   HAL 的 Size 是 uint16，一次最多 65535，这里按 1KB 分块更稳。 */
	while(done < len)
	{
		uint32_t rest = len - done;
		uint16_t n    = (rest > 1024U) ? 1024U : (uint16_t)rest;

		if(Rx(&buf[done], n) != W25Q_OK)
		{
			Cs_High();
			return W25Q_ERR_SPI;
		}
		done += n;
	}
	Cs_High();

	return W25Q_OK;
}
