/* boot_main.c - Bootloader 入口（Boot 区，独立 Keil 工程，裸机，HAL 库版）
 *
 * 工程配置（Keil）：
 *   1. 以正点原子 HAL 库例程"实验47 串口IAP实验/IAP Bootloader V1.0"为基础
 *      （sys.c/delay.c/usart.c 都是该例程自带的 HAL 版 BSP 文件，直接复用；
 *      去掉该例程自带的按键触发串口 IAP 逻辑）
 *   2. 加入本目录 boot_main.c / uds.c / boot_uds.h
 *   3. 链接地址保持默认 0x08000000（Boot 区，16KB）
 *   4. 编译后生成 Boot 固件，烧到 0x08000000
 *
 * 行为：
 *   上电 → 初始化 CAN → 等待 3 秒升级握手
 *       ├─ 收到 0x10 02 → 进入 UDS 升级模式
 *       └─ 超时无升级 → 校验 App 有效 → 跳转 App
 */
#include "sys.h"
#include "delay.h"
#include "usart.h"
#include "boot_uds.h"

#define BOOT_WAIT_MS   3000      /* 升级握手等待时间 */

/* ============ 精简 CAN 驱动（Boot 专用，500K，HAL 版，纯轮询不用中断）
   Boot 区是裸机顺序执行，没有 FreeRTOS/中断队列，轮询收帧和标准库版本
   行为一致，逻辑最简单也最好移植排查问题 ============ */
static CAN_HandleTypeDef hcan1;

/* CAN 是否初始化成功。失败时 Boot 仍然会去启动 App——
   App 的采集、屏幕、串口都不依赖 CAN 好不好，少一路总线只是"不能通过 CAN
   升级"，而卡死在这里是"整机再也起不来"。把"少一个功能"升级成"变砖"，
   是错误处理里最不该犯的错。
   （原来三处 HAL 失败都是 while(1)，串口上只留下一行 Boot 横幅，
     现场看到的就是一块什么都不做的板子。） */
static u8 s_can_ok = 0;

static u8 Boot_CAN_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    CAN_FilterTypeDef filter = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_AFIO_CLK_ENABLE();
    __HAL_RCC_CAN1_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_11;               /* CAN_RX 上拉输入 */
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = GPIO_PIN_12;               /* CAN_TX 复用推挽 */
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    hcan1.Instance = CAN1;
    hcan1.Init.TimeTriggeredMode = DISABLE;
    hcan1.Init.AutoBusOff = ENABLE;
    hcan1.Init.AutoWakeUp = DISABLE;
    hcan1.Init.AutoRetransmission = ENABLE;
    hcan1.Init.ReceiveFifoLocked = DISABLE;
    hcan1.Init.TransmitFifoPriority = DISABLE;
    hcan1.Init.Mode = CAN_MODE_NORMAL;
    hcan1.Init.SyncJumpWidth = CAN_SJW_1TQ;
    hcan1.Init.TimeSeg1 = CAN_BS1_9TQ;
    hcan1.Init.TimeSeg2 = CAN_BS2_8TQ;
    hcan1.Init.Prescaler = 4;             /* 36MHz/(1+9+8)/4 = 500Kbps，同 App 侧 */
    if (HAL_CAN_Init(&hcan1) != HAL_OK) {
        printf("CAN init failed, OTA unavailable\r\n");
        return 1;
    }

    filter.FilterIdHigh = 0x0000;
    filter.FilterIdLow = 0x0000;
    filter.FilterMaskIdHigh = 0x0000;
    filter.FilterMaskIdLow = 0x0000;
    filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
    filter.FilterBank = 0;
    filter.FilterMode = CAN_FILTERMODE_IDMASK;
    filter.FilterScale = CAN_FILTERSCALE_32BIT;
    filter.FilterActivation = ENABLE;
    filter.SlaveStartFilterBank = 14;
    if (HAL_CAN_ConfigFilter(&hcan1, &filter) != HAL_OK) {
        printf("CAN filter config failed, OTA unavailable\r\n");
        return 1;
    }

    if (HAL_CAN_Start(&hcan1) != HAL_OK) {
        printf("CAN start failed, OTA unavailable\r\n");
        return 1;
    }
    /* 不激活中断通知，main 循环里轮询 FIFO 即可 */
    s_can_ok = 1;
    return 0;
}

