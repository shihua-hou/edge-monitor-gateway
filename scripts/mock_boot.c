/* mock_boot.c - host 端"假 Bootloader"，在 vcan/真实 CAN 上复现
 * firmware_stm32/bootloader/uds.c 的协议状态机（会话/安全访问/擦除/下载/
 * 传输/CRC/复位），只是把"写 Flash"换成写一块内存 + 落盘，用来在没有真机
 * 的情况下回归测试 uds_tool.c 的 OTA 流程——尤其是之前修过的两个坑：
 *   1) 固件大小不是 BLOCK_LEN(6) 整数倍时最后一块的收尾逻辑
 *   2) CRC 校验范围两端要对齐（按实际固件长度，不是整个 App 分区）
 *
 * 协议参数（SID/NRC/XTEA key/种子公式）必须和 uds.c / boot_uds.h 保持一致，
 * 这里是手动同步的两份实现，不是共享同一份源码——修改协议后两边都要改，
 * 这本身也是这个脚本要覆盖的"两端不一致"风险。
 *
 * 用法：
 *   ./mock_boot <can口，如 vcan0> <收到的固件落盘路径> [--verbose]
 *
 * 退出方式：收到 0x11 ECU 复位请求后，把已收到的 dl_total 字节写到目标文件，
 * 打印一行 "MOCK_BOOT_DONE" 后退出（ota_test.sh 靠这行判断本轮结束）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/can.h>
#include <linux/can/raw.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* ============ 协议常量（须与 firmware_stm32/bootloader/boot_uds.h 一致） ============ */
#define CAN_ID_UDS_REQ   0x110
#define CAN_ID_UDS_RES   0x120

#define SID_DIAG_SESSION  0x10
#define SID_ECU_RESET     0x11
#define SID_READ_DID      0x22
#define SID_SEC_ACCESS    0x27
#define SID_RUTINE_CTRL   0x31
#define SID_REQ_DOWNLOAD  0x34
#define SID_TRANS_DATA    0x36
#define SID_EXIT_TRANSFER 0x37
#define SID_POS_RESP      0x40
#define SID_NEG_RESP      0x7F

#define NRC_LEN_ERROR     0x13
#define NRC_COND_NOT_OK   0x22
#define NRC_OUT_OF_RANGE  0x31
#define NRC_SEC_DENIED    0x33
#define NRC_GEN_FAIL      0x72

#define BLOCK_LEN         6
#define MAX_FIRMWARE      (64 * 1024)

#define SEC_MAGIC         0xA5A5u
#define XTEA_ROUNDS       32
#define XTEA_KEY0         0x4D474E54u
#define XTEA_KEY1         0x45444F4Du
#define XTEA_KEY2         0x41545741u
#define XTEA_KEY3         0x592B2B2Bu
static const u32 s_sec_key[4] = { XTEA_KEY0, XTEA_KEY1, XTEA_KEY2, XTEA_KEY3 };

/* ============ 状态机（对应 uds.c 里的 static 变量） ============ */
static u8  s_prog_session = 0;
static u8  s_unlocked     = 0;
static u8  s_downloading  = 0;
static u32 s_dl_addr;      /* 相对 sim_flash 起始的偏移，不是绝对地址 */
static u32 s_dl_total;
static u32 s_dl_received;
static u8  s_next_seq;
static u32 s_seed;
static u32 s_reset_counter = 0;   /* 模拟 BKP_DR1，每次请求种子当作复位一次 */
/* 真机的 App 分区(496KB)比 MAX_FIRMWARE(64KB)大得多，末块整块写入(含 0xFF
   补齐)造成的最多 BLOCK_LEN-1 字节越界完全有富余空间；这里的模拟"Flash"
   如果刚好只开 MAX_FIRMWARE 大小，固件长度恰好等于 MAX_FIRMWARE 时最后一块
   反而会被越界检查误判——多留 BLOCK_LEN 字节余量，行为才和真机一致 */
static u8  sim_flash[MAX_FIRMWARE + BLOCK_LEN];
static int g_verbose = 0;

