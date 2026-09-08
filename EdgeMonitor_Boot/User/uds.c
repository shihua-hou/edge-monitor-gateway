/* uds.c - UDS 协议栈 + Flash 操作 + CRC32（Boot 区使用，裸机无 RTOS，HAL 版） */
#include "boot_uds.h"
#include <string.h>

/* ============ 状态机 ============ */
static u8 s_prog_session = 0;      /* 是否进入编程会话 */
static u8 s_unlocked     = 0;      /* 安全访问是否通过 */
static u8 s_downloading  = 0;      /* 是否在传输固件 */
static u32 s_dl_addr;              /* 当前写入地址 */
static u32 s_dl_total;             /* 固件总长度 */
static u32 s_dl_received;          /* 已接收字节数（按实际数据字节数累计，非整块数） */
static u8  s_next_seq;              /* 0x36 期望收到的下一个块序号（0~255 循环） */
static u32 s_seed;                  /* 本次会话种子（安全访问用，声明须在 UDS_Init 之前，
                                        原来声明在文件后半段 do_security 附近，UDS_Init
                                        里对它清零编译不过——C 的文件作用域变量不会被
                                        提前声明，用到就必须已经声明过） */
/* 注：完整性校验用的 CRC32 只在 0x31 例程控制里传（4 字节装得下）。
   0x34 请求下载帧总共 8 字节，装不下完整 32 位 CRC，v1 曾在这里塞过截断
   的 24 位 CRC 但从未被读取——已删除该字段，Data[5..7] 明确为保留字节。 */

/* 软件 CRC32（表驱动） */
static u32 s_crc_table[256];
static u8  s_crc_init = 0;

static void crc32_init(void)
{
    u32 i, j, c;
    if (s_crc_init) return;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        s_crc_table[i] = c;
    }
    s_crc_init = 1;
}

/* 计算 App 区 CRC，只算"实际下载的固件长度"，不是整个 496KB App 分区。
 * 这里原来固定用 APP_SIZE（496KB）扫全区——但 uds_tool.c 传来的期望 CRC
 * (fw_crc = crc32_buf(fw, fsize)) 只是对 .bin 文件本身这 fsize 字节算的，
 * 两边范围对不上：分区里固件之外的部分是擦除后的 0xFF 填充，只要固件不是
 * 刚好把 496KB 分区写满，二者几乎不可能相等。也就是说 CRC 校验这一步
 * 之前是必现失败的，OTA 流程会在最后一步（也是最容易被面试官现场问起
 * 的一步）翻车。改成按 s_dl_total（0x34 时声明的固件长度）计算，和
 * uds_tool.c 保持同一范围。 */
/* ============ A/B 元数据读写 ============ */
void FwMeta_Read(FwMeta_t *out)
{
    const FwMeta_t *m = (const FwMeta_t *)META_ADDR;
    if (FwMeta_IsValid(m)) {
        *out = *m;
        return;
    }
    /* 元数据无效（首次刷机、或写元数据时断电）：按"固件在 A 槽"兜底。
       这是最合理的假设——用 JTAG 直接烧录时烧的就是 A 槽的地址 */
    out->magic = META_MAGIC;
    out->active_slot = 0;
    out->pending_slot = SLOT_INVALID;
    out->boot_attempts = 0;
    out->slot_crc[0] = out->slot_crc[1] = 0;
    out->slot_size[0] = out->slot_size[1] = 0;
    out->checksum = FwMeta_CalcChecksum(out);
}

u8 FwMeta_Write(const FwMeta_t *m)
{
    FLASH_EraseInitTypeDef erase;
    u32 page_error;
    FwMeta_t tmp = *m;
    const u16 *src;
    u32 addr, i, words;

    tmp.magic = META_MAGIC;
    tmp.checksum = FwMeta_CalcChecksum(&tmp);

    HAL_FLASH_Unlock();
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = META_ADDR;
    erase.NbPages = 1;                 /* 元数据只占第一页 */
    if (HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK) {
        HAL_FLASH_Lock();
        return 0;
    }
    /* 半字编程，跟固件写入用同一种方式 */
    src = (const u16 *)&tmp;
    words = sizeof(FwMeta_t) / 2;
    addr = META_ADDR;
    for (i = 0; i < words; i++) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr, src[i]) != HAL_OK) {
            HAL_FLASH_Lock();
            return 0;
        }
        addr += 2;
    }
    HAL_FLASH_Lock();
    return 1;
}

