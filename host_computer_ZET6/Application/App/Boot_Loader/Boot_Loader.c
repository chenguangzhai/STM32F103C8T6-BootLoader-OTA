/**
 * Boot_Loader —— 2026-11-25 起本工程不再跳转到 App，
 * 原来用于"验证 App 向量表"和"跳过去之前清理外设"的两个函数已删除。
 * 保留的 Getlen 只是一次性读串口的辅助接口，主流程现在走 uart_app 的环形缓冲。
 */
#include "Boot_Loader.h"

uint16_t Boot_Loader_Getlen(void)
{
    uint8_t  rx_buf[512] = {0};		/* 取数据用的落地缓冲 */
    uint16_t rx_len = 0;			/* 本帧累计收到的字节数 */

    if(Uart_FrameReady(UART_LOG) == 1)
    {
        rx_len = Uart_Read(UART_LOG, rx_buf, sizeof(rx_buf));
        if(rx_len > 0)
        {
            return rx_len;
        }
    }
    return 0;
}
