/************************************************************
 * motor.c - TB6612 双路电机驱动 + 正交编码器 PI 闭环
 *
 * 模块：WHEELTEC TB6612 稳压版（D153C）。A 路=左轮，B 路=右轮。
 *
 * 接线（全部集中在战舰板 P2 左排，不跨排子）：
 *
 *   模块 H1 = Input_IO        模块 H2 = Output_IO
 *   H1-7 PWMA -> PC6          H2-6 GND  -> GND   <== 必接
 *   H1-1 PWMB -> PC7          H2-4 E1A  -> PB4   左轮编码器
 *   H1-5 AIN1 -> PC8          H2-3 E1B  -> PB5
 *   H1-6 AIN2 -> PC9          H2-2 E2A  -> PA15  右轮编码器
 *   H1-3 BIN1 -> PC10         H2-1 E2B  -> PB3
 *   H1-2 BIN2 -> PC11         H2-5 ADC  -> 不接（电池电压，固件未实现）
 *   H1-4 STBY -> PA8
 *
 *   H1 那排 7 个脚全是信号、一根 GND 都没有，共地必须从 H2-6 引。
 *   不共地的话逻辑电平没有参考，行为完全不可预期。
 *
 * 电源：12V 只进模块的 J8。主控【不要】从模块的 5V 取电——模块过温保护
 * 会切断板载 5V/3.3V，电机堵转发热正是最需要遥测的时刻，主控却跟着掉电了。
 ************************************************************/
#include "motor.h"
#include <stdio.h>

/* ===================== 引脚配置 =====================
 * 全部集中在战舰板的 **P2（左排）**，13 根线不用跨排子插。
 * 依据是战舰 V3 的装配图（丝印上印了 P1/P2 的引脚顺序）。
 *
 * 为什么不用 PA6/PA7 做左轮编码器（TIM3 原生引脚）：
 *   ① 它们在 P1（右排），会造成接线跨两排；
 *   ② 战舰板上 PA5/PA6/PA7 是 SPI1，挂着 Flash 和 NRF24L01 座，
 *      片选一旦被拉低，Flash 就会驱动 MISO 和编码器打架——
 *      而且现象极隐：静止时读数正常，一给电机命令才乱。
 *   改用 TIM3 部分重映射到 PB4/PB5，两个问题一起解决。
 */
/* PWM：TIM8_CH1=PC6(A/左)  TIM8_CH2=PC7(B/右)。
   选 TIM8 是因为 TIM4 被超声波输入捕获占了，而 TIM8 全工程空闲，
   PC6/PC7 又正好是它 CH1/CH2 的原生引脚，不需要重映射。 */
#define PWM_PORT        GPIOC
#define PWM_A_PIN       GPIO_PIN_6
#define PWM_B_PIN       GPIO_PIN_7

/* 方向：PC8~PC11。这四个脚在板上接 TF 卡座(SDIO)，不插卡时悬空，
   当普通输出用没问题。⚠️ 插了 TF 卡就会冲突——这个项目用不到 SD 卡。 */
#define DIR_PORT        GPIOC
#define AIN1_PIN        GPIO_PIN_8
#define AIN2_PIN        GPIO_PIN_9
#define BIN1_PIN        GPIO_PIN_10
#define BIN2_PIN        GPIO_PIN_11

/* STBY 给 PA8：它是 P2 上最干净的一个脚（不接任何板载外设）。
   急停是安全相关的信号，把最干净的脚给它。 */
#define STBY_PORT       GPIOA
#define STBY_PIN        GPIO_PIN_8

/* 编码器（都在 P2）：
     左轮 = TIM3 部分重映射 -> CH1=PB4, CH2=PB5
     右轮 = TIM2 部分重映射1 -> CH1=PA15, CH2=PB3

   PA15/PB3/PB4 上电后分别是 JTDI/JTDO/NJTRST，
   __HAL_AFIO_REMAP_SWJ_NOJTAG() 一次把三个都释放了，SWD 不受影响。

   TIM2 用"部分重映射1"(01)而不是完全重映射(11)：两者的 CH1/CH2 一样都是
   PA15/PB3，但完全重映射会把 CH3/CH4 挂到 PB10/PB11——那是 MPU6050 的软件 I2C。
   虽然不配那两个通道就不会真的占用引脚，但没必要留这个隱患。 */
