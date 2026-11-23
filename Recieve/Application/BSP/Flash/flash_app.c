/**
 * Flash 写入实现（C8T6 最小版）
 *
 * 【来源】从 参考/Boot_Loader/Application/BSP/Flash/flash_app.c 逐字复制，
 *   只对注释做了芯片规格订正（页 1KB / App 区 0x08005000 起 / 末尾 0x08010000）。
 *   代码逻辑未动：HAL 的擦写接口对 F1 全系列一致，容量差异只体现在页大小宏上。
 * 【副作用】整个 Recieve 工程只有本文件碰 HAL_FLASH_*，别处要写 Flash 必须走这里，
 *   避免解锁/上锁配对错乱导致后续写不进。
 */
#include "flash_app.h"

#include "stm32f1xx_hal.h"

/**
  * 擦除一页
  *
  * HAL 的擦除接口按「从哪页开始、擦几页」调用，自己不做对齐检查 ——
  * 给 0x08004500 它也会从那儿开始擦，擦到哪页全看运气。
  * 所以这里先判页首对齐（1KB），不对就直接拒绝。
  */
int8_t Flash_ErasePage(uint32_t addr)
{
	FLASH_EraseInitTypeDef init;
	uint32_t page_err = 0U;
	HAL_StatusTypeDef st;

	/* 没按 1KB 对齐就退出，别试 —— 擦错页会把 Boot 自己抹掉 */
	if ((addr & (FLASH_PAGE_BYTES - 1U)) != 0U)
	{
		return FLASH_ERR_ARG;
	}

	/* 越界检查：C8T6 只有 64KB，0x08010000 之后不存在，擦了也没意义 */
	if ((addr < APP_ADDR) || (addr >= FLASH_END))
	{
		return FLASH_ERR_ARG;
	}

	init.TypeErase   = FLASH_TYPEERASE_PAGES;		/* 按页擦 */
	init.Banks       = FLASH_BANK_1;				/* 单 bank，按页擦时用不到 */
	init.PageAddress = addr;						/* 从这一页开始 */
	init.NbPages     = 1U;							/* 只擦一页 */

	HAL_FLASH_Unlock();								/* 不解锁写不进 */
	st = HAL_FLASHEx_Erase(&init, &page_err);
	HAL_FLASH_Lock();								/* 立刻锁回，减少误写窗口 */

	/* 擦完这一页全是 0xFF。page_err 是出错页地址，调试时可看 */
	if (st != HAL_OK)
	{
		return FLASH_ERR_ERASE;
	}
	return FLASH_OK;
}

/**
  * 写入一段数据
  *
  * F103 底层一次只能写 2 字节（半字），所以循环两字节两字节写。
  * 拼接顺序「低字节放低地址」，写反了读回来就是字节颠倒。
  */
int8_t Flash_Write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
	uint32_t i;

	if ((buf == NULL) || (len == 0U))
	{
		return FLASH_ERR_ARG;
	}
	/* 半字对齐：地址奇数会报 PGERR，一个字节都写不进 */
	if ((addr & 1U) != 0U)
	{
		return FLASH_ERR_ARG;
	}
	/* 越界检查：防止一次喂太多把 bin 写到 Flash 外面 */
	if ((addr < APP_ADDR) || ((addr + len) > FLASH_END))
	{
		return FLASH_ERR_ARG;
	}

	HAL_FLASH_Unlock();

	for (i = 0U; i < len; i += 2U)
	{
		uint16_t hw;

		if ((i + 1U) < len)
		{
			/* 完整半字：低字节在前 */
			hw = (uint16_t)((uint16_t)buf[i] | ((uint16_t)buf[i + 1U] << 8));
		}
		else
		{
			/* 只剩一个字节：高 8 位补 0xFF（未写态）凑半字 */
			hw = (uint16_t)((uint16_t)buf[i] | 0xFF00U);
		}

		if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr + i, (uint64_t)hw) != HAL_OK)
		{
			HAL_FLASH_Lock();
			return FLASH_ERR_WRITE;
		}
	}

	HAL_FLASH_Lock();
	return FLASH_OK;
}