/* ============ XTEA（与 uds.c / uds_tool.c 完全一致） ============ */
static void xtea_encipher(u32 *v, const u32 *key)
{
    u32 v0 = v[0], v1 = v[1], sum = 0;
    u32 delta = 0x9E3779B9u;
    int i;
    for (i = 0; i < XTEA_ROUNDS; i++) {
        v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3]);
        sum += delta;
        v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3]);
    }
    v[0] = v0; v[1] = v1;
}

static u32 calc_key(u32 seed)
{
    u8 blk[8];
    u32 key;
    blk[0] = (u8)(seed >> 24); blk[1] = (u8)(seed >> 16);
    blk[2] = (u8)(seed >> 8);  blk[3] = (u8)seed;
    blk[4] = blk[5] = blk[6] = blk[7] = 0;
    xtea_encipher((u32 *)blk, s_sec_key);
    key = ((u32)blk[0] << 24) | ((u32)blk[1] << 16) | ((u32)blk[2] << 8) | (u32)blk[3];
    return key;
}

/* ============ CRC32（与 uds.c / uds_tool.c 一致） ============ */
static u32 crc_table[256];
static void crc_init(void)
{
    u32 i, j, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
}
static u32 crc32_region(const u8 *p, u32 len)
{
    u32 crc = 0xFFFFFFFFu;
    while (len--) crc = (crc >> 8) ^ crc_table[(crc ^ *p++) & 0xFF];
    return crc ^ 0xFFFFFFFFu;
}

/* ============ 响应构造 ============ */
static void pos_resp(u8 *resp, u8 sid, const u8 *extra, u8 extra_len)
{
    u8 i;
    resp[0] = sid | SID_POS_RESP;
    for (i = 0; i < extra_len && i < 6; i++) resp[1 + i] = extra[i];
}
static void neg_resp(u8 *resp, u8 sid, u8 nrc)
{
    resp[0] = SID_NEG_RESP; resp[1] = sid; resp[2] = nrc;
}

/* ============ 各服务，逻辑对应 uds.c ============ */
static void do_session(u8 *req, u8 *resp, u8 *rlen)
{
    if (req[1] == 0x01) {
        s_prog_session = 0; s_unlocked = 0; s_downloading = 0;
    } else if (req[1] == 0x02) {
        s_prog_session = 1;
    } else {
        neg_resp(resp, SID_DIAG_SESSION, NRC_OUT_OF_RANGE); *rlen = 3; return;
    }
    u8 ex[2] = { 0x00, req[1] };
    pos_resp(resp, SID_DIAG_SESSION, ex, 2);
    *rlen = 3;
}

static void do_security(u8 *req, u8 *resp, u8 *rlen)
{
    if (!s_prog_session) { neg_resp(resp, SID_SEC_ACCESS, NRC_COND_NOT_OK); *rlen = 3; return; }
    if (req[1] == 0x01) {
        s_reset_counter++;
        u32 seed = s_reset_counter ^ SEC_MAGIC ^ (MAX_FIRMWARE >> 2);
        s_seed = seed;
        resp[0] = SID_SEC_ACCESS | SID_POS_RESP;
        resp[1] = 0x01;
        resp[2] = (u8)(seed >> 24); resp[3] = (u8)(seed >> 16);
        resp[4] = (u8)(seed >> 8);  resp[5] = (u8)seed;
        *rlen = 6;
    } else if (req[1] == 0x02) {
        u32 key = calc_key(s_seed);
        u32 got = ((u32)req[2] << 24) | ((u32)req[3] << 16) | ((u32)req[4] << 8) | (u32)req[5];
        if (got == key) {
            s_unlocked = 1;
            pos_resp(resp, SID_SEC_ACCESS, NULL, 0); *rlen = 1;
        } else {
            neg_resp(resp, SID_SEC_ACCESS, NRC_SEC_DENIED); *rlen = 3;
        }
    } else {
        neg_resp(resp, SID_SEC_ACCESS, NRC_OUT_OF_RANGE); *rlen = 3;
    }
}

