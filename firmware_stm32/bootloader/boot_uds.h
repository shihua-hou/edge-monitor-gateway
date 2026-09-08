#ifndef __BOOT_UDS_H
#define __BOOT_UDS_H

#include "stm32f1xx_hal.h"
#include "types.h"

/* STM32F1 出厂烧录的 Flash 容量寄存器，单位 KB（参考手册固定地址，规格书文档化的，
   不是库函数）。原来这里调用一个叫 FLASH_GetSizeFlashtSize() 的函数，但整个项目
   里从没定义过这个函数，标准库也没有这个名字——一直是个编译不过的死代码，现在改成
   直接读寄存器 */
#define FLASH_SIZE_REG  (*(volatile u16 *)0x1FFFF7E0UL)

/* ============ Flash 分区 ============
   分区布局和元数据结构见 fw_meta.h（Boot 和 App 共用）。
   APP_ADDR/APP_SIZE 现在指"当前这次升级要写入的那个槽"，不再是固定的
   0x08004000——具体是 A 还是 B，由 UDS_SetTargetSlot() 在升级开始时设定 */
#include "fw_meta.h"

#define APP_SIZE        SLOT_SIZE
/* 页大小直接用 HAL 自己的 FLASH_PAGE_SIZE（stm32f1xx_hal_flash_ex.h 里已经
   按高密度器件定义成 0x800=2048），不要自己重复定义一份——之前 2048UL 和
   HAL 的 0x800U 值相同但字面量形式不同，编译器按 token 比较判定"不兼容重定义"
   报警告，而且万一以后哪边改动就会真的对不上，不如干脆用同一个 */

/* ============ CAN 帧 ID ============ */
#define CAN_ID_UDS_REQ   0x110   /* 上位机 → Boot 请求 */
#define CAN_ID_UDS_RES   0x120   /* Boot → 上位机响应 */

/* ============ UDS SID ============ */
#define SID_DIAG_SESSION  0x10
#define SID_ECU_RESET     0x11
#define SID_READ_DID      0x22
#define SID_SEC_ACCESS    0x27
#define SID_RUTINE_CTRL   0x31
#define SID_REQ_DOWNLOAD  0x34
#define SID_TRANS_DATA    0x36
#define SID_EXIT_TRANSFER 0x37
#define SID_POS_RESP      0x40   /* SID | 0x40 = 正响应 */
#define SID_NEG_RESP      0x7F

/* ============ DID ============ */
#define DID_APP_VERSION   0xF000
#define DID_BOOT_VERSION  0xF001
/* A/B 状态，响应体：[3]=active [4]=pending(0xFF=无) [5]=target [6]=attempts。
   上位机据此决定传哪份固件，见 uds.c do_read_did 里的说明 */
#define DID_AB_STATUS     0xF002

/* ============ NRC 负响应码 ============ */
#define NRC_LEN_ERROR     0x13
#define NRC_COND_NOT_OK   0x22
#define NRC_OUT_OF_RANGE  0x31
#define NRC_SEC_DENIED    0x33
#define NRC_GEN_FAIL      0x72

/* ============ 传输参数 ============ */
/* 0x36 帧格式：Data[0]=SID, Data[1]=块序号(0~255循环), Data[2..7]=数据。
   数据区占 6 字节而不是 7，是因为空出 1 字节放块序号才能支持丢帧重传
   （之前版本 7 字节全用来装数据，没有序号字段，"块序号+确认重传"是文档
   写了但代码没实现的空话，见 uds.c do_transfer 的重新实现） */
#define BLOCK_LEN         6
/* 固件长度上限 = 单个 A/B 槽的容量。
 *
 * 原来写死 64KB，是 A/B 分区之前遗留下来的。当前固件约 32KB，还没顶到，
 * 所以一直没暴露——这类"今天恰好没事、明天必然出事"的常量最危险：
 * 出问题时你改的是别处（给 App 加了个功能），没人会想到回头看这一行。
 *
 * 而且这个上限是【协议双方都要遵守】的：上位机 uds_tool.c 里有一份、
 * 设备端这里有一份。只改一侧比两侧都不改更糟——上位机放行 240KB、
 * 设备端在 64KB 拒绝，报出来的是 0x34 的否定响应，工具那边显示成
 * "请求下载失败"，比原来那句 "size invalid" 更难定位。
 * （实际发生过：先改了 uds_tool.c 才发现这边也有一份。）
 *
 * 改布局时两处必须一起改，两边注释都写明了对应关系。 */
#define MAX_FIRMWARE     SLOT_SIZE

/* ============ 安全访问（v2 动态种子 + XTEA） ============ */
/* 种子 = 复位计数器 ^ 0xA5A5 ^ (Flash容量KB>>2)，每次复位+1，每次会话不同 */
#define SEC_MAGIC        0xA5A5u
#define XTEA_ROUNDS      32
/* 固定 128-bit 密钥（Boot 与上位机必须一致，不随固件分发） */
#define XTEA_KEY0        0x4D474E54u   /* "MGNT" */
#define XTEA_KEY1        0x45444F4Du   /* "EDOM" */
#define XTEA_KEY2        0x41545741u   /* "ATWA" */
#define XTEA_KEY3        0x592B2B2Bu   /* "Y+++" */

void XTEA_Encipher(u32 *v, const u32 *key);  /* 64-bit 块 XTEA 加密（32轮） */
u32  UDS_GetSeed(void);                      /* 生成动态种子（每次不同） */
u32  UDS_CalcKey(u32 seed);                  /* 由种子计算密钥（XTEA 加密） */

/* ============ 对外接口（boot_main.c 调用） ============ */
void UDS_Init(void);                       /* 初始化 UDS 状态机（含 CRC 表、种子） */
u8   UDS_HandleFrame(u8 *req, u8 len, u8 *resp, u8 *resp_len);
     /* 处理一帧 UDS 请求，生成响应帧（resp 由调用方提供缓冲，至少 8 字节） */
u8   UDS_IsSessionProgramming(void);       /* 是否处于编程会话（用于超时握手判断） */

/* ============ A/B 分区元数据（实现在 uds.c，Boot 侧用） ============ */
void FwMeta_Read(FwMeta_t *out);            /* 读元数据；无效时填一份默认值 */
u8   FwMeta_Write(const FwMeta_t *m);       /* 擦 Meta 页并写回，成功返回 1 */
u32  FwMeta_TargetSlot(void);               /* 本次升级该写哪个槽（当前运行槽的另一个） */
u32  UDS_GetTargetSlotAddr(void);           /* 目标槽起始地址，供擦除/写入使用 */

#endif
