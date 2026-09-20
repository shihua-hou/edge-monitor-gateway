#ifndef __CAN_H
#define __CAN_H

#include "stm32f1xx_hal.h"
#include "types.h"

/* ============ CAN 帧 ID 定义（与 docs/CAN通信协议.md 一致） ============ */
#define CAN_ID_STATUS_REPORT   0x100   /* STM32->iMX6ULL 状态上报 */
#define CAN_ID_IMU_FRAME       0x101   /* STM32->iMX6ULL IMU姿态 */
#define CAN_ID_MOTOR_STATE     0x102   /* STM32->iMX6ULL 小车运动状态 */
#define CAN_ID_MOTOR_DISP      0x103   /* STM32->iMX6ULL 左右轮**有符号位移** */
#define CAN_ID_CTRL_CMD        0x110   /* iMX6ULL->STM32 控制命令 */
#define CAN_ID_PARAM_SET       0x111   /* iMX6ULL->STM32 参数配置 */
/* v2.1: App 侧诊断桩用独立 ID，不与 0x110 控制命令共用同一 ID 靠首字节取值区分
   （旧设计里 Boot 和 App 都要在 0x110 上响应 0x10/0x22，语义脆弱），
   Boot 区仍固定用 0x110/0x120（见 bootloader/boot_uds.h），互不影响 */
#define CAN_ID_DIAG_REQ         0x130  /* iMX6ULL->STM32 App 诊断请求(升级前状态探测,仅 0x22/0x11) */
#define CAN_ID_DIAG_RES         0x120  /* STM32->iMX6ULL 诊断响应(与 Boot 的 UDS 响应 ID 一致，网关统一解析) */

/* ============ 0x130 诊断请求支持的最小 UDS 子集 ============ */
#define DIAG_SID_READ_DID       0x22   /* 读 DID，用于探测"当前跑的是 App"及其版本 */
#define DIAG_SID_ECU_RESET      0x11   /* 复位到 Boot，配合升级流程 */
#define DIAG_POS_RESP_OFFSET    0x40
#define DIAG_NEG_RESP           0x7F
#define DIAG_NRC_OUT_OF_RANGE   0x31
#define DIAG_DID_APP_VERSION    0xF000 /* 与 Boot 侧 boot_uds.h 的 DID 编号保持一致 */

/* ============ 0x110 控制命令 - 目标设备 / 动作枚举 ============ */
#define DEV_LED         0x01
#define DEV_BUZZER      0x02
#define DEV_MOTOR       0x03

#define ACT_ON          0x01   /* 开/前进 */
#define ACT_OFF         0x02   /* 关/停止 */
#define ACT_BACK        0x03   /* 后退 */
#define ACT_LEFT        0x04   /* 左转 */
#define ACT_RIGHT       0x05   /* 右转 */
#define ACT_SPEED       0x06   /* 调速 */
/* 急停：直接拉低 TB6612 的 STBY，输出进高阻。
   不走 PWM=0 这条路——PWM=0 依赖定时器和固件都是好的，而急停恰恰要在
   "固件可能不好"的时候生效。STBY 是纯硬件通路。 */
/* 坦克式差速：p1=左轮 p2=右轮，取值 0~200 映射到 -100~+100 %。
   为什么需要它：路径跟踪（纯追踪）要求每一拍都能连续调整左右轮速差，
   而 ACT_ON/LEFT/RIGHT 这种离散命令只能"直走或原地转"，
   拿它们拼出来的轨迹会一摊一摊地摆。

   为什么用 0~200 而不是直接用有符号字节：协议里 p1/p2 一直是无符号语义
   （速度百分比），改成有符号会让旧解析器对同一个字节得出完全不同的值，
   而且不会报错。偏移编码把兼容风险限在新动作码内部。 */
#define ACT_TANK        0x09
#define ACT_TANK_BIAS   100    /* p 值减去它就是实际百分比 */

#define ACT_ESTOP       0x07   /* 急停 */
#define ACT_RESUME      0x08   /* 解除急停 */

/* 接收回调函数指针：void cb(u32 id, u8 *buf, u8 len) */
typedef void (*CAN_RxCallback)(u32 id, u8 *buf, u8 len);

/* ============ 接口 ============ */
void CAN1_Init(void);                             /* 初始化 500Kbps */
u8   CAN1_SendStd(u32 id, u8 *buf, u8 len);       /* 发送标准帧 */
void CAN1_SetRxCallback(CAN_RxCallback cb);       /* 注册接收回调 */
u8   CAN1_GetRxCount(void);                       /* 获取 FIFO0 待读帧数(调试用) */

#endif