static void do_routine(u8 *req, u8 *resp, u8 *rlen)
{
    u8 rid = req[3];
    if (!s_prog_session || !s_unlocked) {
        neg_resp(resp, SID_RUTINE_CTRL, NRC_COND_NOT_OK); *rlen = 3; return;
    }
    if (rid == 0x00) {
        memset(sim_flash, 0xFF, sizeof(sim_flash));
        s_downloading = 0; s_dl_received = 0; s_dl_addr = 0;
        pos_resp(resp, SID_RUTINE_CTRL, NULL, 0); *rlen = 1;
        if (g_verbose) printf("[mock] erase ok\n");
    } else if (rid == 0x01) {
        u32 expect = ((u32)req[4] << 24) | ((u32)req[5] << 16) | ((u32)req[6] << 8) | (u32)req[7];
        u32 calc = crc32_region(sim_flash, s_dl_total);
        u8 ex[4] = { (u8)(calc >> 24), (u8)(calc >> 16), (u8)(calc >> 8), (u8)calc };
        if (calc == expect) { pos_resp(resp, SID_RUTINE_CTRL, ex, 4); *rlen = 5; }
        else                { neg_resp(resp, SID_RUTINE_CTRL, NRC_GEN_FAIL); *rlen = 3; }
        if (g_verbose) printf("[mock] crc check: calc=0x%08X expect=0x%08X %s\n",
                               calc, expect, (calc == expect) ? "OK" : "MISMATCH");
    } else {
        neg_resp(resp, SID_RUTINE_CTRL, NRC_OUT_OF_RANGE); *rlen = 3;
    }
}

static void do_download(u8 *req, u8 *resp, u8 *rlen)
{
    if (!s_prog_session || !s_unlocked) {
        neg_resp(resp, SID_REQ_DOWNLOAD, NRC_COND_NOT_OK); *rlen = 3; return;
    }
    s_dl_total = ((u32)req[1] << 24) | ((u32)req[2] << 16) | ((u32)req[3] << 8) | (u32)req[4];
    if (s_dl_total == 0 || s_dl_total > MAX_FIRMWARE) {
        neg_resp(resp, SID_REQ_DOWNLOAD, NRC_OUT_OF_RANGE); *rlen = 3; return;
    }
    s_downloading = 1; s_dl_addr = 0; s_dl_received = 0; s_next_seq = 0;
    u8 ex[2] = { 0x00, BLOCK_LEN };
    pos_resp(resp, SID_REQ_DOWNLOAD, ex, 2); *rlen = 3;
    if (g_verbose) printf("[mock] download requested, total=%u\n", s_dl_total);
}

static void do_transfer(u8 *req, u8 len, u8 *resp, u8 *rlen)
{
    u8 block[BLOCK_LEN], i, nb, seq;
    if (!s_downloading || !s_unlocked) {
        neg_resp(resp, SID_TRANS_DATA, NRC_COND_NOT_OK); *rlen = 3; return;
    }
    if (len < 3) { neg_resp(resp, SID_TRANS_DATA, NRC_LEN_ERROR); *rlen = 3; return; }
    seq = req[1];
    nb  = (u8)(len - 2);

    if (seq == (u8)(s_next_seq - 1)) {
        resp[0] = SID_TRANS_DATA | SID_POS_RESP; resp[1] = seq; *rlen = 2; return;
    }
    if (seq != s_next_seq) {
        neg_resp(resp, SID_TRANS_DATA, NRC_OUT_OF_RANGE); *rlen = 3; return;
    }
    if (s_dl_addr + BLOCK_LEN > sizeof(sim_flash) || s_dl_received + nb > s_dl_total) {
        neg_resp(resp, SID_TRANS_DATA, NRC_OUT_OF_RANGE); *rlen = 3; return;
    }
    for (i = 0; i < BLOCK_LEN; i++) block[i] = (i < nb) ? req[2 + i] : 0xFF;
    memcpy(&sim_flash[s_dl_addr], block, BLOCK_LEN);

    s_dl_addr     += BLOCK_LEN;
    s_dl_received += nb;
    s_next_seq     = (u8)(s_next_seq + 1);

    resp[0] = SID_TRANS_DATA | SID_POS_RESP; resp[1] = seq; *rlen = 2;
}