#define ENC_L_TIM       TIM3
#define ENC_R_TIM       TIM2

/* 编码器符号校正：让"正 = 小车前进"对两侧都成立。
 *
 * 为什么不对称（实测得出，不是接错线）：
 *   WHEELTEC 这块模块的两个电机接口，**电机极性是镜像的**——
 *     MOTORC2(A/左)：1=AOUT1  6=AOUT2
 *     MOTORC1(B/右)：1=BOUT2  6=BOUT1   ← 反的
 *   这是故意的：左右电机对称安装时，同样的 IN1/IN2 电平能让车直走。
 *   但**编码器引脚没有跟着镜像**（两边都是 3=ExB, 4=ExA），
 *   于是两个电机物理上反向旋转时，编码器必然输出相反的符号。
 *
 * 不改的后果已经实测过：左轮的 PI 拿到的是**反相反馈**，
 * 目标 +40、实测 -55 -> 误差 95 -> 加大输出 -> 转得更快 -> 读数更负……
 * **负反馈变成了正反馈**，一路冲到 -488 RPM。轮子垂高了才没出事。
 *
 * 标定方法：电机不通电，用手把两个轮子都往车头方向转，
 * 看 0x102 里哪侧 RPM 是负的，就把那侧置 -1。换底盘/改接线后要重标。 */
#define ENC_L_SIGN      (-1)
#define ENC_R_SIGN      (+1)

/* PWM 20kHz：72MHz / (0+1) / 3600 = 20kHz。
   为什么必须 20k 以上：可听频率上限约 20kHz，低于它电机会发出和占空比
   同频的啸叫，低速时尤其明显。这不是玄学，是巡检设备在楼道里跑起来
   一路尖叫的实际问题。 */
#define PWM_PSC         0
#define PWM_ARR         3599
#define PWM_MAX         3400       /* 留一点余量，不做到满占空比 */

/* PI 参数。整定方法：先把 Ki 置 0，加大 Kp 到刚出现超调，再加 Ki 消除稳态误差 */
#define PI_KP           18
/* Ki 从 6 提到 24：实测下原值要约 10 秒才接近目标，对"下一条命令马上就来"的
   遥控场景太慢了。调到 4 倍对应约 2~3 秒。再大就要开始担心超调，
   而这车的惯量和负载还没在地面上实测过，不冒进。 */
#define PI_KI           24
#define PI_INTEG_MAX    (PWM_MAX * 100)   /* 积分限幅，防止长时间堵转积出巨大输出 */

#define TICK_MS         20                 /* Motor_Tick 的调用周期，必须一致 */

/* 飞车保护。
 * 这不是预防性的“万一”，是刚发生过的事：左轮编码器符号反了，
 * 负反馈变成正反馈，目标 40 RPM 的情况下一路冲到 -488 RPM，
 * 全靠“轮子垂高了”没出事。换底盘、改接线、编码器接触不良，
 * 都能重现同一类失控——而巡检车失控时旁边未必有人。
 *
 * 判据用“转速远超上限”而不是“输出饱和”：爬坡、载重时输出饱和是
 * 正常的，但转速不会超；只有反馈出错时转速才会失控地涨。 */
#define RUNAWAY_RPM     (MOTOR_MAX_RPM * 3 / 2)   /* 300 RPM */
#define RUNAWAY_TICKS   5                          /* 连续 5 拍 = 100ms 才算数，避开单拍毛刺 */

static TIM_HandleTypeDef s_pwm;
static TIM_HandleTypeDef s_enc_l, s_enc_r;
static u8  s_ready   = 0;
static u8  s_estop   = 0;

typedef struct {
    s16 target_rpm;
    s16 rpm;
    s32 integ;
    u16 last_cnt;
    /* 里程直接累加毫米，不存原始计数。
       原先存计数、读取时再除：(cnt / 1456) * 204 / 10——整数除法先除后乘，
       不足一圈的部分全被丢掉，里程只能以**一个轮周长（204mm）为步长跳变**。
       小车走 15cm 显示 0，走 25cm 直接跳到 20——这个精度做巡检里程根本不能看。
       现在每拍先乘后除并把余数留到下一拍，精度 1mm 且长期不累计误差。 */
    u32 odom_mm;
    u32 odom_rem;      /* 上一拍除不尽的余数，单位是"计数×mm" */
} Wheel_t;

