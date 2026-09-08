/* can.c - CAN1 驱动，HAL 版（战舰板 PA11=RX / PA12=TX，板载 TJA1050）
 * 逻辑和引脚/波特率/优先级都和标准库版本一致，只是把 StdPeriph 的
 * CAN_InitTypeDef/CAN_FilterInitTypeDef 换成了 HAL 的 CAN_HandleTypeDef。 */
#include "can.h"

static CAN_HandleTypeDef hcan1;
static CAN_RxCallback s_rx_cb = 0;   /* 接收回调 */

/**
 * @brief CAN1 初始化，波特率 500Kbps
 * 时钟：APB1 = 36MHz，Prescaler=4, BS1=9, BS2=8 -> 500K（和标准库版本算法一致）
 */
void CAN1_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    CAN_FilterTypeDef filter = {0};

    /* ---- 时钟 ---- */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_AFIO_CLK_ENABLE();
    __HAL_RCC_CAN1_CLK_ENABLE();

    /* ---- 引脚：CAN_RX=PA11 上拉输入，CAN_TX=PA12 复用推挽输出（默认映射，不remap） ---- */
    gpio.Pin = GPIO_PIN_11;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = GPIO_PIN_12;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* ---- CAN 参数：与标准库版本完全一致的位时序 ---- */
    hcan1.Instance = CAN1;
    hcan1.Init.TimeTriggeredMode = DISABLE;
    hcan1.Init.AutoBusOff = ENABLE;      /* 对应 StdPeriph 的 CAN_ABOM：总线自动恢复 */
    hcan1.Init.AutoWakeUp = DISABLE;
    hcan1.Init.AutoRetransmission = ENABLE; /* 对应 CAN_NART=DISABLE（DISABLE NART=允许自动重发） */
    hcan1.Init.ReceiveFifoLocked = DISABLE;
    hcan1.Init.TransmitFifoPriority = DISABLE;
    hcan1.Init.Mode = CAN_MODE_NORMAL;
    hcan1.Init.SyncJumpWidth = CAN_SJW_1TQ;
    hcan1.Init.TimeSeg1 = CAN_BS1_9TQ;
    hcan1.Init.TimeSeg2 = CAN_BS2_8TQ;
    hcan1.Init.Prescaler = 4;            /* 36MHz/(1+9+8)/4 = 500Kbps */
    if (HAL_CAN_Init(&hcan1) != HAL_OK) {
        while (1);   /* 初始化失败，配置有问题，停在这里方便调试定位 */
    }

    /* ---- 滤波器：接收所有标准帧（0号滤波器，掩码全 0） ---- */
    filter.FilterIdHigh = 0x0000;
    filter.FilterIdLow = 0x0000;
    filter.FilterMaskIdHigh = 0x0000;
    filter.FilterMaskIdLow = 0x0000;
    filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
    filter.FilterBank = 0;
    filter.FilterMode = CAN_FILTERMODE_IDMASK;
    filter.FilterScale = CAN_FILTERSCALE_32BIT;
    filter.FilterActivation = ENABLE;
    filter.SlaveStartFilterBank = 14;    /* 单 CAN 型号用不到，HAL 仍要求填一个合法值 */
    if (HAL_CAN_ConfigFilter(&hcan1, &filter) != HAL_OK) {
        while (1);
    }

    if (HAL_CAN_Start(&hcan1) != HAL_OK) {
        while (1);
    }

    /* ---- 接收中断：FIFO0 有新报文 ---- */
    /* 重要：中断回调里调用 xQueueSendFromISR，抢占优先级数字必须 >=
       FreeRTOSConfig.h 里的 configMAX_SYSCALL_INTERRUPT_PRIORITY（本项目取 5）。
       此处设 5，若你的 FreeRTOSConfig.h 该宏不同，请同步修改，两边必须一致。 */
    HAL_NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);
    HAL_CAN_ActivateNotification(&hcan1, CAN_IT_RX_FIFO0_MSG_PENDING);
}

