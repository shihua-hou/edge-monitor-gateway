/************************************************************
 * dht11.c - DHT11 温湿度传感器驱动（单总线，HAL 版）
 * 引脚：战舰板 PG11
 * 时序、校验逻辑与标准库版本完全一致，只是把 GPIO 配置/读写换成 HAL API。
 * 注意：DHT11 最小采样间隔 1s，调用频率必须 ≥1s
 ************************************************************/
#include "dht11.h"
#include "./SYSTEM/delay/delay.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

#define DHT11_PORT  GPIOG
#define DHT11_PIN   GPIO_PIN_11

/* 直接读 IDR 寄存器，不走 HAL_GPIO_ReadPin()——DHT11 每个 bit 的判断窗口
   只有几十微秒，HAL_GPIO_ReadPin 内部的 assert_param + 分支判断这层函数
   调用开销，在如此短的时间窗口里占比已经不可忽略，这很可能就是"读几个
   bit 后开始失步、后面全错"的真正原因，而不是算法逻辑本身有问题。 */
#define DHT11_READ()  ((DHT11_PORT->IDR & DHT11_PIN) != 0)

static void dht11_out(void)
{
    GPIO_InitTypeDef g = {0};
    g.Pin = DHT11_PIN;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(DHT11_PORT, &g);
}

static void dht11_in(void)
{
    GPIO_InitTypeDef g = {0};
    g.Pin = DHT11_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;      /* 上拉输入 */
    HAL_GPIO_Init(DHT11_PORT, &g);
}

/* 读 1 bit：等前导 50us 低电平结束后，只测"高电平实际持续了多久"（原始
 * width，单位约 1us），不在这里判 0/1——固定阈值（不管是原来卡点式的
 * 40us，还是后来测脉宽配 50us）都是在赌 delay_us() 的绝对精度，赌了好
 * 几轮都没赌对。改成把 40 个 bit 的 width 全部测出来，交给上层用这批
 * 数据自己的最大/最小值算动态阈值——0 bit(标称26~28us)和1 bit(标称70us)
 * 之间有将近 3 倍的差距，只要 delay_us 有稳定的比例关系（不管整体偏大
 * 还是偏小），动态阈值都能自动找到正确的分界点，不需要猜一个绝对数字。 */
static u16 dht11_read_bit_width(void)
{
    u8 retry = 0;
    u16 width = 0;

    while (!DHT11_READ()) {
        if (++retry > 100) return 0;
        delay_us(1);
    }
    while (DHT11_READ()) {
        delay_us(1);
        if (++width > 200) break;   /* 总线异常兜底，不死等 */
    }
    return width;
}

/* 主机启动时序：拉低 ≥18ms，再拉高 20~40us */
static void dht11_rst(void)
{
    dht11_out();
    HAL_GPIO_WritePin(DHT11_PORT, DHT11_PIN, GPIO_PIN_RESET);
    delay_ms(20);
    HAL_GPIO_WritePin(DHT11_PORT, DHT11_PIN, GPIO_PIN_SET);
    delay_us(30);
}

/* 等待 DHT11 响应：80us 低 + 80us 高 */
static u8 dht11_check(void)
{
    u8 retry = 0;
    dht11_in();
    while (DHT11_READ()) {  /* 等总线被拉低 */
        if (++retry > 100) return 1;
        delay_us(1);
    }
    retry = 0;
    while (!DHT11_READ()) { /* 80us 低 */
        if (++retry > 100) return 1;
        delay_us(1);
    }
    retry = 0;
    while (DHT11_READ()) {  /* 80us 高 */
        if (++retry > 100) return 1;
        delay_us(1);
    }
    return 0;
}

void DHT11_Init(void)
{
    __HAL_RCC_GPIOG_CLK_ENABLE();
    dht11_out();
    HAL_GPIO_WritePin(DHT11_PORT, DHT11_PIN, GPIO_PIN_SET);   /* 空闲拉高 */
}