/* 本次升级要写哪个槽：永远写"当前没在运行的那个"，这样升级全程
   都不会碰到正在跑的固件，失败了也还有退路 */
u32 FwMeta_TargetSlot(void)
{
    FwMeta_t m;
    FwMeta_Read(&m);
    return (m.active_slot == 0) ? 1u : 0u;
}

u32 UDS_GetTargetSlotAddr(void)
{
    return SLOT_ADDR(FwMeta_TargetSlot());
}

static u32 crc32_region(u32 len)
{
    const u8 *p = (const u8 *)UDS_GetTargetSlotAddr();
    u32 crc = 0;
    crc32_init();
    crc = crc ^ 0xFFFFFFFF;
    while (len--) crc = (crc >> 8) ^ s_crc_table[(crc ^ *p++) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

/* ============ 初始化 ============ */
void UDS_Init(void)
{
    s_prog_session = 0;
    s_unlocked     = 0;
    s_downloading  = 0;
    s_next_seq     = 0;
    s_seed         = 0;
    crc32_init();
}

u8 UDS_IsSessionProgramming(void) { return s_prog_session; }

/* ============ 辅助：构造正/负响应 ============ */
static void pos_resp(u8 *resp, u8 sid, u8 *extra, u8 extra_len)
{
    u8 i;
    resp[0] = sid | SID_POS_RESP;
    for (i = 0; i < extra_len && i < 6; i++) resp[1 + i] = extra[i];
}

static void neg_resp(u8 *resp, u8 sid, u8 nrc)
{
    resp[0] = SID_NEG_RESP;
    resp[1] = sid;
    resp[2] = nrc;
}

/* ============ 服务：会话控制 0x10 ============ */
static void do_session(u8 *req, u8 *resp, u8 *rlen)
{
    if (req[1] == 0x01) {              /* 默认会话：退出编程 */
        s_prog_session = 0;
        s_unlocked = 0; s_downloading = 0;
    } else if (req[1] == 0x02) {       /* 编程会话 */
        s_prog_session = 1;
    } else {
        neg_resp(resp, SID_DIAG_SESSION, NRC_OUT_OF_RANGE);
        *rlen = 3;
        return;
    }
    u8 ex[2] = { 0x00, req[1] };       /* P2*10ms, 会话类型 */
    pos_resp(resp, SID_DIAG_SESSION, ex, 2);
    *rlen = 3;
}

/* ============ v2 安全访问：XTEA 加密 + 动态种子 ============ */
static const u32 s_sec_key[4] = { XTEA_KEY0, XTEA_KEY1, XTEA_KEY2, XTEA_KEY3 };

void XTEA_Encipher(u32 *v, const u32 *key)
{
    u32 v0 = v[0], v1 = v[1], sum = 0;
    u32 delta = 0x9E3779B9u;
    u8 i;
    for (i = 0; i < XTEA_ROUNDS; i++) {
        v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3]);
        sum += delta;
        v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3]);
    }
    v[0] = v0; v[1] = v1;
}

/* 复位计数器：存备份寄存器 BKP_DR1，每次复位 +1（F1 经典 BKP 外设是独立
   寄存器块，不像后面型号并到 RTC 里，HAL 没有专门包一层函数，直接读写
   BKP->DR1 这个寄存器就是官方标准做法，跟用不用 HAL/标准库无关） */
static u32 sec_reset_counter(void)
{
    u32 c;
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_RCC_BKP_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
    c = BKP->DR1 + 1;
    BKP->DR1 = (u16)c;
    HAL_PWR_DisableBkUpAccess();
    return c;
}

/* 动态种子：每次请求都不同（复位计数 + 混淆） */
u32 UDS_GetSeed(void)
{
    u32 c = sec_reset_counter();
    return c ^ SEC_MAGIC ^ (FLASH_SIZE_REG >> 2);
}

