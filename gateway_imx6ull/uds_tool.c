/* uds_tool.c - i.MX6ULL UDS 上位机（阶段 9）
 *
 * 功能：读取 .bin 固件，通过 CAN 按 UDS 协议下发到 STM32 Bootloader，
 *       完成擦除→传输→校验→复位跳转的全流程 OTA 升级。
 *
 * 编译：
 *   gcc uds_tool.c -o uds_tool                      # 本机(x86)测试
 *   arm-linux-gnueabihf-gcc uds_tool.c -o uds_tool  # 交叉编译
 *
 * 运行（先配好 can0）：
 *   ./uds_tool app.bin [can0]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#define CAN_ID_UDS_REQ   0x110   /* Boot 区 UDS 请求（仅在 Boot 里生效） */
#define CAN_ID_UDS_RES   0x120   /* Boot/App 诊断响应共用同一 ID */
#define CAN_ID_DIAG_REQ  0x130   /* App 侧诊断桩：仅 0x22(读版本)/0x11(复位) */
#define BLOCK_LEN        6       /* Data[0]=SID,Data[1]=块序号,Data[2..7]=数据，与 boot_uds.h 一致 */
#define MAX_BLOCK_RETRY  3       /* 同一块连续无确认重试次数，超过则放弃升级 */
#define RES_TIMEOUT_MS   2000
/* 擦除 App 区是 Boot 端同步阻塞调用 HAL_FLASHEx_Erase，248 页(496KB/2KB)
   逐页物理擦除，STM32F103 高密度器件每页约 20~40ms，加起来可能要 5~10
   秒——固定 2 秒超时会把"Boot 正在正常擦，只是还没擦完"误判成失败，
   这一步单独给一个大得多的超时 */
#define ERASE_TIMEOUT_MS 15000
#define PROBE_TIMEOUT_MS 300     /* 探测用短超时：没收到就当作已在 Boot */
/* App 复位后到 Boot 起来的等待时间。
   注：曾经以为"第一次升级必失败、第二次才成功"是这个值给小了，把它从
   1000 调到 2000 也没解决——真正的原因在 Boot 侧：3 秒握手窗口里收到
   0x10 02 之后只是 break 进 uds_loop，那一帧已经被从 CAN FIFO 取走却
   从来没有应答过，上位机只能等到超时；重跑一次时 Boot 已经在 uds_loop
   里了，才会正常回复。已在 boot_main.c 的握手窗口里就地应答修掉。
   这个值本身按"复位到 Boot CAN 初始化完成"留裕量，1000 足够，保留 2000
   纯粹是不差这一秒。 */
#define RESET_WAIT_MS    2000
#define DIAG_DID_APP_VERSION 0xF000

/* 单个 A/B 槽的大小，必须跟 firmware_stm32/fw_meta.h 的 SLOT_SIZE 一致
   （Slot A 在 0x08004000，Slot B 在 0x08040000，各 240KB）。
   这里不 include 那个头文件：它是 STM32 侧的裸机代码，带一堆 HAL 依赖，
   为了一个常量把它拉进上位机工具不划算。代价是改分区布局时两处要一起改，
   所以两边注释里都写明了对应关系。 */
#define SLOT_SIZE_BYTES  (240 * 1024L)

/* 交互终端里跑，显示带动画的进度条（\r 原地刷新，好看）；被 ota_service.c
   用 popen 调起来时，标准输出接的是管道不是 tty，这时候退回每行一个百分比
   的老格式（\n 换行），因为那边靠按行读取来解析进度、转发到 MQTT——这两种
   场景的输出需求是冲突的，用 isatty() 在运行时自动判断该用哪种 */
static int g_is_tty = 0;

/* ============ CRC32（与 Boot 侧算法一致） ============ */
static uint32_t crc32_table[256];
static void crc32_init(void)
{
    uint32_t i, j, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
}
static uint32_t crc32_buf(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    while (len--) crc = (crc >> 8) ^ crc32_table[(crc ^ *data++) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

/* ============ XTEA 加密（与 Boot 侧 uds.c 算法完全一致） ============ */
static const uint32_t sec_key[4] = { 0x4D474E54u, 0x45444F4Du, 0x41545741u, 0x592B2B2Bu };

static void xtea_encipher(uint32_t *v, const uint32_t *key)
{
    uint32_t v0 = v[0], v1 = v[1], sum = 0;
    uint32_t delta = 0x9E3779B9u;
    int i;
    for (i = 0; i < 32; i++) {
        v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3]);
        sum += delta;
        v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3]);
    }
    v[0] = v0; v[1] = v1;
}

