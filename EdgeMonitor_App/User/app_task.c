/************************************************************
 * app_task.c - EdgeMonitor App 区任务（阶段 1 真实传感器采集版）
 *
 * 任务模型（FreeRTOS，优先级见 App_Task_Create 里的说明）：
 *   task_sensor_collect  采集任务（栈 512 字，20ms 调度表，优先级 4 最高）
 *   task_cmd_process     命令处理（栈 256 字，优先级 3）
 *   task_can_report      CAN 周期上报（栈 256 字，优先级 2）
 *   task_watchdog        IWDG 喂狗（栈 128 字，优先级 1 最低）
 *
 * 传感器采集调度表（真实驱动，杜绝假数据）：
 *   20ms  : MPU6050 姿态（固定 dt=0.02s 互补滤波）
 *   200ms : 超声波测距 + 光敏 ADC
 *   1000ms: DHT11 温湿度（最小采样间隔 1s，必须遵守）
 *
 * 依赖：正点原子战舰板工程（delay.h/sys.h）+ sensor/ 目录驱动
 ************************************************************/
#include "app_task.h"
#include "can.h"
#include "fw_meta.h"     /* A/B 分区布局 + 元数据（与 Boot 共用同一份定义） */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* 传感器驱动 */
#include "sensor/dht11.h"
#include "sensor/hcsr04.h"
#include "sensor/adc_light.h"
#include "sensor/mpu6050.h"
#include "motor.h"

SensorData_t g_sensor;              /* 全局传感器数据 */
static QueueHandle_t s_cmd_q;       /* CAN 命令消息队列 */

/* ============ 独立看门狗 IWDG ============
 * 目的：任务跑飞/死锁时自动复位，而不是挂在那里等人去断电。板子装在
 * 巡检小车上，没人守着，"卡住不动"和"复位后继续跑"差别很大。
 *
 * 喂狗方式是签到式，不是找个地方无脑喂：两个周期任务各自打卡，喂狗
 * 任务确认两个都打过卡才喂一次并清零。这样任意一个任务卡死都会导致
 * 喂不上狗而复位；如果只在某一个任务里喂，另一个任务死了照样喂得欢，
 * 看门狗就形同虚设。
 *
 * cmd 任务不参与签到：它阻塞在 xQueueReceive(portMAX_DELAY) 上，没有
 * 命令时长时间不运行本来就是正常状态，要求它打卡只会误复位。
 *
 * !! 和 OTA 的关系（重要）：IWDG 一旦用软件启动就关不掉，只能靠系统复位。
 * 好在 App 是通过 NVIC_SystemReset() 交还给 Boot 的，系统复位会把 IWDG
 * 恢复成关闭状态（选项字节默认是软件看门狗模式 WDG_SW=1），所以 Boot 里
 * 不需要、也不应该喂狗。这一点必须守住——Boot 擦除 App 区那 248 页要
 * 5~10 秒，远超这里 4 秒的超时，万一哪天在 Boot 里也开了 IWDG，擦除会
 * 被拦腰复位，OTA 直接废掉。
 */
/* ============ A/B 升级：启动确认 ============
 * Boot 把刚升级的槽标成 pending 后就跳过来试运行，并且每试一次就把
 * boot_attempts 加一落盘。App 这边跑稳之后必须主动"确认"一次，把 pending
 * 提升为 active 并清零计数；否则 Boot 试满 MAX_BOOT_ATTEMPTS 次就会回滚。
 *
 * 确认时机选在"CAN 已经成功上报若干次之后"，而不是 main 一进来就确认——
 * 一上来就确认等于把这套机制废掉：固件哪怕启动 200ms 后就跑飞，也已经被
 * 盖章为"可用"了。要等到核心功能（采集 + CAN 上报）真的跑通才算数。
 */
static u8 s_boot_confirmed = 0;