static void Boot_CAN_Send(u32 id, u8 *buf, u8 len)
{
    /* CAN 没初始化成功时直接返回。正常流程走不到这里（初始化失败会
       直接去启动 App），留着是防御——以后要是有人加了别的路径，
       不至于去操作一个没配置好的外设 */
    if (!s_can_ok) return;
    CAN_TxHeaderTypeDef txHeader;
    u32 mailbox;
    u16 timeout = 0;

    txHeader.StdId = id;
    txHeader.ExtId = 0;
    txHeader.IDE = CAN_ID_STD;
    txHeader.RTR = CAN_RTR_DATA;
    txHeader.DLC = len;
    txHeader.TransmitGlobalTime = DISABLE;

    if (HAL_CAN_GetTxMailboxesFreeLevel(&hcan1) == 0) return;
    if (HAL_CAN_AddTxMessage(&hcan1, &txHeader, buf, &mailbox) != HAL_OK) return;

    while (HAL_CAN_IsTxMessagePending(&hcan1, mailbox)) {
        if (++timeout > 0xFFF) { HAL_CAN_AbortTxRequest(&hcan1, mailbox); return; }
    }
}

/* 非阻塞收一帧，成功返回 1 */
static u8 Boot_CAN_Recv(u32 *id, u8 *buf, u8 *len)
{
    if (!s_can_ok) return 0;   /* 同 Boot_CAN_Send */
    CAN_RxHeaderTypeDef rxHeader;
    if (HAL_CAN_GetRxFifoFillLevel(&hcan1, CAN_RX_FIFO0) == 0) return 0;
    if (HAL_CAN_GetRxMessage(&hcan1, CAN_RX_FIFO0, &rxHeader, buf) != HAL_OK) return 0;
    *id = rxHeader.StdId;
    *len = (u8)rxHeader.DLC;
    return 1;
}

/* ============ 跳转到指定槽 ============ */
static u8 SlotLooksValid(u32 addr)
{
    u32 sp = *(volatile u32 *)addr;
    u32 pc = *(volatile u32 *)(addr + 4);
    if ((sp & 0xFFF00000) != 0x20000000) return 0;   /* 栈顶得在 RAM 范围内 */
    if ((pc & 1) == 0) return 0;                     /* Reset_Handler 必须是 Thumb */
    return 1;
}

static void JumpToSlot(u32 slot)
{
    u32 addr = SLOT_ADDR(slot);
    u32 sp, pc;
    void (*app_entry)(void);

    if (!SlotLooksValid(addr)) {
        printf("Slot %c invalid (SP/PC)\r\n", (char)('A' + slot));
        return;
    }
    sp = *(volatile u32 *)addr;
    pc = *(volatile u32 *)(addr + 4);

    printf("Jump to slot %c @0x%08X\r\n", (char)('A' + slot), (unsigned int)addr);
    __disable_irq();
    SysTick->CTRL = 0;                  /* 停 SysTick */
    SCB->VTOR = addr;                   /* 中断向量表指向该槽 */
    __set_MSP(sp);                      /* 设栈指针 */
    app_entry = (void (*)(void))pc;
    app_entry();
    while (1);
}

/* ============ A/B 启动决策 ============
 * 规则：
 *   1. 有 pending（刚升级完还没被确认的槽）→ 先试它，试之前把尝试次数 +1 写回
 *   2. 尝试次数超过 MAX_BOOT_ATTEMPTS → 判定新固件起不来，丢弃 pending 回滚
 *   3. 没有 pending → 直接启动 active
 *
 * "尝试次数先加再跳"是关键：如果新固件跑飞了、或者根本没跑到确认那一步，
 * 设备会复位重来，这时计数已经落盘了，几次之后自然触发回滚。要是等启动
 * 成功再计数，跑飞的固件永远不会把计数加上去，就永远回滚不了。
 */
static void BootSelectAndJump(void)
{
    FwMeta_t m;
    FwMeta_Read(&m);

    if (m.pending_slot < SLOT_COUNT) {
        if (m.boot_attempts >= MAX_BOOT_ATTEMPTS) {
            printf("!! slot %c failed %u times, rolling back to slot %c\r\n",
                   (char)('A' + m.pending_slot), (unsigned)m.boot_attempts,
                   (char)('A' + m.active_slot));
            m.pending_slot = SLOT_INVALID;
            m.boot_attempts = 0;
            FwMeta_Write(&m);
            /* 落回 active 槽，下面统一处理 */
        } else {
            u32 slot = m.pending_slot;
            m.boot_attempts++;
            FwMeta_Write(&m);           /* 先落盘再跳，理由见上面注释 */
            printf("Trying pending slot %c (attempt %u/%u)\r\n",
                   (char)('A' + slot), (unsigned)m.boot_attempts, MAX_BOOT_ATTEMPTS);
            JumpToSlot(slot);
            /* 能走到这里说明这个槽连向量表都不合法，直接按失败处理，
               不用等三次——它根本不可能启动起来 */
            printf("pending slot unusable, fall back\r\n");
            FwMeta_Read(&m);
            m.pending_slot = SLOT_INVALID;
            m.boot_attempts = 0;
            FwMeta_Write(&m);
        }
    }

    printf("Boot active slot %c\r\n", (char)('A' + m.active_slot));
    JumpToSlot(m.active_slot);
}

