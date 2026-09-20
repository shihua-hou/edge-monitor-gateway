#ifndef __APP_TASK_H
#define __APP_TASK_H

#include "stm32f1xx_hal.h"
#include "types.h"

/* ============ 传感器数据（全局共享，任务间通过此结构交换） ============ */
typedef struct {
    s16 temperature_x10;   /* 温度, 0.1℃ 单位 (253=25.3℃) */
    s16 humidity_x10;      /* 湿度, 0.1% 单位 (562=56.2%) */
    u8  light_percent;     /* 光照 0~100% */
    u16 distance_cm;       /* 超声波距离 cm */
    s16 pitch_x100;        /* 俯仰角 0.01° */
    s16 roll_x100;         /* 横滚角 0.01° */
    s16 yaw_x100;          /* 偏航角 0.01° */
    /* 转速改成有符号（协议 v2.4）。无符号的话倒车和前进一个样，
     * "一侧轮子反转"（抱死、接线反了、差速转向）在大屏上看不出来。
     * 网关侧 gateway_can.c / gateway_mqtt.c 必须同步改成 int16_t 解析。 */
    s16 rpm_left;          /* 左轮转速 RPM，正=前进 */
    s16 rpm_right;         /* 右轮转速 RPM，正=前进 */
    /* 里程 = 路程，只增不减；位移 = 有符号，倒车会减小。
       两者**不可互换**，做轨迹推算只能用 disp。 */
    u32 odom_left_cm;      /* 左轮里程 cm */
    u32 odom_right_cm;     /* 右轮里程 cm */
    s32 disp_left_mm;      /* 左轮位移 mm，正=前进，倒车减小 */
    s32 disp_right_mm;     /* 右轮位移 mm，正=前进，倒车减小 */
    u8  sys_state;         /* 系统状态字 bit0=LED bit1=Buzzer bit2=Motor bit3=EStop */
    /* 传感器健康位，见下面的 SENS_FAULT_*。
     *
     * 为什么单独占一个字节、不并进 sys_state：sys_state 由 cmd 任务写，
     * 健康位由 collect 任务写。挤在同一个字节里就成了两个任务对同一地址
     * 做读-改-写，会互相覆盖（Cortex-M 上单字节访问虽是原子的，
     * 但"读-改-写"三步之间可以被抢占）。拆成两个字段，各写各的，
     * 从根子上不存在竞态，比加互斥锁便宜也可靠。 */
    u8  sensor_health;
    /* 电池电压 mV。**0 = 那根线没接**，不是"没电了"——两者在大屏上
     * 必须长得不一样，否则每台没接 ADC 线的设备都在报低电量，
     * 不到一天就没人看这个告警了。 */
    u16 batt_mv;
} SensorData_t;

/* 传感器故障位。置 1 = 该传感器连续多次读取失败，对应的数值字段是【陈旧值】，
 * 不是当前实测值。
 *
 * 为什么需要这个：所有传感器的失败值都长得像合法读数——DHT11 失败保留上次值、
 * 超声波超时返回 0、IMU 读失败以前直接清零（0° 恰是最不可疑的姿态）。
 * 线松了、模块坏了，大屏上照样是一个稳定的数字，谁也发现不了。
 * 对监控系统来说，"数据是死的"这件事必须能被看见。 */
#define SENS_FAULT_TH     0x01   /* 温湿度 DHT11 */
#define SENS_FAULT_DIST   0x02   /* 超声波 HC-SR04 */
#define SENS_FAULT_LIGHT  0x04   /* 光敏 ADC */
#define SENS_FAULT_MOTOR  0x10   /* 电机驱动初始化失败（定时器/编码器配不起来） */
#define SENS_FAULT_IMU    0x08   /* 六轴 MPU6050 */

extern SensorData_t g_sensor;   /* 全局传感器数据 */

void App_Task_Create(void);     /* 创建三个任务(main.c 调用)；之前一直没在这里声明,
                                    main.c 靠隐式声明蒙混过关,编译器只警告没报错 */

/* ============ 任务入口 ============ */
void task_sensor_collect(void *arg);  /* 传感器采集任务（调度表 20ms/200ms/1s） */
void task_can_report(void *arg);      /* CAN 周期上报任务 */
void task_cmd_process(void *arg);     /* 控制命令处理任务 */

/* ============ 执行设备接口 ============ */
void HW_Led_Set(u8 on);
void HW_Buzzer_Set(u8 on);
void HW_Motor_SetSpeed(u8 pwm_percent, u8 dir); /* dir: 0停 1前 2后 */
void HW_Motor_SetSteer(u8 direction, u8 pwm, u16 ms); /* 0x04左转 0x05右转 */
/* 坦克式差速。left/right 是 -100~+100 的百分比（已去偏置）。
   这个接口自带超时保护，见 motor.h 的 Motor_SetTank 说明。 */
void HW_Motor_SetTank(s8 left, s8 right);

#endif