/* 由种子计算密钥：与 Boot 侧 UDS_CalcKey 完全一致 */
static uint32_t uds_calc_key(uint32_t seed)
{
    uint8_t blk[8];
    uint32_t key;
    blk[0] = (uint8_t)(seed >> 24); blk[1] = (uint8_t)(seed >> 16);
    blk[2] = (uint8_t)(seed >> 8);  blk[3] = (uint8_t)seed;
    blk[4] = 0; blk[5] = 0; blk[6] = 0; blk[7] = 0;
    xtea_encipher((uint32_t *)blk, sec_key);
    key = ((uint32_t)blk[0] << 24) | ((uint32_t)blk[1] << 16) |
          ((uint32_t)blk[2] << 8)  | (uint32_t)blk[3];
    return key;
}

/* ============ SocketCAN 打开 ============ */
static int open_can(const char *ifname)
{
    int s;
    struct sockaddr_can addr;
    struct ifreq ifr;

    s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) { perror("socket"); return -1; }
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) { perror("ioctl"); return -1; }
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return -1; }
    return s;
}

/* ============ 发送请求并等待响应（req_id/timeout 可变，供 UDS 和诊断桩共用） ============ */
/* 返回 0=正响应, -1=超时, -2=负响应(NRC 存在 resp[2]) */
static int send_req_to(int s, uint32_t req_id, int timeout_ms,
                       const uint8_t *data, uint8_t len, uint8_t *resp, int *rlen)
{
    struct can_frame f;
    struct pollfd pfd;
    int n;

    memset(&f, 0, sizeof(f));
    f.can_id = req_id;
    f.can_dlc = len;
    memcpy(f.data, data, len);
    if (write(s, &f, sizeof(f)) < 0) { perror("write can"); return -1; }

    /* 等待响应（带超时） */
    pfd.fd = s;
    pfd.events = POLLIN;
    n = poll(&pfd, 1, timeout_ms);
    if (n <= 0) return -1;

    if (read(s, &f, sizeof(f)) < 0) { perror("read can"); return -1; }
    if (f.can_id != CAN_ID_UDS_RES) return -1;

    *rlen = f.can_dlc;
    memcpy(resp, f.data, f.can_dlc);
    if (resp[0] == 0x7F) {
        printf("  [NEG] SID=0x%02X NRC=0x%02X\n", resp[1], resp[2]);
        return -2;
    }
    return 0;
}

static int send_req(int s, const uint8_t *data, uint8_t len, uint8_t *resp, int *rlen)
{
    int rc = send_req_to(s, CAN_ID_UDS_REQ, RES_TIMEOUT_MS, data, len, resp, rlen);
    if (rc == -1) printf("  [timeout] no response\n");
    return rc;
}

/* 通用请求助手：打印 SID 名 */
static int do_req(int s, const char *name, const uint8_t *data, uint8_t len,
                  uint8_t *resp, int *rlen)
{
    int rc = send_req(s, data, len, resp, rlen);
    if (rc == 0) printf("  [OK ] %s\n", name);
    else         printf("  [ERR] %s\n", name);
    return rc;
}

/* 自定义超时版：目前只有 0x31 擦除 Flash 用得到 */
static int do_req_timeout(int s, const char *name, int timeout_ms,
                          const uint8_t *data, uint8_t len, uint8_t *resp, int *rlen)
{
    int rc = send_req_to(s, CAN_ID_UDS_REQ, timeout_ms, data, len, resp, rlen);
    if (rc == -1) printf("  [timeout] no response\n");
    if (rc == 0) printf("  [OK ] %s\n", name);
    else         printf("  [ERR] %s\n", name);
    return rc;
}

/* ============ 升级前状态探测（原方案书里描述过、此前一直没实现的那一步） ============
 * 1. 向 0x130 发 0x22 读 App 版本 DID：
 *      - 有响应 → 当前在 App → 发 0x11 请求复位，等 App 软复位、Boot 起来
 *      - 无响应（超时）→ 大概率已在 Boot 等待升级 → 直接往下走
 * 2. 这一步只用短超时（PROBE_TIMEOUT_MS），避免设备已在 Boot 时白等 2 秒 */