/* S3 超时：编程会话内 10s 没收到任何诊断请求就自动退出编程会话，
   避免上位机中途崩掉/断线后 Boot 卡死在解锁状态出不来（方案书里写了这条
   "v2 已加固"，但之前只留了一行"预留"注释，实际没做——现在补上） */
#define S3_TIMEOUT_MS   10000

/* ============ UDS 升级主循环 ============ */
static void uds_loop(void)
{
    u8  rx[8], tx[8], rxlen, txlen;
    u32 rxid;
    u8  reset_req = 0;
    u32 idle_ms = 0;

    printf("Enter UDS programming mode\r\n");
    while (!reset_req) {
        if (Boot_CAN_Recv(&rxid, rx, &rxlen)) {
            if (rxid == CAN_ID_UDS_REQ) {
                reset_req = UDS_HandleFrame(rx, rxlen, tx, &txlen);
                if (txlen > 0) Boot_CAN_Send(CAN_ID_UDS_RES, tx, txlen);
                idle_ms = 0;   /* 收到有效诊断请求，重新计时 */
            }
        } else {
            delay_ms(1);
            idle_ms++;
            if (UDS_IsSessionProgramming() && idle_ms >= S3_TIMEOUT_MS) {
                printf("S3 timeout, exit programming session\r\n");
                UDS_Init();     /* 清会话/安全访问/下载状态，留在 Boot 等新的 0x10 02 */
                idle_ms = 0;
            }
        }
    }
    /* 收到 0x11 复位 → 跳转 App */
    delay_ms(100);
    BootSelectAndJump();
}

/* ============ 主入口 ============ */
int main(void)
{
    /* HAL_Init() 内部已经调用 HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4)，
       不用再像标准库那样手动调 NVIC_PriorityGroupConfig */
    HAL_Init();
    sys_stm32_clock_init(RCC_PLL_MUL9);   /* 72MHz，和 App 工程时钟配置一致 */
    delay_init(72);
    usart_init(115200);
    printf("\r\n===== Bootloader v1.0 =====\r\n");

    if (Boot_CAN_Init() != 0) {
        /* CAN 起不来就没有升级这条路可走，别白等 3 秒握手，
           直接去启动 App——设备至少还能正常干活，
           串口上那行 "CAN init failed" 已经说清楚了原因 */
        printf("CAN unavailable, skip upgrade handshake\r\n");
        BootSelectAndJump();
        printf("App invalid and CAN dead, halted\r\n");
        while (1) { }
    }
    UDS_Init();

    /* 等待 3 秒升级握手 */
    u8 upgraded = 0;
    u8 rx[8], tx[8], rxlen, txlen;
    u32 rxid;
    u32 wait = BOOT_WAIT_MS;
    while (wait > 0) {
        if (Boot_CAN_Recv(&rxid, rx, &rxlen)) {
            if (rxid == CAN_ID_UDS_REQ && rx[0] == SID_DIAG_SESSION && rx[1] == 0x02) {
                /* 这一帧必须在这里就地应答：它已经被从 CAN FIFO 里取走了，
                   进 uds_loop 之后不会再出现第二次。原来这里只是 break，
                   于是上位机发出的第一个 0x10 请求永远等不到响应，只能超时
                   失败；等它重跑一次时 Boot 已经在 uds_loop 里了，那次才会
                   被正常处理——"OTA 第一次必失败、第二次才成功"就是这么来的，
                   跟复位后等多久（RESET_WAIT_MS）没有关系 */
                upgraded = 1;   /* 上位机请求进入编程会话 */
                UDS_HandleFrame(rx, rxlen, tx, &txlen);
                if (txlen > 0) Boot_CAN_Send(CAN_ID_UDS_RES, tx, txlen);
                break;
            }
        }
        delay_ms(1);
        wait--;
    }

    if (upgraded) {
        uds_loop();   /* 进入 UDS 升级模式（内部含握手帧处理） */
    } else {
        printf("No upgrade request, try jump App\r\n");
        BootSelectAndJump();
    }

    /* 跳转失败则停留等待升级 */
    printf("App invalid, stay in Boot, waiting upgrade...\r\n");
    while (1) {
        if (Boot_CAN_Recv(&rxid, rx, &rxlen)) {
            if (rxid == CAN_ID_UDS_REQ) {
                /* 同上：收到的这一帧要先应答再进主循环，直接进 uds_loop
                   等于把它丢了，上位机同样会超时 */
                u8 reset_req = UDS_HandleFrame(rx, rxlen, tx, &txlen);
                if (txlen > 0) Boot_CAN_Send(CAN_ID_UDS_RES, tx, txlen);
                if (reset_req) { delay_ms(100); BootSelectAndJump(); }
                else            uds_loop();
            }
        }
        delay_ms(1);
    }
}
