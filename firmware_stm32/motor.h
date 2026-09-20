#ifndef __MOTOR_H
#define __MOTOR_H

#include "stm32f1xx_hal.h"
#include "types.h"

/* motor.h - TB6612 双路电机驱动 + 正交编码器闭环
 *
 * 硬件：WHEELTEC TB6612 稳压版（D153C）
 *   A 路 -> 左轮，B 路 -> 右轮
 *   模块自带 12V->5V->3.3V 稳压，TB6612 的 VCC 用板载 5V，
 *   不需要主控供逻辑电；主控只需要和它共地。
 *
 * 控制方式：20ms 周期的增量式 PI 闭环，目标量是转速(RPM)。
 *   为什么做闭环而不是直接给 PWM：同样的占空比，空载和爬坡的实际转速差
 *   很远，直线走着走着就偏了。而且没有转速反馈的话，"电机堵转"和
 *   "电机正常转"在系统看来完全一样——这正是这个项目一直在防的那类问题：
 *   故障不能伪装成正常。
 */

/* ===== 电机参数：换底盘/换电机必须改这两个 =====
 * MG513 系列的减速比有 P30 / P20 / P12 等多个版本，编码器线数也有 11/13 线，
 * 这两个数错了，转速和里程会整体差一个固定倍数——而且看起来完全合理，
 * 不去实测根本发现不了。
 *
 * 标定方法：电机不通电，轮胎上贴个标记，手转整整 5 圈，
 * 看 0x102 里的里程增量是不是 5 × 轮周长（Φ65 轮 = 102cm）。
 * 转 5 圈而不是 1 圈，是把"目测一圈"的误差摊薄到五分之一。
 * 偏了是固定比例误差，按实测反算 ENC_CNT_PER_REV 即可，不用改硬件。
 *
 * 当前状态：减速比 28 已由供应商确认，**但整体标定还没实测验证过**。 */
/* MG513 P28 + 13 线编码器：13 × 4（x4 倍频）× 28（减速比）= 1456 */
#define ENC_CNT_PER_REV   1456
/* Φ65 轮：π × 65 = 204.2mm，取 204（误差 0.1%） */
#define WHEEL_CIRC_MM     204

#define MOTOR_MAX_RPM     200      /* 目标转速上限，pwm_percent=100 时对应的转速 */

u8   Motor_Init(void);                     /* 0=成功 */

/* 闭环目标转速，正=前进，负=后退。超出 ±MOTOR_MAX_RPM 会被截断 */
void Motor_SetTargetRpm(s16 left, s16 right);

void Motor_Stop(void);                     /* 目标清零，PWM 归零（STBY 保持使能） */

/* 急停：直接拉低 STBY，TB6612 输出进高阻。
 * 这条路径不经过 PI、不经过 PWM 寄存器，是硬件级的——软件跑飞时
 * 只要这个引脚还是低的，电机就一定不转。 */
void Motor_EmergencyStop(void);
void Motor_Resume(void);                   /* 解除急停 */

/* 每 20ms 调用一次：读编码器 -> 算转速和里程 -> 跑 PI -> 更新 PWM。
 * 必须周期稳定，PI 的积分项和转速换算都依赖这个固定周期。 */
void Motor_Tick(void);

void Motor_GetDisp(s32 *disp_l_mm, s32 *disp_r_mm);
void Motor_GetState(s16 *rpm_l, s16 *rpm_r, u32 *odom_l_cm, u32 *odom_r_cm);

/* 当前是否在转（任一轮目标或实测非零）。给 sys_state 的 bit2 用 */
u8   Motor_IsRunning(void);

/* 是否触发过飞车保护。一旦为真，STBY 已拉低、电机硬停，
   而且**不会自动恢复**——必须收到 ACT_RESUME 才解除。
   上报时映射成 SENS_FAULT_MOTOR，否则"车停了"和"车因为失控被切断"
   在大屏上长得一模一样。 */
u8   Motor_IsFaulted(void);

/* 定时转向：left/right 差速转 ms 毫秒后自动停。ms=0 表示一直转 */
void Motor_Turn(s16 left_rpm, s16 right_rpm, u16 ms);

/* 坦克式差速（路径跟踪用）。left/right 单位是百分比 -100~+100。
 *
 * **自带超时保护**：收到一次就启动一个 TANK_TIMEOUT_MS 的计时，
 * 超时前没有新的 SetTank 就自动停车。
 *
 * 为什么只给这个接口加超时、而不是给所有运动命令：
 *   手动遥控（ACT_ON 等）的语义是"一直跑到我叫停"，上层自己负责停；
 *   而坦克命令是**自主路径跟踪**在用，上位机（Qt 界面）一旦卡死或崩溃，
 *   车会带着最后一条差速指令一直转圈跑下去。
 *   自主模式下"指令流断了"必须等价于"停"。 */
void Motor_SetTank(s8 left_pct, s8 right_pct);

#endif /* __MOTOR_H */