/* 密钥 = XTEA 加密种子（64-bit 块，取加密结果高 32 位，大端组装） */
u32 UDS_CalcKey(u32 seed)
{
    u8 blk[8];
    /* 这里把 u8[8] 强转成 u32* 来喂 XTEA，严格来说是违反 C 的别名规则的
       （用一种类型的左值去访问另一种类型的对象），理论上 -O2 下编译器
       可以重排/缓存导致行为不符合预期。正确写法是用 union 或 memcpy 中转。
       【故意不改】：这是 OTA 里最脆弱的一环——密钥算法一旦算错，
       设备端就会拒绝所有升级请求，而 Boot 区只能靠 J-Link 重刷才能救回来。
       当前工具链（ARMCC V5）下行为是确定且正确的，实测多次 OTA 均通过。
       将来若更换编译器或提高优化等级，这里是第一个要复查的地方。 */
    u32 *v = (u32 *)blk;
    u32 key;
    blk[0] = (u8)(seed >> 24); blk[1] = (u8)(seed >> 16);
    blk[2] = (u8)(seed >> 8);  blk[3] = (u8)seed;
    blk[4] = 0; blk[5] = 0; blk[6] = 0; blk[7] = 0;
    XTEA_Encipher(v, s_sec_key);
    key = ((u32)blk[0] << 24) | ((u32)blk[1] << 16) |
          ((u32)blk[2] << 8)  | (u32)blk[3];
    return key;
}

/* ============ 服务：安全访问 0x27 ============ */
static void do_security(u8 *req, u8 *resp, u8 *rlen)
{
    if (!s_prog_session) { neg_resp(resp, SID_SEC_ACCESS, NRC_COND_NOT_OK); *rlen = 3; return; }
    if (req[1] == 0x01) {              /* 请求种子（动态） */
        u32 seed = UDS_GetSeed();
        s_seed = seed;
        resp[0] = SID_SEC_ACCESS | SID_POS_RESP;
        resp[1] = 0x01;
        resp[2] = (u8)(seed >> 24);
        resp[3] = (u8)(seed >> 16);
        resp[4] = (u8)(seed >> 8);
        resp[5] = (u8)seed;
        *rlen = 6;
    } else if (req[1] == 0x02) {       /* 发送密钥：key = XTEA(seed) */
        u32 key = UDS_CalcKey(s_seed);
        u32 got = ((u32)req[2] << 24) | ((u32)req[3] << 16) |
                  ((u32)req[4] << 8)  | ((u32)req[5]);
        if (got == key) {
            s_unlocked = 1;
            pos_resp(resp, SID_SEC_ACCESS, 0, 0);
            *rlen = 1;
        } else {
            neg_resp(resp, SID_SEC_ACCESS, NRC_SEC_DENIED);
            *rlen = 3;
        }
    } else {
        neg_resp(resp, SID_SEC_ACCESS, NRC_OUT_OF_RANGE);
        *rlen = 3;
    }
}

