#ifndef __FW_META_H
#define __FW_META_H

#include "types.h"

/* ============================================================
 * A/B 双分区 OTA 的 Flash 布局与元数据
 *
 * Boot 和 App 都要用这个头（Boot 负责选槽/回滚，App 负责启动确认），
 * 所以单独拆出来，两个 Keil 工程各放一份（由 scripts/sync_firmware.py 保持一致）。
 *
 * 为什么要 A/B：原来只有一个 App 区，CRC 校验失败时能停在 Boot 不刷坏，
 * 但新固件如果"能启动却有 bug"（跑飞、外设初始化失败、逻辑错误），旧固件
 * 已经被覆盖了，没有任何退路——只能接上 JTAG 重刷。装在巡检小车上的设备
 * 出这种情况就得召回。A/B 分区让新固件写进另一个槽，启动不起来就自动回滚。
 *
 * 采用 direct-xip 模式（MCUboot 的两种模式之一）：两个槽都能直接执行，
 * 切换只改元数据里一个字段，瞬时生效、回滚零成本、没有拷贝窗口。
 * 代价是两个槽的固件链接地址不同，必须编译两份 bin —— 因为 Cortex-M
 * 的代码不是位置无关的，链接到 0x08004000 的固件放到 0x08040000 上跑不起来。
 * （另一种 swap 模式只需一份固件，但要把备份区拷贝到运行区，拷贝途中断电
 *  会毁掉唯一可运行的固件，还得再加一层拷贝恢复逻辑，不划算。）
 *
 * Flash 布局（STM32F103ZET6 共 512KB，页 2KB）：
 *   0x08000000  Boot      16KB
 *   0x08004000  Slot A   240KB
 *   0x08040000  Slot B   240KB
 *   0x0807C000  Meta      16KB（只用第一页 2KB，其余保留）
 * ============================================================ */

#define BOOT_ADDR        0x08000000UL
#define BOOT_SIZE        (16 * 1024UL)

#define SLOT_A_ADDR      0x08004000UL
#define SLOT_B_ADDR      0x08040000UL
#define SLOT_SIZE        (240 * 1024UL)

#define META_ADDR        0x0807C000UL
#define META_SIZE        (16 * 1024UL)

#define SLOT_COUNT       2
#define SLOT_INVALID     0xFFFFFFFFUL

/* slot 号(0/1) → 起始地址 */
#define SLOT_ADDR(n)     ((n) == 0 ? SLOT_A_ADDR : SLOT_B_ADDR)

/* 新固件启动失败多少次之后回滚。设 3 是给"偶发启动失败"留点余地
   （比如上电瞬间某个外设没就绪），但又不至于让人对着一台反复重启的
   设备等太久：3 次失败大约十几秒就会回到旧固件 */
#define MAX_BOOT_ATTEMPTS   3

#define META_MAGIC       0xEDA0B1CEUL

/* 元数据。写之前要擦掉整页（2KB），所以字段尽量少、一次写完。
   全部用 u32 是为了半字编程时不用处理跨字对齐 */
typedef struct {
    u32 magic;              /* META_MAGIC，不匹配就当元数据无效 */
    u32 active_slot;        /* 当前确认可用的槽，Boot 默认启动它 */
    u32 pending_slot;       /* 刚升级完、还没确认的槽；SLOT_INVALID 表示没有 */
    u32 boot_attempts;      /* pending 槽已经被尝试启动几次 */
    u32 slot_crc[SLOT_COUNT];   /* 各槽固件 CRC32，Boot 跳转前校验 */
    u32 slot_size[SLOT_COUNT];  /* 各槽固件字节数，0 表示该槽是空的 */
    u32 checksum;           /* 以上字段之和，防止读到写了一半的元数据 */
} FwMeta_t;

/* 元数据自身的校验和：字段不多，直接求和即可，目的只是识别
   "写到一半断电"这种半成品，不是防篡改 */
static __inline u32 FwMeta_CalcChecksum(const FwMeta_t *m)
{
    return m->magic + m->active_slot + m->pending_slot + m->boot_attempts
         + m->slot_crc[0] + m->slot_crc[1]
         + m->slot_size[0] + m->slot_size[1];
}

static __inline u8 FwMeta_IsValid(const FwMeta_t *m)
{
    return (m->magic == META_MAGIC && m->checksum == FwMeta_CalcChecksum(m)) ? 1 : 0;
}

#endif /* __FW_META_H */