static void App_ConfirmBootOnce(void)
{
    FwMeta_t m;
    const FwMeta_t *src = (const FwMeta_t *)META_ADDR;
    FLASH_EraseInitTypeDef erase;
    u32 page_error, addr, i;
    const u16 *p;

    if (s_boot_confirmed) return;
    s_boot_confirmed = 1;               /* 无论成败都只尝试一次，别反复擦写 Flash */

    if (!FwMeta_IsValid(src)) return;   /* 没有元数据（JTAG 直刷的场景），无需确认 */
    m = *src;
    if (m.pending_slot >= SLOT_COUNT) return;   /* 没有待确认的槽，正常启动而已 */

    /* 走到这里说明"我就是那个刚升级上来的固件，而且已经跑通了" */
    m.active_slot = m.pending_slot;
    m.pending_slot = SLOT_INVALID;
    m.boot_attempts = 0;
    m.magic = META_MAGIC;
    m.checksum = FwMeta_CalcChecksum(&m);

    HAL_FLASH_Unlock();
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = META_ADDR;
    erase.NbPages = 1;
    if (HAL_FLASHEx_Erase(&erase, &page_error) == HAL_OK) {
        p = (const u16 *)&m;
        addr = META_ADDR;
        for (i = 0; i < sizeof(FwMeta_t) / 2; i++) {
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr, p[i]) != HAL_OK) break;
            addr += 2;
        }
        printf("A/B: boot confirmed, active slot = %c\r\n", (char)('A' + m.active_slot));
    } else {
        printf("A/B: confirm failed (meta erase)\r\n");
    }
    HAL_FLASH_Lock();
}

#define ALIVE_COLLECT  0x01
#define ALIVE_REPORT   0x02
#define ALIVE_ALL      (ALIVE_COLLECT | ALIVE_REPORT)

static volatile u8 s_alive_flags = 0;
static IWDG_HandleTypeDef s_hiwdg;

static void IWDG_Start(void)
{
    /* LSI 约 40kHz，64 分频后 625Hz（1.6ms 一个 tick），
       重装载 2500 → 约 4.0 秒超时。
       正常情况下喂狗任务每 500ms 喂一次，留了 8 倍裕量，
       不会因为偶发的调度抖动误复位 */
    s_hiwdg.Instance       = IWDG;
    s_hiwdg.Init.Prescaler = IWDG_PRESCALER_64;
    s_hiwdg.Init.Reload    = 2499;
    if (HAL_IWDG_Init(&s_hiwdg) != HAL_OK) {
        printf("IWDG init fail\r\n");
    }
    /* 调试时冻结看门狗：不然单步/断点停住的几秒就会被复位，
       根本没法用调试器排查问题 */
    __HAL_DBGMCU_FREEZE_IWDG();
}