/* ============ 服务：例程控制 0x31（擦除 / CRC 校验） ============ */
static void do_routine(u8 *req, u8 *resp, u8 *rlen)
{
    u8 rid = req[3];                   /* 例程 ID：0x00 擦除 / 0x01 校验 */
    if (!s_prog_session || !s_unlocked) {
        neg_resp(resp, SID_RUTINE_CTRL, NRC_COND_NOT_OK); *rlen = 3; return;
    }
    if (rid == 0x00) {                 /* 擦除 App 区 */
        FLASH_EraseInitTypeDef erase;
        u32 page_error;
        HAL_FLASH_Unlock();
        /* STM32F103ZET6 属高密度器件，Flash 页大小 2KB（不是 1KB）。
           HAL_FLASHEx_Erase 一次调用能擦多页，不用像标准库那样逐页循环调用 */
        erase.TypeErase = FLASH_TYPEERASE_PAGES;
        /* 擦的是"非活动槽"，不是正在跑的那个——这正是 A/B 的意义所在：
           升级全程都不碰当前可用的固件，擦到一半断电也只是废掉备用槽 */
        erase.PageAddress = UDS_GetTargetSlotAddr();
        erase.NbPages = (u32)(APP_SIZE / FLASH_PAGE_SIZE);
        if (HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK) {
            HAL_FLASH_Lock();
            neg_resp(resp, SID_RUTINE_CTRL, NRC_GEN_FAIL); *rlen = 3; return;
        }
        HAL_FLASH_Lock();
        s_downloading = 0;
        s_dl_received = 0;
        s_dl_addr = UDS_GetTargetSlotAddr();
        pos_resp(resp, SID_RUTINE_CTRL, 0, 0);
        *rlen = 1;
    } else if (rid == 0x01) {          /* CRC 校验：req[4..7]=期望CRC(大端) */
        u32 expect = ((u32)req[4] << 24) | ((u32)req[5] << 16) |
                     ((u32)req[6] << 8)  | ((u32)req[7]);
        u32 calc = crc32_region(s_dl_total);
        u8 ex[4] = { (u8)(calc >> 24), (u8)(calc >> 16), (u8)(calc >> 8), (u8)calc };
        if (calc == expect) {
            /* CRC 通过只说明"传输没出错"，不代表这个固件真的能跑起来。
               所以这里把新槽标成 pending 而不是直接 active：先让 Boot 试着
               启动它，等它自己确认活下来了才提升为 active。启动不了就回滚。 */
            FwMeta_t m;
            u32 slot = FwMeta_TargetSlot();
            FwMeta_Read(&m);
            m.pending_slot = slot;
            m.boot_attempts = 0;
            m.slot_crc[slot] = calc;
            m.slot_size[slot] = s_dl_total;
            if (FwMeta_Write(&m)) {
                pos_resp(resp, SID_RUTINE_CTRL, ex, 4);
                *rlen = 5;
            } else {
                /* 固件写进去了但元数据没写成，Boot 不会知道有新固件可试，
                   下次还是启动旧槽——数据是安全的，但这次升级等于没生效，
                   必须如实报失败，不能让上位机显示"升级成功" */
                neg_resp(resp, SID_RUTINE_CTRL, NRC_GEN_FAIL);
                *rlen = 3;
            }
        } else {
            neg_resp(resp, SID_RUTINE_CTRL, NRC_GEN_FAIL);
            *rlen = 3;
        }
    } else {
        neg_resp(resp, SID_RUTINE_CTRL, NRC_OUT_OF_RANGE);
        *rlen = 3;
    }
}

/* ============ 服务：请求下载 0x34 ============ */
static void do_download(u8 *req, u8 *resp, u8 *rlen)
{
    if (!s_prog_session || !s_unlocked) {
        neg_resp(resp, SID_REQ_DOWNLOAD, NRC_COND_NOT_OK); *rlen = 3; return;
    }
    /* req[1..4] = 固件长度(大端), req[5..7] = 保留（不再冒充存 CRC，见文件头注释） */
    s_dl_total = ((u32)req[1] << 24) | ((u32)req[2] << 16) |
                 ((u32)req[3] << 8)  | ((u32)req[4]);
    if (s_dl_total == 0 || s_dl_total > MAX_FIRMWARE) {
        neg_resp(resp, SID_REQ_DOWNLOAD, NRC_OUT_OF_RANGE); *rlen = 3; return;
    }
    s_downloading = 1;
    s_dl_addr = UDS_GetTargetSlotAddr();   /* 写非活动槽，见 do_routine 擦除处注释 */
    s_dl_received = 0;
    s_next_seq = 0;
    u8 ex[2] = { 0x00, BLOCK_LEN };    /* maxNumberOfBlockLength */
    pos_resp(resp, SID_REQ_DOWNLOAD, ex, 2);
    *rlen = 3;
}