static Wheel_t s_l, s_r;
static u16 s_turn_left_ms = 0;     /* 定时转向剩余时间 */
static u8  s_runaway_cnt = 0;
static u32 s_last_ms = 0;          /* 上一拍的时刻，用于算实际周期 */
static u8  s_faulted = 0;          /* 触发过飞车保护，需要人工解除 */

/* ---------- 小工具 ---------- */
static s32 clamp32(s32 v, s32 lo, s32 hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* 编码器计数差，处理 16 位回绕。
   直接相减在跨越 0 <-> 65535 时会得到一个 6 万多的巨大值，
   闭环会瞬间被这个假差值踢飞（表现是电机毫无征兆地猛冲一下）。
   先按 u16 相减、再按 s16 解释，回绕就自然抵消了。 */
static s16 enc_delta(u16 now, u16 last)
{
    return (s16)((u16)(now - last));
}

/* ---------- 方向控制 ---------- */
/* TB6612 真值表：IN1=1 IN2=0 正转；IN1=0 IN2=1 反转；
   IN1=IN2=0 停车(滑行)；IN1=IN2=1 刹车(短接制动)。
   停车用滑行不用刹车：刹车会把电机当发电机短路，堵转时电流很大，
   对这块 1.2A/通道的片子不友好。 */
static void set_dir(u8 is_left, s8 sign)
{
    const uint16_t p1 = is_left ? AIN1_PIN : BIN1_PIN;
    const uint16_t p2 = is_left ? AIN2_PIN : BIN2_PIN;
    if (sign > 0) {
        HAL_GPIO_WritePin(DIR_PORT, p1, GPIO_PIN_SET);
        HAL_GPIO_WritePin(DIR_PORT, p2, GPIO_PIN_RESET);
    } else if (sign < 0) {
        HAL_GPIO_WritePin(DIR_PORT, p1, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(DIR_PORT, p2, GPIO_PIN_SET);
    } else {
        HAL_GPIO_WritePin(DIR_PORT, p1, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(DIR_PORT, p2, GPIO_PIN_RESET);
    }
}

static void set_duty(u8 is_left, u16 duty)
{
    if (is_left) __HAL_TIM_SET_COMPARE(&s_pwm, TIM_CHANNEL_1, duty);
    else         __HAL_TIM_SET_COMPARE(&s_pwm, TIM_CHANNEL_2, duty);
}

/* ---------- 初始化 ---------- */
static u8 enc_init(TIM_HandleTypeDef *h, TIM_TypeDef *inst)
{
    TIM_Encoder_InitTypeDef enc = {0};

    h->Instance = inst;
    h->Init.Prescaler = 0;
    h->Init.CounterMode = TIM_COUNTERMODE_UP;
    h->Init.Period = 0xFFFF;
    h->Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    h->Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    /* TI12 = 两路都计，x4 倍频。用 x4 不用 x2/x1：分辨率直接翻倍，
       低速时每个控制周期的计数才够多，闭环不至于在个位数上抖。 */
    enc.EncoderMode = TIM_ENCODERMODE_TI12;
    enc.IC1Polarity = TIM_ICPOLARITY_RISING;
    enc.IC1Selection = TIM_ICSELECTION_DIRECTTI;
    enc.IC1Prescaler = TIM_ICPSC_DIV1;
    enc.IC1Filter = 6;      /* 输入滤波，滤掉电机换向的毛刺 */
    enc.IC2Polarity = TIM_ICPOLARITY_RISING;
    enc.IC2Selection = TIM_ICSELECTION_DIRECTTI;
    enc.IC2Prescaler = TIM_ICPSC_DIV1;
    enc.IC2Filter = 6;

    if (HAL_TIM_Encoder_Init(h, &enc) != HAL_OK) return 1;
    if (HAL_TIM_Encoder_Start(h, TIM_CHANNEL_ALL) != HAL_OK) return 1;
    return 0;
}

u8 Motor_Init(void)
{
    GPIO_InitTypeDef g = {0};
    TIM_OC_InitTypeDef oc = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_AFIO_CLK_ENABLE();
    __HAL_RCC_TIM8_CLK_ENABLE();
    __HAL_RCC_TIM3_CLK_ENABLE();
    __HAL_RCC_TIM2_CLK_ENABLE();

    /* 方向 + STBY：普通推挽输出。
       上电默认全低 —— STBY=0 意味着 TB6612 处于待机、输出高阻，
       电机在初始化完成之前绝不会动。顺序反了（先使能后配方向）会有
       一小段时间输出是不确定的。 */
    g.Pin = AIN1_PIN | AIN2_PIN | BIN1_PIN | BIN2_PIN;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(DIR_PORT, &g);
    HAL_GPIO_WritePin(DIR_PORT, AIN1_PIN | AIN2_PIN | BIN1_PIN | BIN2_PIN, GPIO_PIN_RESET);

    g.Pin = STBY_PIN;                      /* STBY 在 GPIOA，单独初始化 */
    HAL_GPIO_Init(STBY_PORT, &g);
    HAL_GPIO_WritePin(STBY_PORT, STBY_PIN, GPIO_PIN_RESET);

    /* PWM 引脚：复用推挽 */
    g.Pin = PWM_A_PIN | PWM_B_PIN;
    g.Mode = GPIO_MODE_AF_PP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(PWM_PORT, &g);

    s_pwm.Instance = TIM8;
    s_pwm.Init.Prescaler = PWM_PSC;
    s_pwm.Init.CounterMode = TIM_COUNTERMODE_UP;
    s_pwm.Init.Period = PWM_ARR;
    s_pwm.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    s_pwm.Init.RepetitionCounter = 0;
    s_pwm.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_PWM_Init(&s_pwm) != HAL_OK) {
        printf("motor: TIM8 PWM init fail\r\n");
        return 1;
    }

    oc.OCMode = TIM_OCMODE_PWM1;
    oc.Pulse = 0;
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCNPolarity = TIM_OCNPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;
    oc.OCIdleState = TIM_OCIDLESTATE_RESET;
    oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;
    if (HAL_TIM_PWM_ConfigChannel(&s_pwm, &oc, TIM_CHANNEL_1) != HAL_OK ||
        HAL_TIM_PWM_ConfigChannel(&s_pwm, &oc, TIM_CHANNEL_2) != HAL_OK) {
        printf("motor: TIM8 channel config fail\r\n");
        return 1;
    }
    /* TIM8 是高级定时器，输出还受 BDTR 的 MOE 位控制。
       HAL_TIM_PWM_Start 内部会置 MOE，所以这里不用单独开——
       但如果哪天换成裸寄存器写，忘了 MOE 就会得到"配置全对、引脚没波形"
       这种最难查的现象。 */
    if (HAL_TIM_PWM_Start(&s_pwm, TIM_CHANNEL_1) != HAL_OK ||
        HAL_TIM_PWM_Start(&s_pwm, TIM_CHANNEL_2) != HAL_OK) {
        printf("motor: TIM8 PWM start fail\r\n");
        return 1;
    }

    /* 编码器引脚。
       左轮 TIM3_CH1/CH2 = PA6/PA7；右轮 TIM2 完全重映射后 CH1=PA15, CH2=PB3。

       ⚠️ 关 JTAG 保留 SWD：PA15 和 PB3 上电后默认是 JTDI/JTDO，不释放的话
       重映射拿不到这两个脚。__HAL_AFIO_REMAP_SWJ_NOJTAG 只关 JTAG，
       SWD(PA13/PA14) 完全不受影响。
       但——如果你的 J-Link 在 Keil 里配的是 JTAG 模式而不是 SW 模式，
       烧进去之后就再也连不上了（要靠 BOOT0 拉高进 ISP 才能救）。
       烧录前先确认 Keil -> Debug -> Settings -> Port 选的是 SW。 */
    __HAL_AFIO_REMAP_SWJ_NOJTAG();       /* 释放 PA15/PB3/PB4，保留 SWD */
    __HAL_AFIO_REMAP_TIM2_PARTIAL_1();   /* TIM2 CH1=PA15 CH2=PB3 */
    __HAL_AFIO_REMAP_TIM3_PARTIAL();     /* TIM3 CH1=PB4  CH2=PB5 */

    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;                /* 编码器多为开漏输出，需要上拉 */
    g.Pin = GPIO_PIN_15;                 /* PA15 右轮 A 相 */
    HAL_GPIO_Init(GPIOA, &g);
    g.Pin = GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5;  /* PB3 右B相 / PB4 左A相 / PB5 左B相 */
    HAL_GPIO_Init(GPIOB, &g);

    if (enc_init(&s_enc_l, ENC_L_TIM) != 0) {
        printf("motor: left encoder init fail\r\n");
        return 1;
    }
    if (enc_init(&s_enc_r, ENC_R_TIM) != 0) {
        printf("motor: right encoder init fail\r\n");
        return 1;
    }

    s_l.last_cnt = (u16)__HAL_TIM_GET_COUNTER(&s_enc_l);
    s_r.last_cnt = (u16)__HAL_TIM_GET_COUNTER(&s_enc_r);

    /* 一切就绪才解除待机 */
    HAL_GPIO_WritePin(STBY_PORT, STBY_PIN, GPIO_PIN_SET);
    s_ready = 1;
    s_estop = 0;
    return 0;
}

/* ---------- 对外接口 ---------- */
void Motor_SetTargetRpm(s16 left, s16 right)
{
    if (!s_ready) return;
    s_l.target_rpm = (s16)clamp32(left,  -MOTOR_MAX_RPM, MOTOR_MAX_RPM);
    s_r.target_rpm = (s16)clamp32(right, -MOTOR_MAX_RPM, MOTOR_MAX_RPM);
    s_turn_left_ms = 0;
}

void Motor_Turn(s16 left_rpm, s16 right_rpm, u16 ms)
{
    Motor_SetTargetRpm(left_rpm, right_rpm);
    s_turn_left_ms = ms;
}

void Motor_Stop(void)
{
    s_l.target_rpm = s_r.target_rpm = 0;
    s_l.integ = s_r.integ = 0;
    s_turn_left_ms = 0;
    set_duty(1, 0); set_duty(0, 0);
    set_dir(1, 0);  set_dir(0, 0);
}

void Motor_EmergencyStop(void)
{
    s_estop = 1;
    Motor_Stop();
    HAL_GPIO_WritePin(STBY_PORT, STBY_PIN, GPIO_PIN_RESET);
}

void Motor_Resume(void)
{
    if (!s_ready) return;
    s_estop = 0;
    s_faulted = 0;            /* 人工确认过了，清掉飞车标志 */
    s_runaway_cnt = 0;
    HAL_GPIO_WritePin(STBY_PORT, STBY_PIN, GPIO_PIN_SET);
}

u8 Motor_IsRunning(void)
{
    if (!s_ready || s_estop) return 0;
    return (s_l.target_rpm || s_r.target_rpm || s_l.rpm || s_r.rpm) ? 1 : 0;
}

void Motor_GetState(s16 *rpm_l, s16 *rpm_r, u32 *odom_l_cm, u32 *odom_r_cm)
{
    if (rpm_l) *rpm_l = s_l.rpm;
    if (rpm_r) *rpm_r = s_r.rpm;
    if (odom_l_cm) *odom_l_cm = s_l.odom_mm / 10;
    if (odom_r_cm) *odom_r_cm = s_r.odom_mm / 10;
}

/* ---------- 闭环 ---------- */
static void wheel_tick(Wheel_t *w, TIM_HandleTypeDef *enc, u8 is_left, u32 dt_ms)
{
    u16 now = (u16)__HAL_TIM_GET_COUNTER(enc);
    s16 d = (s16)(enc_delta(now, w->last_cnt) * (is_left ? ENC_L_SIGN : ENC_R_SIGN));
    s32 err, out;
    s8  sign;
    u32 num;

    w->last_cnt = now;

    /* 里程：只累加走过的路程，不分正负。
       溢出核算：200RPM 时每 20ms 约 97 个计数，97×204 + 余数 < 2万，
       离 u32 上限很远；而 odom_mm 要跑到 4295 公里才会翻。 */
    num = (u32)((d >= 0) ? d : -d) * WHEEL_CIRC_MM + w->odom_rem;
    w->odom_mm  += num / ENC_CNT_PER_REV;
    w->odom_rem  = num % ENC_CNT_PER_REV;

    /* 计数 -> RPM，用**实际经过的时间**而不是假设的 TICK_MS。
     *
     * 为什么不能假设固定周期（实测出来的）：同一个采集循环里有两个
     * **阻塞式**读取——超声波每 200ms 等回波（183cm 约 11ms，无回波时最多 35ms），
     * DHT11 每 1s 约 25ms。那一拍的真实间隔是 31ms 而不是 20ms，编码器多走的计数
     * 就被当成了"转得更快"，下一拍又偏少。
     *
     * 现象：0x102 上周期性出现 78 RPM 的尖峰和 4 RPM 的凹陷，**两轮同时**（因为
     * 卡的是整个循环），而且上报周期 200ms 和超声波周期 200ms 相对漂移，
     * 形成一个约 23 帧的拍频。这个假尖峰还会直接灌进 PI 当成巨大误差。
     *
     * 用实测 dt 之后，不管循环被阻塞多久，算出来的转速都是对的。 */
    w->rpm = (s16)(((s32)d * 60000 / (s32)dt_ms) / (s32)ENC_CNT_PER_REV);

    if (w->target_rpm == 0) {
        /* 目标为 0 时把积分清掉。不清的话，停车前积累的积分会在下次启动时
           一股脑释放出去，车会突然窜一下——很危险，也很难查。 */
        w->integ = 0;
        set_duty(is_left, 0);
        set_dir(is_left, 0);
        return;
    }

    err = (s32)w->target_rpm - (s32)w->rpm;
    w->integ = clamp32(w->integ + err, -PI_INTEG_MAX, PI_INTEG_MAX);
    out = (PI_KP * err) + (PI_KI * w->integ) / 100;
    out = clamp32(out, -PWM_MAX, PWM_MAX);

    sign = (out > 0) ? 1 : ((out < 0) ? -1 : 0);
    if (out < 0) out = -out;
    set_dir(is_left, sign);
    set_duty(is_left, (u16)out);
}

void Motor_Tick(void)
{
    if (!s_ready) return;

    if (s_estop) {                 /* 急停期间只更新反馈，不驱动 */
        s_last_ms = HAL_GetTick();  /* 不更新的话，解除急停后第一拍的 dt 是整段急停时长 */
        s_l.rpm = s_r.rpm = 0;
        s_l.last_cnt = (u16)__HAL_TIM_GET_COUNTER(&s_enc_l);
        s_r.last_cnt = (u16)__HAL_TIM_GET_COUNTER(&s_enc_r);
        return;
    }

    /* 定时转向倒计时。放在闭环之前：这一拍就该停的话，本拍就不要再驱动了 */
    if (s_turn_left_ms) {
        if (s_turn_left_ms <= TICK_MS) { s_turn_left_ms = 0; Motor_Stop(); }
        else s_turn_left_ms -= TICK_MS;
    }

    {
        /* 实测周期。首次调用和急停恢复后 dt 会很大，限幅一下，
           否则一个巨大的 dt 会把转速算成接近 0，PI 会误以为轮子没转。 */
        u32 now_ms = HAL_GetTick();
        u32 dt = now_ms - s_last_ms;
        s_last_ms = now_ms;
        if (dt == 0)   dt = 1;
        if (dt > 200)  dt = TICK_MS;
        wheel_tick(&s_l, &s_enc_l, 1, dt);
        wheel_tick(&s_r, &s_enc_r, 0, dt);
    }

    /* 飞车检测：任一侧转速远超上限并持续 100ms，就拉低 STBY 硬停。
       不自动恢复：能跑到这一步说明反馈链路本身有问题，自动重试只会
       反复飞车。要恢复得发 ACT_RESUME，等于让人确认过一次。 */
    if (!s_faulted) {
        s16 al = (s16)((s_l.rpm < 0) ? -s_l.rpm : s_l.rpm);
        s16 ar = (s16)((s_r.rpm < 0) ? -s_r.rpm : s_r.rpm);
        if (al > RUNAWAY_RPM || ar > RUNAWAY_RPM) {
            if (++s_runaway_cnt >= RUNAWAY_TICKS) {
                s_faulted = 1;
                printf("motor: RUNAWAY l=%d r=%d, STBY off\n", (int)s_l.rpm, (int)s_r.rpm);
                Motor_EmergencyStop();
            }
        } else {
            s_runaway_cnt = 0;
        }
    }
}

u8 Motor_IsFaulted(void) { return s_faulted; }
