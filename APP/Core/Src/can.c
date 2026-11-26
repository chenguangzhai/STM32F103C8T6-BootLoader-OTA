/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    can.c
  * @brief   This file provides code for the configuration
  *          of the CAN instances.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "can.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

CAN_HandleTypeDef hcan;

/* CAN init function */
void MX_CAN_Init(void)
{

  /* USER CODE BEGIN CAN_Init 0 */

  /* USER CODE END CAN_Init 0 */

  /* USER CODE BEGIN CAN_Init 1 */

  /* USER CODE END CAN_Init 1 */
  hcan.Instance = CAN1;
  /* 2026-11-25 21:59：回到 125k（PSC=16，36MHz/(16×18)=125kbps，采样点 88.9%）。
     125k 是原定的目标速率；中间的 50k / 25k 全是诊断用的降速试验，结论已落地：
     500k/125k/50k/25k 一律表现为 `LEC=5`(bit dominant error)，且对位宽完全不敏感
     （25k 位宽 40µs 仍是同一个错误），所以速率不是变量。
     真正的病灶是 C8T6 外接那颗 VP230 收发器：它驱动显性时自己的 RXD 回读不到，
     于是 ACK 发出去了（对端判 tx OK）、整帧却被自己判错丢弃。更换收发器后（飞线到
     另一块板上的收发器）25k 下 ESR=0、REC=0、连续多帧 ID/DLC/数据全对 —— 固件、位时序、
     时钟树、引脚配置、过滤器全部无罪。
     现在恢复 125k，同时用它验证临时飞线能否撑住更快的边沿：若 ESR 仍为 0 且 rx 不断，
     就定在 125k（OTA 约 11KB/s）；若出现 LEC≠0 或丢帧，说明飞线质量不够，退回 25k。
     ⚠ 三处必须同步：本文件、.ioc 的 CAN.Prescaler、can_drv.h 的 CANDRV_EXPECT_BAUD，
       少一处 CanDrv_Init() 就会返回 -6（位时序与预期不符）。 */
  hcan.Init.Prescaler = 16;
  /* 2026-11-25 21:2x：Normal → Silent → **回到 Normal**。
     Silent 判别试验已完成且给出结论：同一套硬件、同一 50k、同一固件，只把 Mode 换成 SILENT
     之后 C8T6 连续收到 14 帧 `A5 5A D0 01 00 00 01 00`（ID/DLC/数据/序号全对），ESR 归零；
     而 NORMAL 下必定 `LEC=5`(bit dominant error)、REC→255、整帧丢弃。
     即：本侧在 ACK slot 驱动显性后，收发器模块没把这一位回读到自己的 RXD —— 模块的模拟特性缺陷，
     固件无解（ACK 由硬件自动发出，F103 只有 SILENT 能关，且会连发送一起关掉）。
     结论与复现步骤见 CHANGELOG 21:20 条 / memory 2026-11-25。换模块后回来直接跑这版即可。
     ⚠ 与 .ioc 的 CAN.Mode=CAN_MODE_NORMAL 同步，否则 CubeMX 重新生成会写回别的模式。 */
  hcan.Init.Mode = CAN_MODE_NORMAL;
  hcan.Init.SyncJumpWidth = CAN_SJW_1TQ;
  hcan.Init.TimeSeg1 = CAN_BS1_15TQ;
  hcan.Init.TimeSeg2 = CAN_BS2_2TQ;
  hcan.Init.TimeTriggeredMode = DISABLE;
  hcan.Init.AutoBusOff = ENABLE;
  hcan.Init.AutoWakeUp = DISABLE;
  hcan.Init.AutoRetransmission = ENABLE;
  hcan.Init.ReceiveFifoLocked = ENABLE;
  hcan.Init.TransmitFifoPriority = DISABLE;
  if (HAL_CAN_Init(&hcan) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CAN_Init 2 */

  /* USER CODE END CAN_Init 2 */

}

void HAL_CAN_MspInit(CAN_HandleTypeDef* canHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(canHandle->Instance==CAN1)
  {
  /* USER CODE BEGIN CAN1_MspInit 0 */

  /* USER CODE END CAN1_MspInit 0 */
    /* CAN1 clock enable */
    __HAL_RCC_CAN1_CLK_ENABLE();

    __HAL_RCC_GPIOB_CLK_ENABLE();
    /**CAN GPIO Configuration
    PB8     ------> CAN_RX
    PB9     ------> CAN_TX
    */
    GPIO_InitStruct.Pin = GPIO_PIN_8;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_9;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    __HAL_AFIO_REMAP_CAN1_2();

    /* CAN1 interrupt Init */
    HAL_NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);
    HAL_NVIC_SetPriority(CAN1_SCE_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(CAN1_SCE_IRQn);
  /* USER CODE BEGIN CAN1_MspInit 1 */

  /* USER CODE END CAN1_MspInit 1 */
  }
}

void HAL_CAN_MspDeInit(CAN_HandleTypeDef* canHandle)
{

  if(canHandle->Instance==CAN1)
  {
  /* USER CODE BEGIN CAN1_MspDeInit 0 */

  /* USER CODE END CAN1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_CAN1_CLK_DISABLE();

    /**CAN GPIO Configuration
    PB8     ------> CAN_RX
    PB9     ------> CAN_TX
    */
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_8|GPIO_PIN_9);

    /* CAN1 interrupt Deinit */
    HAL_NVIC_DisableIRQ(USB_LP_CAN1_RX0_IRQn);
    HAL_NVIC_DisableIRQ(CAN1_SCE_IRQn);
  /* USER CODE BEGIN CAN1_MspDeInit 1 */

  /* USER CODE END CAN1_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
