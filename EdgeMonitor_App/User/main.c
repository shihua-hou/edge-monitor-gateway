/**
 ****************************************************************************************************
 * @file        main.c
 * @brief       EdgeMonitor App 入口
 * 说明：这个工程是拿正点原子官方 HAL 版 FreeRTOS "移植实验" 例程当骨架改的，
 * 时钟配置(sys_stm32_clock_init)、延时(delay_init)、串口(usart_init)这几行
 * 保留官方原样，不重新发明——它们已经正确处理了 HAL 和 FreeRTOS 的 SysTick
 * 共用问题。原例程自带的 LCD/按键/外部SRAM 演示(freertos_demo.c)不用了，
 * 换成我们自己的 CAN + 传感器采集任务。
 ****************************************************************************************************
 */

#include "./SYSTEM/sys/sys.h"
#include "./SYSTEM/usart/usart.h"
#include "./SYSTEM/delay/delay.h"
#include "can.h"
#include "app_task.h"
#include "FreeRTOS.h"
#include "task.h"

/* 启动文件(startup_stm32f103xe.s)里导出的向量表首地址。取它的地址就是
   本次编译真正的链接基址，不用在代码里写死 */
extern u32 __Vectors;

int main(void)
{
    /* App 不在芯片复位默认的 0x08000000，中断向量表也跟着挪了，必须先把
       VTOR 指过去，否则任何中断（包括 HAL_Init 里用到的 SysTick）都会去
       0x08000000 那张 Boot 的表里找服务函数，一进中断就跑飞。
       必须在 HAL_Init() 之前设置。

       这里取 &__Vectors 而不是写死 0x08004000：A/B 双分区用的是 direct-xip
       模式，同一份源码要分别链接到 Slot A(0x08004000) 和 Slot B(0x08040000)
       编出两份固件。写死地址的话，跑在 B 槽的那份会把向量表指到 A 槽上，
       中断全部错位——而且这种错误只在升级到 B 槽之后才暴露，很难查。 */
    SCB->VTOR = (u32)&__Vectors;

    HAL_Init();                         /* HAL library init */
    sys_stm32_clock_init(RCC_PLL_MUL9); /* system clock -> 72MHz */
    delay_init(72);                     /* delay module init */
    usart_init(115200);                 /* debug uart, also used by printf */

    CAN1_Init();                        /* CAN1 init, 500Kbps */
    App_Task_Create();                  /* create sensor/report/cmd tasks */

    printf("EdgeMonitor STM32 Node boot OK\r\n");

    vTaskStartScheduler();              /* start scheduler, never returns */

    while (1);
}