static void probe_and_prepare(int s)
{
    uint8_t req[8], resp[8];
    int rlen, rc;

    req[0] = 0x22; req[1] = (uint8_t)(DIAG_DID_APP_VERSION >> 8); req[2] = (uint8_t)DIAG_DID_APP_VERSION;
    rc = send_req_to(s, CAN_ID_DIAG_REQ, PROBE_TIMEOUT_MS, req, 3, resp, &rlen);
    if (rc != 0) {
        printf("[probe] 无 App 诊断响应，判定已在 Boot，直接进入升级流程\n");
        return;
    }
    printf("[probe] 检测到节点正在运行 App（version=%u.%u），请求复位到 Boot\n",
           resp[3], resp[4]);

    req[0] = 0x11;
    send_req_to(s, CAN_ID_DIAG_REQ, PROBE_TIMEOUT_MS, req, 1, resp, &rlen);
    /* App 复位这条请求即便没收到确认响应也继续——复位本身会打断通信是正常现象 */
    usleep(RESET_WAIT_MS * 1000);
}

/* 按目标槽解析固件路径。
   传入具体文件就直接用（兼容单槽模式、也方便手动指定）；传入基名则拼成
   <base>_a.bin / <base>_b.bin —— A/B 两个槽的固件链接地址不同，是两份
   不能互换的文件，必须按槽选对。 */
static int resolve_fw_path(const char *arg, int slot, char *out, size_t outlen)
{
    struct stat st;

    if (stat(arg, &st) == 0 && S_ISREG(st.st_mode)) {
        snprintf(out, outlen, "%s", arg);
        if (slot >= 0) {
            /* 文件名里带 _a/_b 时顺手核对一下，选错槽是很难从现象上看出来的
               （固件传完了、CRC 也对，就是启动不起来然后被回滚） */
            const char *base = strrchr(arg, '/');
            base = base ? base + 1 : arg;
            char want = (slot == 0) ? 'a' : 'b';
            const char *tag = strstr(base, "_a.bin");
            if (!tag) tag = strstr(base, "_b.bin");
            if (tag && tag[1] != want) {
                printf("  [警告] 设备要写 Slot %c，但指定的是 %s —— 槽号看起来不匹配\n",
                       'A' + slot, base);
            }
        }
        return 0;
    }

    if (slot < 0) {
        /* 传了基名、设备又不支持 A/B 查询。最常见的原因不是"文件放错了"，
           而是板子上的 Boot 还是不含 A/B 的旧版本——直接把这个可能性写出来，
           省得对着"找不到文件"往文件路径方向查 */
        fprintf(stderr,
            "找不到固件文件 %s\n"
            "  设备的 Boot 不支持 A/B 查询（0x22 F002 返回负响应），可能原因：\n"
            "    1) 板子上的 Boot 还是旧版本，需要先烧写含 A/B 功能的 Bootloader\n"
            "    2) 确实要按单槽模式升级 —— 那就直接给出具体的 .bin 路径，别用基名\n", arg);
        return -1;
    }
    snprintf(out, outlen, "%s_%c.bin", arg, (slot == 0) ? 'a' : 'b');
    if (stat(out, &st) != 0) {
        fprintf(stderr, "找不到固件文件 %s\n"
                        "（A/B 模式需要两份固件：%s_a.bin 和 %s_b.bin，"
                        "分别链接到 0x08004000 和 0x08040000）\n", out, arg, arg);
        return -1;
    }
    return 0;
}

