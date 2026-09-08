/************************************************************
 * hcsr04.c - HC-SR04 超声波测距驱动（HAL 版）
 * 引脚：TRIG=PE6（输出）, ECHO=PB6（TIM4_CH1 输入捕获）
 * 测距原理：触发 10us 高电平 → ECHO 输出高电平时长 ∝ 距离
 *           距离(cm) = 高电平时间(us) / 58
 * 注意：① 测量间隔建议 ≥60ms
 *       ② 若工程内已有其他 TIM4 例程，需合并 TIM4_IRQHandler
 *       ③ 战舰板 PB6/PB7 为板载 EEPROM(I2C1)，不使用 I2C 时无冲突
 ************************************************************/
#include "hcsr04.h"
#include "./SYSTEM/delay/delay.h"
#include <stdio.h>      /* printf：初始化失败时把原因打到串口 */

#define TRIG_PORT  GPIOE
#define TRIG_PIN   GPIO_PIN_6
#define ECHO_PORT  GPIOB
#define ECHO_PIN   GPIO_PIN_6   /* TIM4_CH1 */

static TIM_HandleTypeDef s_htim4;
static volatile u8  s_echo_flag = 0;  /* 0=等上升沿 1=已捕获 2=完成 */
static volatile u16 s_t1 = 0, s_t2 = 0;

/* 外设是否就绪。初始化失败时 GetDistance 直接返回 0（等同"无回波"），
   不去操作一个没配置好的定时器 */
static u8 s_tim_ready = 0;

u8 HC_SR04_Init(void)
{
    GPIO_InitTypeDef g = {0};
    TIM_IC_InitTypeDef ic = {0};

    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_TIM4_CLK_ENABLE();

    /* TRIG=PE6 推挽输出 */
    g.Pin = TRIG_PIN;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(TRIG_PORT, &g);
    HAL_GPIO_WritePin(TRIG_PORT, TRIG_PIN, GPIO_PIN_RESET);

    /* ECHO=PB6 输入，带下拉（TIM4_CH1 复用）。
       不用浮空：ECHO 这根线一旦接触不良或没插牢，浮空脚会被旁边
       TRIG 的跳变容性耦合拱成高电平并悬在那里，表现成"有上升沿、
       永远等不到下降沿"——看起来像模块坏了，实际是线没接上。
       模块推挡输出，内部下拉约 40k，接好时完全不影响。 */
    g.Pin = ECHO_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(ECHO_PORT, &g);

    /* TIM4：预分频 71 → 1MHz 计数（1us/计数值），ARR=0xFFFF 约 65ms */
    s_htim4.Instance = TIM4;
    s_htim4.Init.Prescaler = 71;
    s_htim4.Init.CounterMode = TIM_COUNTERMODE_UP;
    s_htim4.Init.Period = 0xFFFF;
    s_htim4.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    /* 初始化失败绝不能 while(1) 死等：这个函数是在最高优先级的采集任务里
       调用的，卡在这里 = 整机停摆 = 4 秒后被看门狗复位 = 重启后再次卡在
       同一行，无限重启循环，而且串口上一个字都没有。
       一个外设配不起来，正确的做法是标记成不可用、让系统带着这个残疾继续
       跑，把故障如实上报给上层。（同 adc_light.c） */
    if (HAL_TIM_IC_Init(&s_htim4) != HAL_OK) {
        printf("TIM4 IC init fail (ultrasonic disabled)\r\n");
        return 1;
    }

    /* CH1 上升沿捕获 */
    ic.ICPolarity = TIM_ICPOLARITY_RISING;
    ic.ICSelection = TIM_ICSELECTION_DIRECTTI;
    ic.ICPrescaler = TIM_ICPSC_DIV1;
    ic.ICFilter = 0x0F;
    if (HAL_TIM_IC_ConfigChannel(&s_htim4, &ic, TIM_CHANNEL_1) != HAL_OK) {
        printf("TIM4 IC channel config fail (ultrasonic disabled)\r\n");
        return 1;
    }

    HAL_NVIC_SetPriority(TIM4_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(TIM4_IRQn);

    HAL_TIM_IC_Start_IT(&s_htim4, TIM_CHANNEL_1);
    s_tim_ready = 1;
    return 0;
}

/* 输入捕获回调：上升沿记 t1 并切换下降沿，下降沿记 t2 */
void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIM4 || htim->Channel != HAL_TIM_ACTIVE_CHANNEL_1) return;

    if (s_echo_flag == 0) {
        s_t1 = (u16)HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_1);
        __HAL_TIM_SET_CAPTUREPOLARITY(htim, TIM_CHANNEL_1, TIM_INPUTCHANNELPOLARITY_FALLING);
        s_echo_flag = 1;
    } else if (s_echo_flag == 1) {
        s_t2 = (u16)HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_1);
        s_echo_flag = 2;
    }
}

void TIM4_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&s_htim4);
}

u16 HC_SR04_GetDistance(void)
{
    u32 w;
    /* 声明必须全部写在语句前面：ARMCC 默认按 C90 编，
       "声明后面才允许出现语句"这条规矩在这里是硬性的 */
    const u16 TIMEOUT_US = 35000;   /* 无回波超时，略大于 HC-SR04 最大量程往返时间(~35ms) */

    /* 定时器没配起来就直接当"无回波"。不判的话下面会去读一个未初始化的
       定时器计数值，那个值不受控，超时判断随之失效——最坏情况是
       while 循环永远出不去，把最高优先级的采集任务锁死 */
    if (!s_tim_ready) return 0;

    s_echo_flag = 0;
    __HAL_TIM_SET_COUNTER(&s_htim4, 0);
    __HAL_TIM_SET_CAPTUREPOLARITY(&s_htim4, TIM_CHANNEL_1, TIM_INPUTCHANNELPOLARITY_RISING);

    /* 触发 10us 高电平 */
    HAL_GPIO_WritePin(TRIG_PORT, TRIG_PIN, GPIO_PIN_SET);
    delay_us(10);
    HAL_GPIO_WritePin(TRIG_PORT, TRIG_PIN, GPIO_PIN_RESET);

    /* 阻塞等待回波：用 TIM4 自身 1MHz 计数值判断真实耗时(us)（触发前已清零），
       而不是裸循环计数——裸计数的超时时长和 CPU 主频/编译优化强绑定，
       换个主频或优化等级实际等待时间就会跟着漂移，不是真正的超时保护 */
    while (s_echo_flag != 2) {
        if (__HAL_TIM_GET_COUNTER(&s_htim4) > TIMEOUT_US) return 0;   /* 超时无回波 */
    }

    /* 脉宽（处理 16 位计数溢出回绕） */
    w = (s_t2 >= s_t1) ? (u32)(s_t2 - s_t1)
                       : ((u32)0xFFFF - s_t1 + s_t2 + 1);
    return (u16)(w / 58);   /* 距离 cm */
}
