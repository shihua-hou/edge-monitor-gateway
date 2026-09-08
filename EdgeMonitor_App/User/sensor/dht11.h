#ifndef __DHT11_H
#define __DHT11_H

#include "stm32f1xx_hal.h"
#include "types.h"

/* DHT11 温湿度单总线传感器（战舰板 PG11）
 * 注意：DHT11 最小采样间隔 1s，调用频率必须 ≥1s */
void DHT11_Init(void);
/* 0=成功, 1=起始响应超时(总线上等不到 DHT11 拉低/拉高的应答，多半是没接好/
   供电问题), 2=校验和不对(有响应但数据传输出错，多半是时序/干扰问题)。
   之前只返回 0/1，没区分是哪种失败，板子脱离调试器独立跑起来之后，除了
   串口打印什么都看不到，两种原因不分开没法定位问题在哪一头。
   temp/humi 单位 0.1 */
u8   DHT11_Read_Data(s16 *temp_x10, s16 *humi_x10);

#endif
