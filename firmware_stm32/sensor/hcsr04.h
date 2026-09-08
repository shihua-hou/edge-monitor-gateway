#ifndef __HCSR04_H
#define __HCSR04_H

#include "stm32f1xx_hal.h"
#include "types.h"

/* HC-SR04 超声波测距
 * 引脚：TRIG=PE6（推挽输出）, ECHO=PB6（TIM4_CH1 输入捕获）
 * 原理：触发 10us 高电平 → ECHO 高电平时长 ∝ 距离，距离(cm)=us/58
 * 注意：测量间隔建议 ≥60ms；阻塞等待回波 */
/* 返回 0=成功。失败不再 while(1) 死等——这个函数在最高优先级的采集任务里
   调用，卡住等于整机停摆，看门狗复位后又卡在同一行，变成无限重启循环。
   失败后 GetDistance 恒返回 0（等同无回波），由上层置传感器故障位 */
u8   HC_SR04_Init(void);
u16  HC_SR04_GetDistance(void);   /* 距离 cm，0=超时/无回波 */

#endif