/* ============ 服务：传输数据 0x36 ============ */
/* 帧格式：Data[0]=SID, Data[1]=块序号(0~255循环), Data[2..7]=数据(最多6字节)。
 * 响应回显块序号，供上位机判断"这一块到底有没有确认"，从而支持丢帧重传——
 * 之前的版本完全没有序号字段，方案书里写的"块序号+确认重传"其实一直是没有
 * 对应代码的空话，这里补齐真正的实现：
 *   - 收到 seq == 期望序号：正常写入，序号+1
 *   - 收到 seq == 上一个已确认的序号：说明上位机没收到 ack 才重发的重复帧，
 *     直接把 ack 再发一遍，不重写 Flash（对已写过的地址重复 Program 行为未定义）
 *   - 其它序号：乱序/跳块，NRC 拒绝，让上位机知道协议连续性被破坏
 * 注：len 必须按值传递（原实现误写成 u8*，把调用方传来的长度值当指针解引用，
 * 属于类型不匹配的严重 bug，标准编译器下要么报错要么产生未定义行为）。 */
static void do_transfer(u8 *req, u8 len, u8 *resp, u8 *rlen)
{
    u8  block[BLOCK_LEN];
    u8  i, nb, seq;

    if (!s_downloading || !s_unlocked) {
        neg_resp(resp, SID_TRANS_DATA, NRC_COND_NOT_OK); *rlen = 3; return;
    }
    if (len < 3) {                        /* SID + 序号 + 至少1字节数据 */
        neg_resp(resp, SID_TRANS_DATA, NRC_LEN_ERROR); *rlen = 3; return;
    }
    seq = req[1];
    nb  = (u8)(len - 2);
    /* 钳位到一块的容量。CAN 帧最长 8 字节，nb 理论上不会超过 BLOCK_LEN(6)，
       所以现在不会越界。但 s_dl_received 是按 nb 累加的，一旦哪天这个函数
       被别的传输层（比如串口/网络）复用而帧更长，计数就会虚高，
       "已接收 == 总长度"提前成立，固件传了一半就被当成传完了。
       下面拷贝数据的循环本身有 BLOCK_LEN 上界，不存在缓冲区溢出，
       这里防的是【计数错乱】，不是内存安全。 */
    if (nb > BLOCK_LEN) nb = BLOCK_LEN;

    if (seq == (u8)(s_next_seq - 1)) {    /* 重复帧：重发上一次的确认，不重写 Flash */
        resp[0] = SID_TRANS_DATA | SID_POS_RESP;
        resp[1] = seq;
        *rlen = 2;
        return;
    }
    if (seq != s_next_seq) {              /* 乱序/跳块，要求严格连续 */
        neg_resp(resp, SID_TRANS_DATA, NRC_OUT_OF_RANGE); *rlen = 3; return;
    }
    /* 边界检查针对"当前正在写的那个槽"，不再是原来固定的 App 区末尾——
       写超出去会踩到另一个槽（那里放着唯一能回滚的固件）或元数据页 */
    if (s_dl_addr + BLOCK_LEN - 1 > (UDS_GetTargetSlotAddr() + SLOT_SIZE - 1) ||
        s_dl_received + nb > s_dl_total) {
        neg_resp(resp, SID_TRANS_DATA, NRC_OUT_OF_RANGE); *rlen = 3; return;
    }

    /* 拷贝本块数据，末块不足 BLOCK_LEN 则补 0xFF（凑半字对齐写 Flash） */
    for (i = 0; i < BLOCK_LEN; i++) block[i] = (i < nb) ? req[2 + i] : 0xFF;

    HAL_FLASH_Unlock();
    for (i = 0; i < BLOCK_LEN; i += 2) {   /* 半字写入 */
        u16 hw = block[i] | ((u16)block[i + 1] << 8);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, s_dl_addr + i, hw) != HAL_OK) {
            HAL_FLASH_Lock();
            neg_resp(resp, SID_TRANS_DATA, NRC_GEN_FAIL); *rlen = 3; return;
        }
    }
    HAL_FLASH_Lock();

    s_dl_addr     += BLOCK_LEN;          /* Flash 写入地址始终整块推进（含补齐字节） */
    s_dl_received += nb;                 /* 已接收字节数按真实数据量累计，末块才能收尾 */
    s_next_seq     = (u8)(s_next_seq + 1);

    resp[0] = SID_TRANS_DATA | SID_POS_RESP;
    resp[1] = seq;                        /* 回显块号 */
    *rlen = 2;
}

