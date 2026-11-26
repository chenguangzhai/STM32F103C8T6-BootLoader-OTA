#ifndef __BOOT_LOADER_H
#define __BOOT_LOADER_H

#include <stdint.h>
#include "uart_app.h"

/* 2026-11-25：Boot_Loader_AppValid / Boot_Loader_JumpToApp 已随"跳 App"流程一起删除。
   本工程现在只做：串口接收 → 外部 Flash 槽 A；发送由后续 CAN 逻辑负责。 */
uint16_t Boot_Loader_Getlen(void);

#endif
