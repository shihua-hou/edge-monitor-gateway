/************************************************************
 * mpu6050.c - MPU6050 六轴姿态传感器驱动（HAL 版）
 * 接口：软件 I2C（SCL=PB10, SDA=PB11，开漏+上拉）
 * 量程：加速度 ±4g（8192 LSB/g），陀螺仪 ±2000°/s（16.4 LSB/(°/s)）
 * 姿态：互补滤波（roll/pitch），yaw 为陀螺仪积分（会缓慢漂移）
 * 注意：MPU6050_GetAngle 调用间隔固定 20ms（dt=0.02s 写死）
 ************************************************************/
#include "mpu6050.h"
#include "./SYSTEM/delay/delay.h"
#include <math.h>

#define MPU_SCL   GPIO_PIN_10
#define MPU_SDA   GPIO_PIN_11
#define MPU_PORT  GPIOB

/* ---------- 软件 I2C（开漏输出 + 读 IDR） ---------- */
static void i2c_delay(void) { delay_us(5); }   /* ~100kHz */

static void scl_high(void) { HAL_GPIO_WritePin(MPU_PORT, MPU_SCL, GPIO_PIN_SET);   i2c_delay(); }
static void scl_low (void) { HAL_GPIO_WritePin(MPU_PORT, MPU_SCL, GPIO_PIN_RESET); i2c_delay(); }
static void sda_high(void) { HAL_GPIO_WritePin(MPU_PORT, MPU_SDA, GPIO_PIN_SET);   i2c_delay(); }
static void sda_low (void) { HAL_GPIO_WritePin(MPU_PORT, MPU_SDA, GPIO_PIN_RESET); i2c_delay(); }
static u8   sda_read(void) { return (u8)HAL_GPIO_ReadPin(MPU_PORT, MPU_SDA); }

static void i2c_start(void) { sda_high(); scl_high(); sda_low(); scl_low(); }
static void i2c_stop (void) { sda_low();  scl_high(); sda_high(); }
static void i2c_ack (void)  { sda_low();  scl_high(); scl_low(); sda_high(); }
static void i2c_nack(void)  { sda_high(); scl_high(); scl_low(); }

static u8 i2c_write_byte(u8 dat)
{
    u8 i, ack;
    for (i = 0; i < 8; i++) {
        if (dat & 0x80) sda_high(); else sda_low();
        scl_high(); scl_low();
        dat <<= 1;
    }
    sda_high();            /* 释放 SDA 读 ACK */
    scl_high();
    ack = sda_read() ? 1 : 0;
    scl_low();
    return ack;
}

static u8 i2c_read_byte(u8 ack)
{
    u8 i, dat = 0;
    sda_high();            /* 释放总线 */
    for (i = 0; i < 8; i++) {
        scl_high();
        dat = (u8)((dat << 1) | sda_read());
        scl_low();
    }
    if (ack) i2c_ack(); else i2c_nack();
    return dat;
}

/* ---------- MPU6050 寄存器读写 ---------- */
static u8 mpu_write_reg(u8 reg, u8 val)
{
    i2c_start();
    if (i2c_write_byte(0xD0)) { i2c_stop(); return 1; }  /* 0x68<<1 写 */
    i2c_write_byte(reg);
    i2c_write_byte(val);
    i2c_stop();
    return 0;
}

static u8 mpu_read_regs(u8 reg, u8 *buf, u8 len)
{
    u8 i;
    i2c_start();
    if (i2c_write_byte(0xD0)) { i2c_stop(); return 1; }
    i2c_write_byte(reg);
    i2c_start();
    i2c_write_byte(0xD1);                    /* 读 */
    for (i = 0; i < len - 1; i++) buf[i] = i2c_read_byte(1);
    buf[len - 1] = i2c_read_byte(0);
    i2c_stop();
    return 0;
}

