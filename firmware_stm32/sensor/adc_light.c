/************************************************************
 * adc_light.c - ADC1 的独占持有者，目前跑两路：
 *     光敏电阻   PA1  = ADC1_CH1
 *     电池电压   PC5  = ADC1_CH15
 *
 * 为什么电池也写在这个文件里：ADC1 是一个外设，只能有一个所有者。
 * 两个模块各自 HAL_ADC_Init 同一个 ADC1，后初始化的会把前一个的配置
 * 静静覆盖掉。文件名已经不够贴切，但重命名要动 Keil 工程和同步脚本，
 * 调试期间不值得为改名引入构建风险。
 *
 * 单次转换模式下切通道的做法：每次转换前重新 ConfigChannel。
 * 不用扫描模式+DMA 是因为两路都是秒级采样，上 DMA 是杀鸡用牛刀。
 ************************************************************/
#include "adc_light.h"
#include <stdio.h>      /* printf：初始化失败时把原因打到串口 */

/* 模块板载分压：R13=100k / R15=10k，所以 Vadc = Vbat / 11。
   12V 进去只有 1.09V，对 3.3V 的 ADC 完全安全。 */
#define BAT_DIV_RATIO   11

/* 低于这个值就当"那根线根本没接"，而不是"电池没电了"。
   模块的输入范围是 4.5~15V，真实电池不可能低于 3V 还在供电；
   而浮空的 ADC 脚读出来就是 0 附近。
   这两种情况必须分开：把"没接线"报成"电池 0.0V"是假告警，
   而假告警看多了，真告警就也没人信了。 */
#define BAT_MIN_VALID_MV  3000

static ADC_HandleTypeDef s_hadc1;
/* 初始化是否成功。失败后 GetPercent 直接返回错误，不去碰没配好的外设 */
static u8 s_adc_ready = 0;

u8 ADC_Light_Init(void)
{
    GPIO_InitTypeDef g = {0};
    ADC_ChannelConfTypeDef ch = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_ADC1_CLK_ENABLE();
    /* ADC 时钟 = 72/6 = 12MHz（F1 系列 ADC 时钟上限 14MHz，必须分频）；
       宏名和参数已经对照 EdgeMonitor_App 实际带的 stm32f1xx_hal_rcc_ex.h 核实过 */
    __HAL_RCC_ADC_CONFIG(RCC_ADCPCLK2_DIV6);

    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* PA1 光敏 / PC5 电池，都配成模拟输入 */
    g.Pin = GPIO_PIN_1;
    g.Mode = GPIO_MODE_ANALOG;
    HAL_GPIO_Init(GPIOA, &g);
    g.Pin = GPIO_PIN_5;
    HAL_GPIO_Init(GPIOC, &g);

    /* ADC1 规则通道，单次转换 */
    s_hadc1.Instance = ADC1;
    s_hadc1.Init.ScanConvMode = DISABLE;
    s_hadc1.Init.ContinuousConvMode = DISABLE;
    s_hadc1.Init.DiscontinuousConvMode = DISABLE;
    s_hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    s_hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    s_hadc1.Init.NbrOfConversion = 1;
    /* 初始化失败绝不能用 while(1) 死等。这个函数是在采集任务里调用的，
       而采集任务是最高优先级——卡在这里 = 整个系统停摆 = 4 秒后被看门狗
       复位 = 重启后再次卡在同一行，无限重启循环。而且串口上一个字都没有，
       现场只看到"板子一直在重启"，完全无从下手。
       一个外设初始化不了，正确的做法是标记成不可用、让系统带着这个残疾
       继续跑，把故障如实上报给上层，而不是把整机拖死。 */
    if (HAL_ADC_Init(&s_hadc1) != HAL_OK) {
        printf("ADC init fail (light sensor disabled)\r\n");
        return 1;
    }

    ch.Channel = ADC_CHANNEL_1;
    ch.Rank = ADC_REGULAR_RANK_1;
    ch.SamplingTime = ADC_SAMPLETIME_239CYCLES_5;
    if (HAL_ADC_ConfigChannel(&s_hadc1, &ch) != HAL_OK) {
        printf("ADC channel config fail (light sensor disabled)\r\n");
        return 1;
    }

    HAL_ADCEx_Calibration_Start(&s_hadc1);
    s_adc_ready = 1;
    return 0;
}

/* 读一路原始值。单次转换模式下每次都要先把通道配好——
   不重配的话读到的永远是上一个通道，而那个值看起来完全合理。 */
static u8 adc_read_raw(u32 channel, u16 *raw)
{
    ADC_ChannelConfTypeDef ch = {0};

    if (!s_adc_ready) return 1;

    ch.Channel = channel;
    ch.Rank = ADC_REGULAR_RANK_1;
    ch.SamplingTime = ADC_SAMPLETIME_239CYCLES_5;
    if (HAL_ADC_ConfigChannel(&s_hadc1, &ch) != HAL_OK) return 1;

    HAL_ADC_Start(&s_hadc1);
    /* 必须检查转换是否真的完成。超时时 HAL_ADC_GetValue 读到的是上一次的
       残留值——一个"看起来完全正常"的旧数字，正是最难发现的那种故障 */
    if (HAL_ADC_PollForConversion(&s_hadc1, 10) != HAL_OK) {
        HAL_ADC_Stop(&s_hadc1);
        return 1;
    }
    *raw = (u16)HAL_ADC_GetValue(&s_hadc1);
    HAL_ADC_Stop(&s_hadc1);
    if (*raw > 4095) *raw = 4095;
    return 0;
}

u8 ADC_Battery_GetMv(u16 *mv)
{
    u16 raw;
    u32 v;

    if (adc_read_raw(ADC_CHANNEL_15, &raw) != 0) return 1;

    v = (u32)raw * 3300 / 4095;      /* 引脚电压 mV */
    v *= BAT_DIV_RATIO;              /* 回推电池电压 */

    if (v < BAT_MIN_VALID_MV) { *mv = 0; return 2; }   /* 2 = 未接线，不当故障 */
    *mv = (u16)v;
    return 0;
}

u8 ADC_Light_GetPercent(u8 *percent)
{
    u16 v;

    if (adc_read_raw(ADC_CHANNEL_1, &v) != 0) return 1;

    /* 分压方向取决于光敏电阻在电路里接的位置（上拉还是下拉），不是软件
       能预先假设对的——实测这块板子/模块是遮光后 ADC 值变大，说明光敏
       电阻在上拉位置：光越强→阻值越小→分压点越靠近 GND→ADC 值越小。
       跟最初想当然写的"光越强电压越高"方向正好相反，用 100-v 取反。 */
    *percent = (u8)(100 - (u32)v * 100 / 4095);
    return 0;
}
