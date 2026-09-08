#ifndef __MPU6050_H
#define __MPU6050_H

#include "stm32f1xx_hal.h"
#include "types.h"

/* MPU6050 六轴姿态传感器（软件 I2C：SCL=PB10, SDA=PB11）
 * 兼容 MPU6500（WHO_AM_I=0x70）——市面上很多标"MPU6050"的模块实际贴的是这颗，
 * 寄存器基本兼容，Init 里两种 ID 都放行
 * 姿态输出：俯仰/横滚（互补滤波）、偏航（陀螺仪积分，无磁力计会漂移）
 * 注意：MPU6050_GetAngle 调用间隔必须固定为 20ms（互补滤波 dt 固定） */
u8   MPU6050_Init(void);                       /* 0=成功 1=I2C错误 2=器件ID不对 */
/* 单位 0.01°。返回 0=成功，1=I2C 读失败（此时输出保持上一次的角度，
   调用方应据此把 IMU 标记为故障，而不是把输出当成有效读数） */
u8   MPU6050_GetAngle(s16 *pitch_x100, s16 *roll_x100, s16 *yaw_x100);

#endif