/* ---------- 初始化 ---------- */
u8 MPU6050_Init(void)
{
    u8 id;
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    g.Pin = MPU_SCL | MPU_SDA;
    g.Mode = GPIO_MODE_OUTPUT_OD;          /* 开漏输出 */
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(MPU_PORT, &g);
    HAL_GPIO_WritePin(MPU_PORT, MPU_SCL | MPU_SDA, GPIO_PIN_SET);   /* 空闲高（需外部/内部上拉） */

    if (mpu_read_regs(0x75, &id, 1)) return 1;   /* I2C 通信失败 */
    /* WHO_AM_I：0x68=真正的MPU6050，0x70=MPU6500——现在很多标"MPU6050"卖的
       模块实际贴的是 MPU6500（平替，寄存器基本兼容），两个都放行 */
    if (id != 0x68 && id != 0x70) return 2;

    mpu_write_reg(0x6B, 0x00);   /* PWR_MGMT_1：唤醒，时钟自动 */
    mpu_write_reg(0x19, 0x07);   /* SMPLRT_DIV：采样率 1kHz */
    mpu_write_reg(0x1A, 0x06);   /* CONFIG：DLPF 5Hz */
    mpu_write_reg(0x1B, 0x18);   /* GYRO_CONFIG：±2000°/s */
    mpu_write_reg(0x1C, 0x01);   /* ACCEL_CONFIG：±4g */
    return 0;
}

/* ---------- 姿态解算（互补滤波，dt=0.02s 固定） ---------- */
u8 MPU6050_GetAngle(s16 *pitch_x100, s16 *roll_x100, s16 *yaw_x100)
{
    static float s_roll = 0.0f, s_pitch = 0.0f, s_yaw = 0.0f;
    u8 buf[14];
    s16 ax, ay, az, gx, gy, gz;
    float acc_roll, acc_pitch;
    float gyr_roll, gyr_pitch, gyr_yaw;
    const float dt = 0.02f;

    if (mpu_read_regs(0x3B, buf, 14)) {
        /* 读失败时【保持上一次的角度】并返回错误码，绝不清零。
           原来这里是把三个角度都写 0 —— 那等于告诉上层"车是水平的"，
           而水平恰恰是最常见、最不会引起怀疑的姿态：模块虚焊、排线松了，
           大屏上照样显示一个稳稳的 0°，没有任何人会发现姿态数据已经死了。
           传感器失败必须能被上层区分出来，不能伪装成一个合法读数。 */
        *roll_x100  = (s16)(s_roll  * 100.0f);
        *pitch_x100 = (s16)(s_pitch * 100.0f);
        *yaw_x100   = (s16)(s_yaw   * 100.0f);
        return 1;
    }
    ax = (s16)((u16)((u16)buf[0] << 8) | buf[1]);
    ay = (s16)((u16)((u16)buf[2] << 8) | buf[3]);
    az = (s16)((u16)((u16)buf[4] << 8) | buf[5]);
    gx = (s16)((u16)((u16)buf[8] << 8) | buf[9]);
    gy = (s16)((u16)((u16)buf[10] << 8) | buf[11]);
    gz = (s16)((u16)((u16)buf[12] << 8) | buf[13]);

    /* 加速度计算俯仰/横滚（单位 °） */
    acc_roll  = atan2f((float)ay, (float)az) * 57.29578f;
    acc_pitch = atan2f(-(float)ax, sqrtf((float)ay * (float)ay + (float)az * (float)az))
                * 57.29578f;

    /* 陀螺仪角速度积分（°/s × s = °） */
    gyr_roll  = (float)gx / 16.4f * dt;
    gyr_pitch = (float)gy / 16.4f * dt;
    gyr_yaw   = (float)gz / 16.4f * dt;

    /* 互补滤波：陀螺仪(高带宽) + 加速度计(校正漂移) */
    s_roll  = 0.98f * (s_roll  + gyr_roll)  + 0.02f * acc_roll;
    s_pitch = 0.98f * (s_pitch + gyr_pitch) + 0.02f * acc_pitch;
    s_yaw  += gyr_yaw;     /* 无磁力计，偏航角会缓慢漂移 */
    /* 归一化到 (-180, 180]。这一步不是"锦上添花"，是必须的：
       没有磁力计，偏航靠陀螺仪积分，会一直朝一个方向漂。而输出是 s16 的
       0.01° 单位，量程只到 ±327.67° —— 漂过这个数就整数溢出翻转，
       曲线上会凭空出现一条从 +327 跳到 -327 的垂直线，看着像"车瞬间掉头"。
       这是必然发生的（只是早晚），不是偶发。
       用 while 而不是取模：dt=20ms 一帧最多转几度，最多循环一两次，
       比 fmodf 便宜，也不用担心负数取模的实现差异。 */
    while (s_yaw >  180.0f) s_yaw -= 360.0f;
    while (s_yaw <= -180.0f) s_yaw += 360.0f;

    *roll_x100  = (s16)(s_roll  * 100.0f);
    *pitch_x100 = (s16)(s_pitch * 100.0f);
    *yaw_x100   = (s16)(s_yaw   * 100.0f);
    return 0;
}