/* ============ 主流程 ============ */
int main(int argc, char *argv[])
{
    /* v3: 行缓冲，不管 stdout 接的是终端还是管道都一样每行自动 flush。
       之前只在传输循环里手动 fflush 了一次，"[OK]/[ERR]"这些每步状态
       行没有 flush——命令行直接跑没问题（进程退出前终归会冲出来），
       但被 ota_service 用管道包起来实时解析进度时，libc 对非终端输出默认
       全缓冲，这些行会在管道里憋到缓冲区满或进程退出才一次性冒出来，
       "实时进度"就变成了"结束时刷屏"。 */
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
    g_is_tty = isatty(fileno(stdout));

    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s <firmware> [can0]\n"
            "  firmware 可以是：\n"
            "    基名   如 atk_f103   → 自动按目标槽选 atk_f103_a.bin / atk_f103_b.bin\n"
            "    具体文件 如 xx_b.bin → 直接用该文件（需自行确保槽匹配）\n", argv[0]);
        return 1;
    }
    const char *fw_arg = argv[1];
    const char *can_if  = (argc > 2) ? argv[2] : "can0";

    /* 打开 CAN */
    int s = open_can(can_if);
    if (s < 0) return 1;
    printf("CAN %s ready\n", can_if);

    uint8_t req[8], resp[8];
    int rlen;
    int rc = 0;
    /* fw 必须在任何 goto fail 之前就初始化：fail 标签处会 free(fw)，
       而早期失败（比如编程会话没进去）时固件根本还没读进来，
       free 一个未初始化的指针会直接搞坏堆 */
    uint8_t *fw = NULL;
    long fsize = 0;

    /* 0. 升级前状态探测：App 在跑就先请求复位到 Boot，已在 Boot 就直接走流程 */
    probe_and_prepare(s);

    /* 1. 进入编程会话 */
    req[0] = 0x10; req[1] = 0x02;
    rc = do_req(s, "0x10 编程会话", req, 2, resp, &rlen);
    if (rc) goto fail;

    /* 1.5 查询 A/B 状态，决定这次该传哪份固件。
       必须在读固件之前问：direct-xip 模式下 Slot A / Slot B 的固件链接
       地址不同，是两个不能互换的 bin，选错了传上去就是个起不来的固件
       （虽然会被 A/B 回滚机制救回来，但白白浪费一次升级） */
    char fw_path[512];
    int target_slot = -1;
    req[0] = 0x22; req[1] = 0xF0; req[2] = 0x02;
    if (send_req(s, req, 3, resp, &rlen) == 0 && rlen >= 6) {
        target_slot = resp[5];
        printf("  [OK ] 0x22 A/B 状态: active=%c pending=%s target=%c attempts=%u\n",
               'A' + resp[3],
               (resp[4] == 0xFF) ? "无" : (resp[4] == 0 ? "A" : "B"),
               'A' + resp[5], resp[6]);
    } else {
        /* 老版本 Boot 不认识这个 DID。此时按单槽模式走，直接用传入的文件，
           保证新工具能给旧设备升级（升级路径本身不能因为工具更新而断掉） */
        printf("  [--] 0x22 A/B 状态: 设备不支持（老版本 Boot），按单槽模式处理\n");
    }

    if (resolve_fw_path(fw_arg, target_slot, fw_path, sizeof(fw_path)) != 0) goto fail;

    /* 读固件 */
    FILE *fp = fopen(fw_path, "rb");
    if (!fp) { perror("open bin"); goto fail; }
    fseek(fp, 0, SEEK_END);
    fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    /* 上限必须跟【槽的实际大小】一致。原来写死的是 64KB，而 A/B 分区每个槽
       是 240KB——当前固件约 62KB，刚好卡在这个上限边缘没暴露出来。App 再
       加点功能超过 64KB，OTA 就会拒绝升级，还报一句 "size invalid"，
       让人以为是固件文件坏了，实际上文件完全正常、只是这行常量过时了。
       这类"今天恰好没事、明天必然出事"的硬编码比直接的 bug 更难查——
       出问题时改动的是别处，没人会想到回头看这一行。 */
    if (fsize <= 0 || fsize > SLOT_SIZE_BYTES) {
        fprintf(stderr, "firmware size %ld invalid（需在 1 ~ %ld 字节之间，"
                        "即单个 A/B 槽的容量）\n", fsize, SLOT_SIZE_BYTES);
        fclose(fp);
        goto fail;
    }
    fw = malloc(fsize);
    if (!fw) { fclose(fp); goto fail; }
    if (fread(fw, 1, fsize, fp) != (size_t)fsize) {
        fprintf(stderr, "read fail\n"); fclose(fp); free(fw); goto fail;
    }
    fclose(fp);

    crc32_init();
    uint32_t fw_crc = crc32_buf(fw, fsize);
    printf("firmware: %s  size=%ld  crc32=0x%08X\n", fw_path, fsize, fw_crc);

    /* 2. 安全访问：请求种子 → 发送密钥 */
    req[0] = 0x27; req[1] = 0x01;
    rc = do_req(s, "0x27 请求种子", req, 2, resp, &rlen);
    if (rc) goto fail;
    /* 必须先确认响应真的带够了 4 字节种子再去读。do_req 只保证"收到了肯定
       响应"，不保证长度——DLC 短一点的话下面就在读 resp 数组里没被写过的
       部分（栈上的残留），算出来的密钥自然对不上，而报错会停在后面的
       "0x27 发送密钥失败"上，把人往"密钥算法写错了"的方向带。
       实际原因是上一步的响应就不完整。 */
    if (rlen < 6) {
        fprintf(stderr, "0x27 种子响应长度异常（%d 字节，至少要 6），"
                        "对端固件版本对不上？\n", rlen);
        goto fail;
    }
    uint32_t seed = ((uint32_t)resp[2] << 24) | ((uint32_t)resp[3] << 16) |
                    ((uint32_t)resp[4] << 8)  | ((uint32_t)resp[5]);
    uint32_t key = uds_calc_key(seed);   /* v2: XTEA(seed)，非简单取反 */
    req[0] = 0x27; req[1] = 0x02;
    req[2] = (uint8_t)(key >> 24); req[3] = (uint8_t)(key >> 16);
    req[4] = (uint8_t)(key >> 8);  req[5] = (uint8_t)key;
    rc = do_req(s, "0x27 发送密钥", req, 6, resp, &rlen);
    if (rc) goto fail;

    /* 3. 擦除 App 区（同步阻塞操作，给足够长的超时） */
    req[0] = 0x31; req[1] = 0x01; req[2] = 0xFF; req[3] = 0x00;
    rc = do_req_timeout(s, "0x31 擦除Flash", ERASE_TIMEOUT_MS, req, 4, resp, &rlen);
    if (rc) goto fail;

    /* 4. 请求下载（长度；完整 CRC32 在第 7 步用 0x31 单独传，8 字节帧装不下，
          不再在这里塞一份从未被校验用到的截断 CRC，Data[5..7] 明确保留为 0） */
    req[0] = 0x34;
    req[1] = (uint8_t)(fsize >> 24); req[2] = (uint8_t)(fsize >> 16);
    req[3] = (uint8_t)(fsize >> 8);  req[4] = (uint8_t)(fsize);
    req[5] = 0; req[6] = 0; req[7] = 0;
    rc = do_req(s, "0x34 请求下载", req, 8, resp, &rlen);
    if (rc) goto fail;
    printf("  block length=%d\n", (rlen >= 3) ? ((resp[1] << 8) | resp[2]) : BLOCK_LEN);

    /* 5. 分块传输：带块序号 + 丢帧重传（同一块连续 3 次无确认才放弃，
          之前版本没有序号字段，这里是真正按方案书描述实现的部分） */
    if (!g_is_tty) printf("transfer: ");
    long sent = 0;
    uint8_t seq = 0;
    while (sent < fsize) {
        int nb = (fsize - sent < BLOCK_LEN) ? (int)(fsize - sent) : BLOCK_LEN;
        req[0] = 0x36;
        req[1] = seq;
        memcpy(&req[2], fw + sent, nb);

        int retry, ok = 0;
        for (retry = 0; retry < MAX_BLOCK_RETRY; retry++) {
            rc = send_req(s, req, 2 + nb, resp, &rlen);
            if (rc == 0 && rlen >= 2 && resp[1] == seq) { ok = 1; break; }
            printf("\n  [retry %d/%d] block seq=%u @%ld", retry + 1, MAX_BLOCK_RETRY, seq, sent);
        }
        if (!ok) {
            printf("\n  [ERR] block seq=%u @%ld: 连续 %d 次无确认，放弃升级\n",
                   seq, sent, MAX_BLOCK_RETRY);
            goto fail;
        }

        sent += nb;             /* 按实际发出字节数推进，避免末块把 sent 冲过 fsize */
        seq++;
        if (sent % (BLOCK_LEN * 100) == 0 || sent >= fsize) {
            int pct = (int)(sent * 100 / fsize);
            if (g_is_tty) {
                /* \r 原地刷新一条进度条，不是每次都真的换行——这条不走
                   _IOLBF 的自动 flush（那个只在遇到 \n 时触发），手动
                   flush 一下才能让终端立刻看到更新 */
                int filled = pct / 2, k;
                printf("\rtransfer: [");
                for (k = 0; k < 50; k++) putchar(k < filled ? '=' : ' ');
                printf("] %3d%%", pct);
                fflush(stdout);
            } else {
                /* v3: 百分比后面带换行，配合行缓冲，ota_service 才能按行
                   实时解析进度 */
                printf("%d%%\n", pct);
            }
        }
    }
    if (g_is_tty) printf("\n");
    printf("done\n");

    /* 6. 退出传输 */
    req[0] = 0x37;
    rc = do_req(s, "0x37 退出传输", req, 1, resp, &rlen);
    if (rc) goto fail;

    /* 7. CRC 校验 */
    req[0] = 0x31; req[1] = 0x01; req[2] = 0xFF; req[3] = 0x01;
    req[4] = (uint8_t)(fw_crc >> 24); req[5] = (uint8_t)(fw_crc >> 16);
    req[6] = (uint8_t)(fw_crc >> 8);  req[7] = (uint8_t)(fw_crc);
    rc = do_req(s, "0x31 CRC校验", req, 8, resp, &rlen);
    if (rc) goto fail;

    /* 8. 复位跳转 App */
    req[0] = 0x11; req[1] = 0x01;
    rc = do_req(s, "0x11 ECU复位", req, 2, resp, &rlen);
    if (rc) goto fail;

    printf("\n=== Upgrade SUCCESS, jumping to App ===\n");
    free(fw);
    close(s);
    return 0;

fail:
    printf("\n=== Upgrade FAILED ===\n");
    free(fw);
    close(s);
    return 1;
}