/* ============ 任务0: 喂狗（最低优先级） ============ */
static void task_watchdog(void *arg)
{
    (void)arg;
    while (1) {
        if ((s_alive_flags & ALIVE_ALL) == ALIVE_ALL) {
            s_alive_flags = 0;
            HAL_IWDG_Refresh(&s_hiwdg);
        }
        /* 故意放在最低优先级：万一有高优先级任务陷入死循环把 CPU 占满，
           这个任务就轮不上，喂不了狗，正好触发复位——这正是我们想要的 */
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ============ 执行设备（LED/蜂鸣器 战舰板固定引脚） ============ */
#define LED0_PORT GPIOB
#define LED0_PIN  GPIO_PIN_5
#define LED1_PORT GPIOE
#define LED1_PIN  GPIO_PIN_5
#define BUZ_PORT  GPIOB
#define BUZ_PIN   GPIO_PIN_8

/* 用 LED1(PE5/DS1) 而不是 LED0(PB5/DS0)：
   PB5 被征用成 TIM3 部分重映射后的左轮编码器 B 相（见 motor.c）。
   LED1 原先只在初始化里被熄灭、从未使用，正好接手——
   Web 上的"LED 控制"功能不变，只是亮的灯从 DS0 换成 DS1。 */
void HW_Led_Set(u8 on)        { HAL_GPIO_WritePin(LED1_PORT, LED1_PIN, on ? GPIO_PIN_RESET : GPIO_PIN_SET); }
void HW_Buzzer_Set(u8 on)     { HAL_GPIO_WritePin(BUZ_PORT, BUZ_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET); }

/* 小车执行：转成闭环目标转速，具体驱动在 motor.c。
 *
 * 协议里的 p1 是 0~100 的"速度"，这里把它映射成目标 RPM而不是
 * 直接当 PWM 占空比。两者的区别在于：占空比相同时，空载和爬坡的实际
 * 转速差很远，直线走着走着就偏了；而以转速为目标，上坡时 PI 会自动
 * 把占空比顶上去。对已有的 CAN 协议完全兼容，p1 的含义从"占空比"
 * 变成"目标速度百分比"，上层无感。 */
void HW_Motor_SetSpeed(u8 pwm_percent, u8 dir)
{
    s16 rpm;
    if (pwm_percent > 100) pwm_percent = 100;
    rpm = (s16)((s32)MOTOR_MAX_RPM * pwm_percent / 100);
    if (dir == 2) rpm = (s16)-rpm;        /* 2 = 后退 */
    else if (dir == 0) rpm = 0;           /* 0 = 停 */
    Motor_SetTargetRpm(rpm, rpm);
}

/* 差速转向：左转 = 左轮退、右轮进（原地转）。
 * ms=0 时不自动停——这是故意的：界面上长按转向键属于这种情况，
 * 松手时会发 ACT_OFF。但远程下发建议总是带 ms，万一链路断了，
 * 车不会一直转下去。 */
/* 坦克式差速。参数已经是去过偏置的 -100~+100。 */
void HW_Motor_SetTank(s8 left, s8 right)
{
    if (left  >  100) left  =  100;
    if (left  < -100) left  = -100;
    if (right >  100) right =  100;
    if (right < -100) right = -100;
    Motor_SetTank(left, right);
}

void HW_Motor_SetSteer(u8 direction, u8 pwm, u16 ms)
{
    s16 rpm;
    if (pwm > 100) pwm = 100;
    rpm = (s16)((s32)MOTOR_MAX_RPM * pwm / 100);
    if (direction == ACT_LEFT)  Motor_Turn((s16)-rpm, rpm, ms);
    else                        Motor_Turn(rpm, (s16)-rpm, ms);
}

/* ============ 命令结构 ============ */
/* id 用来在任务里区分这条队列消息是"0x110 设备控制命令"还是
   "0x130 诊断请求"，避免像旧版那样让两种协议挤在同一个 CAN ID 上 */
typedef struct {
    u32 id;
    u8 dev;   /* 目标设备（控制命令用） */
    u8 act;   /* 动作（控制命令用） */
    u8 p1;    /* 参数1（控制命令用） */
    u8 p2;    /* 参数2（控制命令用） */
    u8 raw[8];/* 原始数据（诊断请求用） */
    u8 len;   /* 原始数据长度（诊断请求用） */
} Cmd_t;

/* ============ CAN 接收回调（在中断中,只入队,处理放任务里） ============ */
static void OnCanRx(u32 id, u8 *buf, u8 len)
{
    /* 必须清零。两个分支各自只填自己那几个字段，没填的会带着中断栈上的
       残留数据进队列。当前的消费端按 cmd.id 分流、只读对应字段，所以还没
       出过问题——但这是"恰好没踩到"，不是"不会踩到"：以后谁在 DIAG 分支里
       多读一个 cmd.p1，拿到的就是随机值，而且这种 bug 会随中断发生时机
       随机复现，极难定位 */
    Cmd_t cmd = {0};
    BaseType_t higher = pdFALSE;
    u8 i;

    if (id == CAN_ID_CTRL_CMD && len >= 3) {
        cmd.id  = CAN_ID_CTRL_CMD;
        cmd.dev = buf[0];
        cmd.act = buf[1];
        cmd.p1  = buf[2];
        cmd.p2  = (len >= 4) ? buf[3] : 0;
    } else if (id == CAN_ID_DIAG_REQ && len >= 1) {
        cmd.id  = CAN_ID_DIAG_REQ;
        cmd.len = len;
        for (i = 0; i < len && i < 8; i++) cmd.raw[i] = buf[i];
    } else {
        return;
    }

    /* 中断中入队,带 FromISR */
    xQueueSendFromISR(s_cmd_q, &cmd, &higher);
    portYIELD_FROM_ISR(higher);
}

/* ============ 任务1: 传感器采集（20ms 调度表） ============ */
/* 连续失败多少次才判定为故障。
 * 不用"一次失败就报故障"：单总线/软件 I2C/超声波回波这些都有偶发失败，
 * 每次抖动都弹一次故障，告警很快就没人看了。乘以各自的采样周期，
 * 四个传感器的判定延迟都在 0.5~3 秒量级，对巡检场景足够快。 */
#define FAIL_LIMIT_TH     3      /* × 1000ms = 3s */
#define FAIL_LIMIT_DIST   5      /* × 200ms  = 1s */
#define FAIL_LIMIT_LIGHT  5      /* × 200ms  = 1s */
#define FAIL_LIMIT_IMU    25     /* × 20ms   = 0.5s */

/* 把"连续失败计数"翻译成健康位。成功即清零并立刻恢复，失败要连续够次数
 * 才置位——恢复比报故障更积极，避免故障位粘住不放 */
static void health_update(u8 ok, u8 *fail_cnt, u8 limit, u8 bit)
{
    if (ok) {
        *fail_cnt = 0;
        g_sensor.sensor_health &= (u8)~bit;
        return;
    }
    if (*fail_cnt < 255) (*fail_cnt)++;
    if (*fail_cnt >= limit) g_sensor.sensor_health |= bit;
}

void task_sensor_collect(void *arg)
{
    u32 tick = 0;
    u8 f_th = 0, f_dist = 0, f_light = 0, f_imu = 0;   /* 各传感器连续失败计数 */

    /* 初始化真实驱动。任何一个初始化失败都不阻断其余传感器——巡检设备
       少一路数据还能继续干活，整机停摆就什么都干不了了 */
    DHT11_Init();
    if (HC_SR04_Init() != 0) {
        /* 同下面的 ADC：初始化就失败的直接置故障位，不用等运行时累计失败次数 */
        g_sensor.sensor_health |= SENS_FAULT_DIST;
        printf("HC-SR04 init fail\r\n");
    }
    if (ADC_Light_Init() != 0) {
        /* 初始化就失败的，直接把故障位置上，不用等运行时累计失败次数 */
        g_sensor.sensor_health |= SENS_FAULT_LIGHT;
    }
    if (MPU6050_Init() != 0) {
        printf("MPU6050 init fail\r\n");
        g_sensor.sensor_health |= SENS_FAULT_IMU;
    }
    if (Motor_Init() != 0) {
        /* 电机配不起来不阻断采集：巡检设备少一路能力还能当固定传感器用，
           整机停摆就什么都没了。Motor_* 内部有 s_ready 守门，后续调用全部空转。 */
        g_sensor.sensor_health |= SENS_FAULT_MOTOR;
    }

    while (1) {
        /* --- 20ms 周期：IMU 姿态（固定 dt） --- */
        {
            u8 ok = (MPU6050_GetAngle(&g_sensor.pitch_x100,
                                      &g_sensor.roll_x100,
                                      &g_sensor.yaw_x100) == 0);
            health_update(ok, &f_imu, FAIL_LIMIT_IMU, SENS_FAULT_IMU);
        }

        /* --- 200ms 周期：超声波 + 光敏 --- */
        if (tick % 10 == 0) {
            u16 d = HC_SR04_GetDistance();
            u8  lp;
            /* 超声波超时返回 0。0cm 在物理上不可能（模块本身最小量程 2cm），
               所以 0 一律当读取失败，而不是当成"贴着障碍物" */
            if (d > 0) g_sensor.distance_cm = d;
            health_update(d > 0, &f_dist, FAIL_LIMIT_DIST, SENS_FAULT_DIST);

            if (ADC_Light_GetPercent(&lp) == 0) {
                g_sensor.light_percent = lp;
                health_update(1, &f_light, FAIL_LIMIT_LIGHT, SENS_FAULT_LIGHT);
            } else {
                health_update(0, &f_light, FAIL_LIMIT_LIGHT, SENS_FAULT_LIGHT);
            }
        }

        /* --- 1000ms 周期：DHT11 温湿度（最小间隔 1s） --- */
        if (tick % 50 == 0) {
            s16 t, h;
            u8 rc = DHT11_Read_Data(&t, &h);
            if (rc == 0) {
                g_sensor.temperature_x10 = t;
                g_sensor.humidity_x10    = h;
            } else {
                /* 失败则保留上次值，同时打印具体原因——板子脱离调试器独立跑
                   起来之后，除了串口打印看不到别的诊断信息。
                   注意保留旧值这件事本身是合理的（免得数值来回跳），
                   但必须同时把故障位置上，否则上层无从分辨"稳定"和"卡死" */
                printf("DHT11 read fail: %s\r\n",
                       (rc == 1) ? "no response (check wiring/power)" : "checksum error");
            }
            health_update(rc == 0, &f_th, FAIL_LIMIT_TH, SENS_FAULT_TH);

            /* 电池电压跟着 DHT11 一起每秒采一次。电池电压变化是分钟级的，
               采得再快也没意义，反而占着 ADC。 */
            {
                u16 mv;
                u8  brc = ADC_Battery_GetMv(&mv);
                if (brc == 0)      g_sensor.batt_mv = mv;
                else if (brc == 2) g_sensor.batt_mv = 0;   /* 未接线 */
                /* brc==1（读取失败）保留上次值：偶发的一次 ADC 超时不应该
                   让大屏上的电压闪一下。光敏那路已经有健康位盖着 ADC 整体故障。 */
            }
        }

        /* --- 20ms 周期：电机闭环 --- */
        /* 放在循环末尾、与 vTaskDelay(20) 配对，Motor_Tick 内部的 TICK_MS 必须和这里
           一致——PI 的积分项和"计数 -> RPM"的换算都挂在这个固定周期上，
           两边对不上的话转速会整体差一个固定倍数，而且看起来完全合理。 */
        Motor_Tick();
        Motor_GetState(&g_sensor.rpm_left, &g_sensor.rpm_right,
                       &g_sensor.odom_left_cm, &g_sensor.odom_right_cm);
        Motor_GetDisp(&g_sensor.disp_left_mm, &g_sensor.disp_right_mm);
        /* 飞车保护触发后把故障位顶上去。不上报的话，大屏上只会看到
           "转速 0"——和正常停车完全一样，没人会知道刚才失控过。 */
        if (Motor_IsFaulted()) g_sensor.sensor_health |= SENS_FAULT_MOTOR;

        tick++;
        s_alive_flags |= ALIVE_COLLECT;   /* 看门狗签到 */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ============ 任务2: CAN 周期上报 ============ */
void task_can_report(void *arg)
{
    u8 buf[8];
    u16 rep_period = 200;  /* 默认 200ms，可由 0x111 配置 */
    u32 report_cnt = 0;    /* 上报次数，用于 A/B 启动确认的时机判断 */

    while (1) {
        /* 0x100 状态上报（温度/光照/距离/状态字/湿度）
           温度原来只占 buf[0] 一个字节——正常室温（比如22.8℃→temperature_x10=228）
           远超一个字节能装的范围，网关端 (int8_t)buf[0] 一转换，任何超过 12.7℃
           的正常读数都会变成负数，是个必现的协议设计缺陷，不是偶发 bug。buf[7]
           原来完全没用（保留恒为0），刚好腾出来把温度扩成两字节、大端，跟湿度
           的编码方式一致。网关端 gateway_mqtt.c 要同步改，不然会跟这边错位。 */
        buf[0] = (u8)((g_sensor.temperature_x10 >> 8) & 0xFF);
        buf[1] = g_sensor.light_percent;
        buf[2] = (u8)(g_sensor.distance_cm >> 8);
        buf[3] = (u8)(g_sensor.distance_cm & 0xFF);
        /* bit2（电机在转）现算：拿编码器的实测结果，而不是用最后一条命令推导。
           在本地变量上合成、不回写 g_sensor.sys_state，顺带避开了两个任务
           对同一字节做读-改-写的竞态（同 sensor_health 那段注释的道理）。 */
        {
            u8 st = (u8)(g_sensor.sys_state & (u8)~0x04);
            if (Motor_IsRunning()) st |= 0x04;
            buf[4] = st;
        }
        buf[5] = (u8)((g_sensor.humidity_x10 >> 8) & 0xFF);  /* 湿度大端（同 IMU 帧） */
        buf[6] = (u8)(g_sensor.humidity_x10 & 0xFF);
        buf[7] = (u8)(g_sensor.temperature_x10 & 0xFF);
        CAN1_SendStd(CAN_ID_STATUS_REPORT, buf, 8);

        /* 0x101 IMU 姿态 */
        buf[0] = (u8)(g_sensor.pitch_x100 >> 8);
        buf[1] = (u8)(g_sensor.pitch_x100 & 0xFF);
        buf[2] = (u8)(g_sensor.roll_x100 >> 8);
        buf[3] = (u8)(g_sensor.roll_x100 & 0xFF);
        buf[4] = (u8)(g_sensor.yaw_x100 >> 8);
        buf[5] = (u8)(g_sensor.yaw_x100 & 0xFF);
        /* buf[6] = 传感器健康位（SENS_FAULT_*）。放这一帧而不是 0x100 状态帧，
           纯粹是因为 0x100 的 8 个字节已经排满了（温度扩成两字节时用掉了
           最后一个保留字节），而这一帧的 buf[6]/buf[7] 一直空着。
           网关侧 gateway_mqtt.c 要同步解析，否则这个字节等于白发。 */
        buf[6] = g_sensor.sensor_health;
        /* buf[7] = 电池电压，0.1V 为单位（126 = 12.6V）。
           一个字节能表达 0~25.5V，而模块输入范围是 4.5~15V，够用。
           **0 = 未接线**，不是 0V。放在这一帧是因为 0x100 的 8 个字节已经排满，
           而这里的 buf[7] 一直空着。网关侧 gateway_mqtt.c 要同步解析。 */
        buf[7] = (g_sensor.batt_mv / 100 > 255) ? 255 : (u8)(g_sensor.batt_mv / 100);
        CAN1_SendStd(CAN_ID_IMU_FRAME, buf, 8);

        /* 0x103 左右轮有符号位移（各 int32，单位 mm）。
           0x102 那 8 个字节已经排满了，塞不下，所以单开一帧。
           为什么值得单开：上位机原来拿 5Hz 的转速去积分算位移，
           两次采样之间的加减速完全看不见；而这里每个编码器计数都不会漏。

           **必须排在 0x102 前面**：网关收到 0x103 只是缓存起来，
           等 0x102 到了才把两者合并成一条 MQTT 消息发出去。
           顺序反了的话，每条消息里的位移都比转速旧一帧（200ms），
           车速快时就是十几厘米的错位。 */
        buf[0] = (u8)(g_sensor.disp_left_mm  >> 24);
        buf[1] = (u8)(g_sensor.disp_left_mm  >> 16);
        buf[2] = (u8)(g_sensor.disp_left_mm  >> 8);
        buf[3] = (u8)(g_sensor.disp_left_mm       );
        buf[4] = (u8)(g_sensor.disp_right_mm >> 24);
        buf[5] = (u8)(g_sensor.disp_right_mm >> 16);
        buf[6] = (u8)(g_sensor.disp_right_mm >> 8);
        buf[7] = (u8)(g_sensor.disp_right_mm      );
        CAN1_SendStd(CAN_ID_MOTOR_DISP, buf, 8);

        /* 0x102 小车运动状态 */
        buf[0] = (u8)(g_sensor.rpm_left >> 8);
        buf[1] = (u8)(g_sensor.rpm_left & 0xFF);
        buf[2] = (u8)(g_sensor.rpm_right >> 8);
        buf[3] = (u8)(g_sensor.rpm_right & 0xFF);
        buf[4] = (u8)(g_sensor.odom_left_cm >> 8);
        buf[5] = (u8)(g_sensor.odom_left_cm & 0xFF);
        buf[6] = (u8)(g_sensor.odom_right_cm >> 8);
        buf[7] = (u8)(g_sensor.odom_right_cm & 0xFF);
        CAN1_SendStd(CAN_ID_MOTOR_STATE, buf, 8);

        s_alive_flags |= ALIVE_REPORT;   /* 看门狗签到 */

        /* A/B 启动确认：连续上报 25 次（约 5 秒）说明采集和 CAN 都通了，
           这时才敢说这个固件"活下来了"。见 App_ConfirmBootOnce 的注释 */
        if (++report_cnt == 25) App_ConfirmBootOnce();

#ifdef WDG_SELFTEST
        /* 看门狗自测：跑够 5 秒后故意把本任务卡死，验证 IWDG 真的能把系统
           拉回来——"没复位"和"看门狗没装上"在正常运行时看起来一模一样，
           不主动测一次就不知道装的是不是个摆设。
           打开方式：Keil → Options for Target → C/C++ → Define 里加
           WDG_SELFTEST，测完删掉这个宏即可，不用动代码。
           预期：打印下面这行后约 4 秒自动复位，重启时打印
                 "!! last reset: IWDG watchdog (a task hung over 4s)" */
        {
            static u32 selftest_cnt = 0;
            if (++selftest_cnt > 25) {          /* 200ms × 25 = 5s */
                printf("WDG selftest: hanging report task on purpose...\r\n");
                while (1) { }                   /* 不再签到，喂狗任务会停止喂狗 */
            }
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(rep_period));
    }
}

/* ============ 0x130 诊断请求处理（升级前状态探测用，仅 0x22/0x11） ============ *
 * 网关探测流程：先发 0x22 读 DID_APP_VERSION，收到响应说明当前跑的是 App；
 * 再发 0x11 请求复位，App 应答后主动软复位，交还给 Boot 区做 3 秒升级握手。
 * 不实现 0x10/0x27/0x34/0x36 等——那些只在 Boot 区处理，App 侧无需重复一套
 * UDS 状态机，保持这个诊断桩尽量小。 */
static void handle_diag_frame(u8 *raw, u8 len)
{
    u8 resp[8], rlen = 0;

    if (len < 1) return;

    if (raw[0] == DIAG_SID_READ_DID) {
        u16 did = (len >= 3) ? (((u16)raw[1] << 8) | raw[2]) : 0;
        if (did == DIAG_DID_APP_VERSION) {
            resp[0] = DIAG_SID_READ_DID | DIAG_POS_RESP_OFFSET;
            resp[1] = (u8)(did >> 8);
            resp[2] = (u8)did;
            resp[3] = 0x01;    /* App 版本号 1.0，与 Boot 侧保持同一编号规则 */
            resp[4] = 0x00;
            rlen = 5;
        } else {
            resp[0] = DIAG_NEG_RESP;
            resp[1] = DIAG_SID_READ_DID;
            resp[2] = DIAG_NRC_OUT_OF_RANGE;
            rlen = 3;
        }
        CAN1_SendStd(CAN_ID_DIAG_RES, resp, rlen);
    } else if (raw[0] == DIAG_SID_ECU_RESET) {
        resp[0] = DIAG_SID_ECU_RESET | DIAG_POS_RESP_OFFSET;
        rlen = 1;
        CAN1_SendStd(CAN_ID_DIAG_RES, resp, rlen);
        vTaskDelay(pdMS_TO_TICKS(20));   /* 留时间把响应帧真正发出去 */
        NVIC_SystemReset();              /* 软复位，交还 Boot 区做升级握手 */
    }
    /* 其它 SID：App 侧不支持，直接忽略（Boot 侧才处理完整 UDS 流程） */
}

/* ============ 任务3: 控制命令处理 ============ */
void task_cmd_process(void *arg)
{
    Cmd_t cmd;

    while (1) {
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) == pdTRUE) {
            if (cmd.id == CAN_ID_DIAG_REQ) {
                handle_diag_frame(cmd.raw, cmd.len);
                continue;
            }
            switch (cmd.dev) {
            case DEV_LED:
                HW_Led_Set(cmd.act == ACT_ON);
                if (cmd.act == ACT_ON) g_sensor.sys_state |= 0x01;
                else                   g_sensor.sys_state &= ~0x01;
                break;
            case DEV_BUZZER:
                HW_Buzzer_Set(cmd.act == ACT_ON);
                if (cmd.act == ACT_ON) g_sensor.sys_state |= 0x02;
                else                   g_sensor.sys_state &= ~0x02;
                break;
            case DEV_MOTOR:
                switch (cmd.act) {
                case ACT_ON:   HW_Motor_SetSpeed(cmd.p1, 1); break; /* 前进 */
                case ACT_BACK: HW_Motor_SetSpeed(cmd.p1, 2); break; /* 后退 */
                case ACT_OFF:  HW_Motor_SetSpeed(0, 0);      break; /* 停止 */
                case ACT_LEFT:  HW_Motor_SetSteer(ACT_LEFT, cmd.p1, cmd.p2); break;
                case ACT_RIGHT: HW_Motor_SetSteer(ACT_RIGHT, cmd.p1, cmd.p2); break;
                case ACT_SPEED: HW_Motor_SetSpeed(cmd.p1, 1); break;
                case ACT_ESTOP:
                    Motor_EmergencyStop();
                    g_sensor.sys_state |= 0x08;      /* bit3 = 急停 */
                    break;
                case ACT_RESUME:
                    Motor_Resume();
                    g_sensor.sys_state &= (u8)~0x08;
                    break;
                case ACT_TANK:
                    /* p1/p2 是 0~200 的偏移编码，减掉 100 才是真实百分比。
                       直接当有符号读的话，倒车（负值）会变成 150+ 的大正数，
                       车会以满速往前冲——而不会报任何错。 */
                    HW_Motor_SetTank((s8)((s16)cmd.p1 - ACT_TANK_BIAS),
                                     (s8)((s16)cmd.p2 - ACT_TANK_BIAS));
                    break;
                }
                /* bit2（电机在转）不在这里维护了，改由上报时现算，见 task_can_report。
                   理由：用"最后一条命令"推导电机状态，在三种情况下会说谎——
                     ① 定时转向（ms 到期）自己停了，没有命令来清位；
                     ② 急停拉低 STBY 后轮子不转了，但命令层看不到；
                     ③ 命令发了但电机堆死/线掉了，实际根本没转。
                   有了编码器之后就不必再猜：直接拿实测转速说话。
                   顺带避开了两个任务对 sys_state 同一字节做读-改-写的竞态。 */
                break;
            default:
                break;
            }
        }
    }
}

/* ============ 任务创建 ============ */
void App_Task_Create(void)
{
    GPIO_InitTypeDef g = {0};
    s_cmd_q = xQueueCreate(16, sizeof(Cmd_t));
    CAN1_SetRxCallback(OnCanRx);

    /* LED1=PE5（受控的那颗）, 蜂鸣器=PB8。
       PB5(DS0) 不再配置：它已归 motor.c 的左轮编码器 B 相（TIM3 部分重映射）。
       若还把它配成推挻输出，就会和编码器的输出直接对驱（一个推一个拉），
       轻则读数全错，重则烧引脚。 */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    g.Pin = GPIO_PIN_8;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &g);
    g.Pin = GPIO_PIN_5;
    HAL_GPIO_Init(GPIOE, &g);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_5, GPIO_PIN_SET);    /* LED1 熄灭（低电平亮） */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_RESET);  /* 蜂鸣器关闭 */

    /* 复位原因要在启动时就打出来：看门狗复位是"悄无声息"的，板子会自己
       重启接着跑，串口上如果不说一声，现场只会看到"数据断了一下又好了"，
       根本意识不到刚才发生过一次跑飞 */
    /* printf 里一律用英文：ARMCC V5 默认按 GBK 读源文件，而这些源码是 UTF-8，
       中文字符串字面量会被编成乱码进固件（编译期报 #870-D，串口打出来也是
       乱码）。注释里的中文不影响编译，字符串里的会 */
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST)) {
        printf("!! last reset: IWDG watchdog (a task hung over 4s)\r\n");
    }
    __HAL_RCC_CLEAR_RESET_FLAGS();

    /* 优先级安排（数字越大越高）：
     *   collect 4  采集任务必须最高。DHT11 是微秒级时序的单总线协议，读到
     *              一半被抢占，脉宽就会测出离谱值导致校验和失败——看门狗
     *              自测时 report 任务卡死不让出 CPU，同优先级下时间片轮转
     *              切进来，实测脉宽从正常的 11~34 变成 11~201，DHT11 必错。
     *              提到最高之后，读取期间不会再被其它任务打断。
     *              （曾试过 taskENTER_CRITICAL() 保护，会导致后续代码完全
     *               不执行，根因没查清，改用优先级这条更稳妥的路。）
     *   cmd     3  控制命令要及时响应，但可以让位于采集，最多晚 20ms
     *   report  2  周期上报，晚一点没关系
     *   wdg     1  最低，见上面 IWDG 注释：高优先级任务跑飞时它轮不上，
     *              喂不了狗正好触发复位
     */
    xTaskCreate(task_sensor_collect, "collect", 512, 0, 4, 0);  /* 栈 512，浮点姿态解算 */
    xTaskCreate(task_cmd_process,    "cmd",     256, 0, 3, 0);
    xTaskCreate(task_can_report,     "report",  256, 0, 2, 0);
    xTaskCreate(task_watchdog,       "wdg",     128, 0, 1, 0);

    /* 放在建完任务之后启动：从这一刻起 4 秒内必须有人喂狗，
       而喂狗任务要等调度器跑起来才会执行 */
    IWDG_Start();
}
