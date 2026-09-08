/**
 ****************************************************************************************************
 * @file        sys.h
 * @author      ����ԭ���Ŷ�(ALIENTEK)
 * @version     V1.0
 * @date        2020-04-20
 * @brief       ϵͳ��ʼ������(����ʱ������/�жϹ���/GPIO���õ�)
 * @license     Copyright (c) 2020-2032, �������������ӿƼ����޹�˾
 ****************************************************************************************************
 * @attention
 *
 * ʵ��ƽ̨:����ԭ�� STM32F103������
 * ������Ƶ:www.yuanzige.com
 * ������̳:www.openedv.com
 * ��˾��ַ:www.alientek.com
 * �����ַ:openedv.taobao.com
 *
 * �޸�˵��
 * V1.0 20211103
 * ��һ�η���
 *
 ****************************************************************************************************
 */

#ifndef __SYS_H
#define __SYS_H

#include "stm32f1xx.h"


/**
 * SYS_SUPPORT_OS���ڶ���ϵͳ�ļ����Ƿ�֧��OS
 * 0,��֧��OS
 * 1,֧��OS
 */
/* Boot 工程是裸机顺序执行，没有 FreeRTOS。这个模板原是 FreeRTOS 例程的骨架，
   这里改成 0——否则 stm32f1xx_it.c 里 SysTick_Handler/SVC_Handler/PendSV_Handler
   会被 #if(!SYS_SUPPORT_OS) 编译掉，指望 FreeRTOS 的 port.c 来定义，但 Boot
   工程根本没编译 port.c，SysTick_Handler 会掉回启动文件里的空弱定义，
   HAL_IncTick() 永远不被调用，delay_ms/HAL_Delay 全部失效 */
#define SYS_SUPPORT_OS          0


/*��������*******************************************************************************************/

void sys_nvic_set_vector_table(uint32_t baseaddr, uint32_t offset);             /* �����ж�ƫ���� */
void sys_standby(void);                                                         /* �������ģʽ */
void sys_soft_reset(void);                                                      /* ϵͳ����λ */
uint8_t sys_clock_set(uint32_t plln);                                           /* ʱ�����ú��� */
void sys_stm32_clock_init(uint32_t plln);                                       /* ϵͳʱ�ӳ�ʼ������ */

/* ����Ϊ��ຯ�� */
void sys_wfi_set(void);                                                         /* ִ��WFIָ�� */
void sys_intx_disable(void);                                                    /* �ر������ж� */
void sys_intx_enable(void);                                                     /* ���������ж� */
void sys_msr_msp(uint32_t addr);                                                /* ����ջ����ַ */

#endif