static void do_exit(u8 *resp, u8 *rlen)
{
    if (!s_downloading) { neg_resp(resp, SID_EXIT_TRANSFER, NRC_COND_NOT_OK); *rlen = 3; return; }
    s_downloading = 0;
    pos_resp(resp, SID_EXIT_TRANSFER, NULL, 0); *rlen = 1;
}

static void do_read_did(u8 *req, u8 *resp, u8 *rlen)
{
    u16 did = ((u16)req[1] << 8) | req[2];
    if (did == 0xF000 || did == 0xF001) {
        resp[0] = SID_READ_DID | SID_POS_RESP;
        resp[1] = (u8)(did >> 8); resp[2] = (u8)did;
        resp[3] = 0x01; resp[4] = 0x00;
        *rlen = 5;
    } else {
        neg_resp(resp, SID_READ_DID, NRC_OUT_OF_RANGE); *rlen = 3;
    }
}

/* ============ 主循环 ============ */
int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <can-if> <out.bin> [--verbose]\n", argv[0]);
        return 1;
    }
    const char *ifname  = argv[1];
    const char *outpath = argv[2];
    g_verbose = (argc > 3 && strcmp(argv[3], "--verbose") == 0);

    crc_init();
    memset(sim_flash, 0xFF, sizeof(sim_flash));

    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) { perror("socket"); return 1; }
    struct ifreq ifr;
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) { perror("ioctl"); return 1; }
    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }

    printf("[mock] listening on %s, waiting for UDS requests...\n", ifname);

    struct can_frame f;
    for (;;) {
        ssize_t n = read(s, &f, sizeof(f));
        if (n < 0) { if (errno == EINTR) continue; perror("read"); break; }
        if (n < (ssize_t)sizeof(f)) continue;
        if ((f.can_id & CAN_EFF_MASK) != CAN_ID_UDS_REQ) continue;

        u8 resp[8] = {0}, rlen = 0;
        u8 sid = f.data[0];
        u8 jump = 0;

        switch (sid) {
        case SID_DIAG_SESSION:  do_session(f.data, resp, &rlen); break;
        case SID_SEC_ACCESS:    do_security(f.data, resp, &rlen); break;
        case SID_RUTINE_CTRL:   do_routine(f.data, resp, &rlen); break;
        case SID_REQ_DOWNLOAD:  do_download(f.data, resp, &rlen); break;
        case SID_TRANS_DATA:    do_transfer(f.data, f.can_dlc, resp, &rlen); break;
        case SID_EXIT_TRANSFER: do_exit(resp, &rlen); break;
        case SID_READ_DID:      do_read_did(f.data, resp, &rlen); break;
        case SID_ECU_RESET:
            if (f.data[1] == 0x01) {
                resp[0] = SID_ECU_RESET | SID_POS_RESP; rlen = 1; jump = 1;
            } else {
                neg_resp(resp, SID_ECU_RESET, NRC_OUT_OF_RANGE); rlen = 3;
            }
            break;
        default:
            neg_resp(resp, sid, NRC_OUT_OF_RANGE); rlen = 3;
            break;
        }

        if (rlen > 0) {
            struct can_frame rf;
            memset(&rf, 0, sizeof(rf));
            rf.can_id = CAN_ID_UDS_RES;
            rf.can_dlc = rlen;
            memcpy(rf.data, resp, rlen);
            write(s, &rf, sizeof(rf));
        }

        if (jump) {
            FILE *fp = fopen(outpath, "wb");
            if (fp) {
                fwrite(sim_flash, 1, s_dl_total, fp);
                fclose(fp);
            }
            printf("[mock] got ECU reset, wrote %u bytes to %s\n", s_dl_total, outpath);
            printf("MOCK_BOOT_DONE\n");
            fflush(stdout);
            close(s);
            return 0;
        }
    }
    close(s);
    return 1;
}