/* 读一次温湿度：40bit（湿度整数/小数 + 温度整数/小数 + 校验） */
u8 DHT11_Read_Data(s16 *temp_x10, s16 *humi_x10)
{
    u8 buf[5], i, j, check_fail;
    u16 widths[40];
    u16 min_w = 0xFFFF, max_w = 0, threshold;

    dht11_rst();

    /* 不用 taskENTER_CRITICAL 包这段——实测过会导致后续代码彻底执行不到，
       原因没再深究，直接不用。直接读 IDR 寄存器（DHT11_READ 宏）本身已经
       把每次判断的开销降到最低。 */
    check_fail = dht11_check();
    if (!check_fail) {
        for (i = 0; i < 40; i++) {
            widths[i] = dht11_read_bit_width();
            if (widths[i] < min_w) min_w = widths[i];
            if (widths[i] > max_w) max_w = widths[i];
        }
    }

    if (check_fail) return 1;   /* 起始响应超时 */

    /* 动态阈值 = 本次实测 40 个脉宽的最大/最小值中点，不赌固定数字。
     *
     * 但这个办法有个前提：这 40 位里必须【同时存在 0 和 1】，中点才落在
     * 两簇数据之间。如果所有脉宽都差不多（总线被拉死、模块没供电导致
     * 每一位都超时返回同一个值），中点就落在数据自己身上，全部判成 0 ——
     * 而全 0 的校验和恰好也是 0，能顺利通过下面的校验，于是上报一个
     * "0℃ / 0%" 的假读数。这种带着合法外衣的错误比直接报错危险得多。
     *
     * 0 bit 标称 26~28us、1 bit 标称 70us，正常数据的极差应该有两倍以上。
     * 极差过小就说明这批脉宽没有区分度，判定为读取失败。 */
    if (max_w < min_w * 2 || max_w - min_w < 8) {
        printf("DHT11 bad pulse spread: min=%u max=%u (bus stuck or no power?)\r\n",
               min_w, max_w);
        return 2;
    }
    threshold = (u16)((min_w + max_w) / 2);
    /* 这里原本每次采样都打一行 min/max/thr，是当初调这套动态阈值时用来
       观察脉宽分布的。现在读数已经稳定，每秒一行会把串口刷满，正常路径
       上不再打印；真出问题时下面校验和失败那条会带上原始字节，够定位了。
       需要重新观察脉宽分布时，把 DHT11_DEBUG_WIDTH 打开即可 */
#ifdef DHT11_DEBUG_WIDTH
    printf("DHT11 widths min=%u max=%u thr=%u\r\n", min_w, max_w, threshold);
#endif

    for (i = 0; i < 5; i++) {
        buf[i] = 0;
        for (j = 0; j < 8; j++) {
            buf[i] <<= 1;
            if (widths[i * 8 + j] > threshold) buf[i] |= 0x01;
        }
    }

    if ((u8)(buf[0] + buf[1] + buf[2] + buf[3]) != buf[4]) {  /* 校验和不对 */
        /* 失败才打印，正常路径保持安静。带上脉宽区间，校验和错通常就是
           阈值把某几位判反了，这两个数是判断"是不是又回到当初那个问题"
           最直接的依据 */
        printf("DHT11 checksum err: %02X %02X %02X %02X %02X (sum=%02X) width %u~%u\r\n",
               buf[0], buf[1], buf[2], buf[3], buf[4],
               (u8)(buf[0] + buf[1] + buf[2] + buf[3]), min_w, max_w);
        return 2;
    }

    *humi_x10 = (s16)(buf[0] * 10 + buf[1]);
    /* DHT11 协议里 buf[2] 最高位是温度符号位（1=负温度），代码从最早的
       标准库版本开始就一直没处理过这一位，一直当无符号数直接乘 10 加，
       只是之前没在这个温度区间的场景下暴露出来——现在补上 */
    if (buf[2] & 0x80) {
        *temp_x10 = -(s16)(((buf[2] & 0x7F) * 10) + buf[3]);
    } else {
        *temp_x10 = (s16)(buf[2] * 10 + buf[3]);
    }
    return 0;
}
