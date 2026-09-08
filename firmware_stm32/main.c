/************************************************************
 * EdgeMonitor Gateway - STM32 主程序参考（HAL 版）
 * 平台：正点原子 战舰 STM32F103ZET6（Keil5 + HAL库 + FreeRTOS）
 *
 * 这个文件只是参考/存档：实际能编译运行的工程在仓库根目录的
 * EdgeMonitor_App/（拿正点原子官方 HAL 版 FreeRTOS"移植实验"例程当骨架，
 * 加了本目录 can.c/app_task.c/sensor/ 这些文件），真正在用的入口是
 * EdgeMonitor_App/User/main.c，改动内容和下面一致：
 *
 *   - 保留官方原有的 HAL_Init() / sys_stm32_clock_init() / delay_init() /
 *     usart_init() 这几行不动——它们已经正确处理了 HAL 和 FreeRTOS 的
 *     SysTick 共用问题，没必要也没足够信息重新配一遍
 *   - 去掉官方例程自带的 LCD/按键/外部SRAM 初始化（freertos_demo.c 那套演示
 *     用不上，留在工程里但不再调用，是无害的死代码）
 *   - 换成我们自己的 CAN1_Init() + App_Task_Create()
 *
 * 延时函数用的是官方 Drivers/SYSTEM/delay/delay.h 里现成的 delay_us/delay_ms
 * （支持 FreeRTOS 感知，不用自己另外拿 DWT 写一份）。
 ************************************************************/
#include "./SYSTEM/sys/sys.h"
#include "./SYSTEM/usart/usart.h"
#include "./SYSTEM/delay/delay.h"
#include "can.h"
#include "app_task.h"
#include "FreeRTOS.h"
#include "task.h"

/* 启动文件导出的向量表首地址，取它的地址就是本次编译真正的链接基址 */
extern u32 __Vectors;

int main(void)
{
    /* App 不在芯片复位默认的 0x08000000，向量表也跟着挪了，必须在 HAL_Init()
       之前把 VTOR 指过去，否则任何中断（含 SysTick）都会去 Boot 的表里找
       服务函数，一进中断就跑飞。

       取 &__Vectors 而不是写死地址：A/B 双分区用 direct-xip 模式，同一份
       源码要分别链接到 Slot A(0x08004000) 和 Slot B(0x08040000) 编两份固件，
       写死的话跑在 B 槽那份会把向量表指到 A 槽上，中断全部错位。 */
    SCB->VTOR = (u32)&__Vectors;

    HAL_Init();
    sys_stm32_clock_init(RCC_PLL_MUL9); /* 72MHz */
    delay_init(72);
    usart_init(115200);

    CAN1_Init();
    App_Task_Create();

    printf("EdgeMonitor STM32 Node boot OK\r\n");

    vTaskStartScheduler();

    while (1);
}