/**
 * @brief 发送一帧标准数据帧
 * @param id  11 位标准 ID
 * @param buf 数据指针（<=8 字节）
 * @param len 数据长度 1~8
 * @retval 1 成功, 0 失败（邮箱满或超时）
 *
 * 说明：HAL_CAN_AddTxMessage 把帧放进发送邮箱就返回，不等真正发出去；
 * 这里手动轮询 HAL_CAN_IsTxMessagePending 直到该邮箱清空，带超时保护，
 * 行为和标准库版本（轮询 CAN_TransmitStatus）等价。
 */
u8 CAN1_SendStd(u32 id, u8 *buf, u8 len)
{
    CAN_TxHeaderTypeDef txHeader;
    u32 mailbox;
    u16 timeout = 0;

    if (len > 8) len = 8;
    if (len == 0) return 0;

    txHeader.StdId = id;
    txHeader.ExtId = 0;
    txHeader.IDE = CAN_ID_STD;
    txHeader.RTR = CAN_RTR_DATA;
    txHeader.DLC = len;
    txHeader.TransmitGlobalTime = DISABLE;

    if (HAL_CAN_GetTxMailboxesFreeLevel(&hcan1) == 0) {
        return 0;   /* 三个邮箱都满 */
    }
    if (HAL_CAN_AddTxMessage(&hcan1, &txHeader, buf, &mailbox) != HAL_OK) {
        return 0;
    }

    /* 等待该邮箱真正发出，带超时保护。
     *
     * 这里用的是【裸循环计数】而不是时间——hcsr04.c 里明确避免过这种写法
     * （超时时长跟主频/优化等级绑死，换个配置就漂）。这里为什么还这么写，
     * 以及为什么【不要】随手改成 HAL_GetTick，说明如下：
     *
     * 1) HAL 的 tick 在本工程里【是走的】。乍看 stm32f1xx_it.c 会得出相反
     *    结论——那里的 SysTick_Handler 被 #if (!SYS_SUPPORT_OS) 编译掉了。
     *    真正生效的是 Drivers/SYSTEM/delay/delay.c 里的那一份，它同时调
     *    HAL_IncTick() 和 xPortSysTickHandler()。
     *    （只看 it.c 会以为 HAL_GetTick 是死的，从而不敢用——记在这里，
     *      免得下一个人重新推一遍这个结论。）
     *
     * 2) 但 HAL_GetTick 的分辨率是 1ms，而一帧 8 字节 CAN 报文在 500kbps
     *    下只要约 0.22ms。要让基于它的超时有意义至少得给 2~3ms，
     *    比现在这个（约 0.8ms）还长，对发送路径反而更不利。
     *
     * 3) 风险不对等：这段代码跑在周期上报任务里。万一超时判据失效变成死循环，
     *    结果是看门狗复位 -> 重启 -> 再次死循环，而此时 A/B 的 pending 早已
     *    确认过，【不会回滚】——就是一个无限重启的砖。
     *    换取的好处只是"理论上更可移植"，不值这个代价。
     *
     * 真要改的话：用一个自由运行的硬件定时器计数值（像 hcsr04.c 那样），
     * 而不是 HAL_GetTick。 */
    while (HAL_CAN_IsTxMessagePending(&hcan1, mailbox)) {
        if (++timeout > 0xFFF) {
            HAL_CAN_AbortTxRequest(&hcan1, mailbox);   /* 超时，取消该邮箱 */
            return 0;
        }
    }
    return 1;
}

/**
 * @brief 注册接收回调
 */
void CAN1_SetRxCallback(CAN_RxCallback cb)
{
    s_rx_cb = cb;
}

/**
 * @brief 读取 FIFO0 中待处理的帧数
 */
u8 CAN1_GetRxCount(void)
{
    return (u8)HAL_CAN_GetRxFifoFillLevel(&hcan1, CAN_RX_FIFO0);
}

/* ============ HAL CAN 接收回调：FIFO0 有新报文 ============ */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    CAN_RxHeaderTypeDef rxHeader;
    u8 data[8];

    if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &rxHeader, data) != HAL_OK) {
        return;
    }
    if (s_rx_cb) {
        s_rx_cb(rxHeader.StdId, data, (u8)rxHeader.DLC);
    }
}

/* ============ CAN1 RX0 中断服务函数：转交给 HAL 统一分发 ============ */
void USB_LP_CAN1_RX0_IRQHandler(void)
{
    HAL_CAN_IRQHandler(&hcan1);
}