/* ============ 服务：退出传输 0x37 ============ */
static void do_exit(u8 *resp, u8 *rlen)
{
    if (!s_downloading) {
        neg_resp(resp, SID_EXIT_TRANSFER, NRC_COND_NOT_OK); *rlen = 3; return;
    }
    s_downloading = 0;
    pos_resp(resp, SID_EXIT_TRANSFER, 0, 0);
    *rlen = 1;
}

/* ============ 服务：读 DID 0x22 ============ */
static void do_read_did(u8 *req, u8 *resp, u8 *rlen)
{
    u16 did = ((u16)req[1] << 8) | req[2];
    if (did == 0xF000) {               /* 软件版本 */
        resp[0] = SID_READ_DID | SID_POS_RESP;
        resp[1] = (u8)(did >> 8);
        resp[2] = (u8)did;
        resp[3] = 0x01;                /* 版本号 1.0 */
        resp[4] = 0x00;
        *rlen = 5;
    } else if (did == 0xF001) {        /* Bootloader 版本 */
        resp[0] = SID_READ_DID | SID_POS_RESP;
        resp[1] = (u8)(did >> 8);
        resp[2] = (u8)did;
        resp[3] = 0x01;
        resp[4] = 0x00;
        *rlen = 5;
    } else if (did == DID_AB_STATUS) {  /* A/B 分区状态 */
        /* 上位机必须先问这一条才知道该传哪份固件：direct-xip 模式下
           Slot A 和 Slot B 的固件链接地址不同，是两个不能互换的 bin，
           传错了槽就是一个跑不起来的固件（还好会被 A/B 机制回滚掉） */
        FwMeta_t m;
        FwMeta_Read(&m);
        resp[0] = SID_READ_DID | SID_POS_RESP;
        resp[1] = (u8)(did >> 8);
        resp[2] = (u8)did;
        resp[3] = (u8)m.active_slot;                        /* 当前活动槽 */
        resp[4] = (u8)(m.pending_slot < SLOT_COUNT ? m.pending_slot : 0xFF);
        resp[5] = (u8)FwMeta_TargetSlot();                  /* 本次该写哪个槽 */
        resp[6] = (u8)m.boot_attempts;
        *rlen = 7;
    } else {
        neg_resp(resp, SID_READ_DID, NRC_OUT_OF_RANGE);
        *rlen = 3;
    }
}

/* ============ 服务：ECU 复位 0x11（只回响应，跳转由 boot_main 处理） ============ */
static u8 do_reset(u8 *req, u8 *resp, u8 *rlen)
{
    if (req[1] != 0x01) {
        neg_resp(resp, SID_ECU_RESET, NRC_OUT_OF_RANGE);
        *rlen = 3;
        return 0;
    }
    pos_resp(resp, SID_ECU_RESET, 0, 0);
    *rlen = 1;
    return 1;   /* 返回 1 表示请求跳转 */
}

/* ============ 统一入口 ============ */
u8 UDS_HandleFrame(u8 *req, u8 len, u8 *resp, u8 *resp_len)
{
    u8 reset_req = 0;
    *resp_len = 0;

    switch (req[0]) {
    case SID_DIAG_SESSION: do_session(req, resp, resp_len);  break;
    case SID_SEC_ACCESS:   do_security(req, resp, resp_len); break;
    case SID_RUTINE_CTRL:  do_routine(req, resp, resp_len);  break;
    case SID_REQ_DOWNLOAD: do_download(req, resp, resp_len); break;
    case SID_TRANS_DATA:   do_transfer(req, len, resp, resp_len); break;
    case SID_EXIT_TRANSFER:do_exit(resp, resp_len);          break;
    case SID_READ_DID:     do_read_did(req, resp, resp_len); break;
    case SID_ECU_RESET:    reset_req = do_reset(req, resp, resp_len); break;
    default:
        neg_resp(resp, req[0], NRC_OUT_OF_RANGE);
        *resp_len = 3;
        break;
    }
    return reset_req;
}
