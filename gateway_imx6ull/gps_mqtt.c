/**
 * gps_mqtt.c - RTK/GNSS NMEA 解析 + MQTT 发布
 *
 * 硬件：OEM700（和芯星通 UM982 + 内置 Ntrip Client），USB 接到 i.MX6ULL。
 *       USB 枚举出 4 个 CDC-ACM 口：
 *         ttyACM0  静默
 *         ttyACM1  OEM700 自身日志（LuatOS，也是 Lua REPL）
 *         ttyACM2  RTCM 原始观测量（二进制）
 *         ttyACM3  **NMEA 输出**  ← 本程序读这个
 *       波特率固定 115200。
 *
 * 用法：
 *   ./gps_mqtt [串口]          默认 /dev/ttyACM3
 *
 * 编译：
 *   $CC gps_mqtt.c -o gps_mqtt -I mosquitto-1.6.15/lib -L mosquitto-1.6.15/lib -lmosquitto
 *
 * ── 相比上一版的三处实质修改 ──
 *
 * 1. **talker ID 不能写死 GP。**
 *    上一版只匹配 $GPGGA / $GPRMC，而多星座接收机发的是 $GNGGA / $GNRMC
 *    （GN = 混合解算），北斗单独的是 $GB、伽利略 $GA、格洛纳斯 $GL。
 *    接上 OEM700 之后一条都匹配不上，表现是"程序在跑、日志干净、就是没数据"。
 *    现在只比对后三个字母。
 *
 * 2. **没有定位时也要发。**
 *    上一版 `if (fix > 0)` 才发布——丢星之后地图上会永远停在最后一个点，
 *    和"车停在那里"看起来一模一样。这正是这个项目一直在防的：
 *    **故障不能伪装成正常**。现在无论有没有解都按 1Hz 发，fix=0 让前端
 *    显示成"无定位"而不是留着旧点。
 *
 * 3. **GGA / RMC / GSA 合并成一份状态再发。**
 *    上一版收到 GGA 发一次（speed 填 0）、收到 RMC 又发一次（sat/alt 填 0），
 *    前端拿到的字段在两种残缺形态之间反复横跳。现在攒齐一个历元再发。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <time.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <mosquitto.h>

#define MQTT_HOST_DEFAULT "localhost"   /* 实际以 MQTT_HOST 环境变量为准 */
#define MQTT_PORT   1883
#define DEV_DEFAULT "robot01"
#define TTY_DEFAULT "/dev/ttyACM3"

static struct mosquitto *g_mosq = NULL;
static char g_tp_gps[128];

/* 串口 fd。NTRIP 线程要往里写 RTCM，所以提成全局。
   -1 表示串口当前没开（设备被拔了、还在重试）——写之前必须判。 */
static volatile int g_fd = -1;
/* 最近一条原始 GGA。NTRIP 上行要用它告诉基准站"我在哪"，
   网络 RTK(VRS/FKP) 靠它生成虚拟基站，不发就永远只有单点解。 */
static char g_last_gga[128] = "";
static pthread_mutex_t g_gga_lock = PTHREAD_MUTEX_INITIALIZER;
/* 收到的 RTCM 字节数和最近一次收到的时刻，用来在前端显示差分链路是否活着 */
static volatile unsigned long g_rtcm_bytes = 0;
static volatile long g_rtcm_last = 0;

/* ============ 当前历元的解算状态 ============
 * NMEA 一个历元会分成好几条语句发（GGA 带定位质量/卫星数/高程，
 * RMC 带速度/航向/日期，GSA 带 HDOP/PDOP）。攒齐再发，前端才拿得到
 * 一份自洽的数据。 */
typedef struct {
    int    valid;          /* 本历元 GGA 是否解析过 */
    double lat, lon;
    int    fix;            /* GGA 第 6 字段，见 fix_text() */
    int    sat;            /* 参与解算的卫星数 */
    double alt;            /* 海拔 m */
    double hdop;
    double speed;          /* km/h */
    double course;         /* 航向角 度 */
    double diff_age;       /* 差分龄期 s，RTK 时用来判断差分链路是否新鲜 */
} GnssState;

static GnssState g_st;

/* GGA 定位质量。RTK 的两个解是 4 和 5，必须分开显示：
 *   4 固定解 = 厘米级，5 浮动解 = 分米级，差一个数量级。
 *   全都笼统显示成"已定位"，等于把 RTK 最重要的信息扔了。 */
static const char *fix_text(int q)
{
    switch (q) {
    case 0: return "无定位";
    case 1: return "单点";
    case 2: return "差分";
    case 4: return "RTK固定";
    case 5: return "RTK浮动";
    case 6: return "推算";
    default: return "未知";
    }
}

/* ============ NMEA 工具 ============ */

/* ddmm.mmmm → 十进制度。度的位数不固定（纬度 2 位、经度 3 位），
   所以按小数点位置反推，不能写死。 */
static double nmea_to_deg(const char *s)
{
    const char *dot = strchr(s, '.');
    int deg_digits, i;
    double deg = 0, min = 0;
    char buf[16];

    if (!s || !*s || !dot) return 0.0;
    deg_digits = (int)(dot - s) - 2;
    if (deg_digits <= 0 || deg_digits > 3) return 0.0;

    for (i = 0; i < deg_digits && i < 15; i++) buf[i] = s[i];
    buf[i] = '\0';
    deg = atof(buf);
    min = atof(s + deg_digits);
    return deg + min / 60.0;
}

/* 按逗号切分。字段可以为空（NMEA 里空字段很常见，比如无定位时的 GGA）。 */
static int split_nmea(char *line, char f[24][24])
{
    int n = 0;
    char *p = line;
    char *start = p;

    while (*p && n < 24) {
        if (*p == ',' || *p == '*') {
            int len = (int)(p - start);
            if (len > 23) len = 23;
            memcpy(f[n], start, len);
            f[n][len] = '\0';
            n++;
            if (*p == '*') break;
            start = p + 1;
        }
        p++;
    }
    if (n < 24 && *start && *p == '\0') {
        int len = (int)(p - start);
        if (len > 23) len = 23;
        memcpy(f[n], start, len);
        f[n][len] = '\0';
        n++;
    }
    return n;
}

/* 只看后三个字母，忽略 talker ID（GP/GN/GB/GA/GL 都要认） */
static int is_sentence(const char *field0, const char *type3)
{
    size_t len = strlen(field0);
    if (len < 6) return 0;                       /* $ + 2 talker + 3 type */
    return strncmp(field0 + len - 3, type3, 3) == 0;
}

static long now_sec(void);   /* 定义在 NTRIP 一节，发布函数要用 */

/* ============ 发布 ============ */
static void publish_gps(void)
{
    char payload[320];

    snprintf(payload, sizeof(payload),
             "{\"lat\":%.7f,\"lon\":%.7f,\"fix\":%d,\"fixText\":\"%s\","
             "\"sat\":%d,\"alt\":%.2f,\"hdop\":%.2f,"
             "\"speed\":%.2f,\"course\":%.1f,\"diffAge\":%.1f,"
             "\"rtcm\":%lu,\"rtcmAge\":%ld}",
             g_st.lat, g_st.lon, g_st.fix, fix_text(g_st.fix),
             g_st.sat, g_st.alt, g_st.hdop,
             g_st.speed, g_st.course, g_st.diff_age,
             /* rtcmAge = -1 表示"从来没收到过差分数据"，和"收到过但断了"
                是两种完全不同的故障：前者查账号/挂载点，后者查网络。 */
             g_rtcm_bytes,
             g_rtcm_last ? (now_sec() - g_rtcm_last) : -1L);

    mosquitto_publish(g_mosq, NULL, g_tp_gps, strlen(payload), payload, 0, false);

    /* 经纬度打 7 位小数：RTK 固定解是厘米级，而第 6 位小数约等于 0.1m、
       第 7 位约 1cm。打 6 位等于在日志里就把精度截掉了。 */
    printf("[gps] %s sat=%d hdop=%.2f %.7f,%.7f alt=%.2f age=%.1fs\n",
           fix_text(g_st.fix), g_st.sat, g_st.hdop,
           g_st.lat, g_st.lon, g_st.alt, g_st.diff_age);
    fflush(stdout);
}

/* ============ 处理一行 NMEA ============ */
static void handle_line(char *line)
{
    char f[24][24];
    int n;
    if (line[0] != '$') return;
    n = split_nmea(line, f);
    if (n < 2) return;

    if (is_sentence(f[0], "GGA") && n >= 10) {
        g_st.fix = atoi(f[6]);
        g_st.sat = atoi(f[7]);
        g_st.hdop = atof(f[8]);
        g_st.alt = atof(f[9]);
        g_st.diff_age = (n >= 14) ? atof(f[13]) : 0.0;

        if (g_st.fix > 0 && f[2][0]) {
            g_st.lat = nmea_to_deg(f[2]);
            g_st.lon = nmea_to_deg(f[4]);
            if (f[3][0] == 'S') g_st.lat = -g_st.lat;
            if (f[5][0] == 'W') g_st.lon = -g_st.lon;
        } else {
            /* 丢解时把坐标清零，而不是留着上一次的。
               留着旧坐标 + fix=0 会让前端有机会"顺手"把旧点画出来；
               清零之后前端只能如实显示"无定位"。 */
            g_st.lat = g_st.lon = 0.0;
        }
        g_st.valid = 1;

        /* 存一份给 NTRIP 上行。无解时也存：网络 RTK 需要一个大致位置
           才能给出虚拟基站，而无解的 GGA 里其实常常已经有单点坐标了。 */
        pthread_mutex_lock(&g_gga_lock);
        snprintf(g_last_gga, sizeof(g_last_gga), "%s", line);
        pthread_mutex_unlock(&g_gga_lock);

        /* GGA 是一个历元的收尾语句，收到就发一次（约 1Hz） */
        publish_gps();

    } else if (is_sentence(f[0], "RMC") && n >= 9) {
        if (f[7][0]) g_st.speed  = atof(f[7]) * 1.852;   /* 节 → km/h */
        if (f[8][0]) g_st.course = atof(f[8]);

    } else if (is_sentence(f[0], "GSA") && n >= 17) {
        /* GSA 的 HDOP 比 GGA 的更常更新；有就用它覆盖 */
        if (f[16][0]) g_st.hdop = atof(f[16]);
    }
}


/* ==================== NTRIP 客户端 ====================
 *
 * 为什么放在板子上，而不用模块自带的 NTRIP：
 * UM982 内置了 NTRIP 客户端，但它要走**模块自己的 4G**，而那张卡一直卡在
 * `wait IP_READY` 注册不上。板子这边 WiFi 是通的、网关本来就联着 broker，
 * 让板子去拉 RTCM 再从串口喂给模块，模块的 4G 就整个不需要了——
 * 少一个必须工作的部件，就少一个会坏的地方。
 *
 * 为什么塞进 gps_mqtt 而不是单独一个进程：
 * 串口只有一个。两个进程同时打开同一个 tty，读的那个会随机丢掉半行 NMEA
 * （内核把输入分给谁没有保证），而且谁也不知道另一个还在不在。
 * 这里读写都在一个进程内，RTCM 写、NMEA 读共用同一个 fd，没有竞争。
 *
 * 协议：NTRIP 1.0，本质是一个不会结束的 HTTP GET。
 * 服务端回 "ICY 200 OK" 之后，剩下的字节流就是 RTCM3，原样转给模块即可——
 * 我们**不解析 RTCM**：解析它一点好处都没有，模块才是需要它的人，
 * 中间多一道解析只会多一处可能改坏数据的地方。
 */

static const char *B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const char *in, char *out, size_t outsz)
{
    size_t n = strlen(in), i, j = 0;
    for (i = 0; i < n && j + 4 < outsz; i += 3) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < n) v |= (unsigned char)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned char)in[i + 2];
        out[j++] = B64[(v >> 18) & 63];
        out[j++] = B64[(v >> 12) & 63];
        out[j++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        out[j++] = (i + 2 < n) ? B64[v & 63]        : '=';
    }
    out[j] = '\0';
}

static long now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static int tcp_connect(const char *host, int port)
{
    struct addrinfo hints, *res = NULL, *p;
    char portstr[16];
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;              /* CORS 基本都是 IPv4 */
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;

    for (p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static void *ntrip_thread(void *arg)
{
    const char *host  = getenv("NTRIP_HOST");
    const char *mount = getenv("NTRIP_MOUNT");
    const char *user  = getenv("NTRIP_USER");
    const char *pass  = getenv("NTRIP_PASS");
    const char *ports = getenv("NTRIP_PORT");
    int port = (ports && ports[0]) ? atoi(ports) : 2101;
    char userpass[192], auth[256], req[512], buf[2048];
    int backoff = 2;

    (void)arg;
    if (!host || !host[0] || !mount || !mount[0]) {
        printf("[ntrip] 未配置 NTRIP_HOST/NTRIP_MOUNT，差分未启用（只会有单点解）\n");
        fflush(stdout);
        return NULL;
    }

    snprintf(userpass, sizeof(userpass), "%s:%s", user ? user : "", pass ? pass : "");
    b64_encode(userpass, auth, sizeof(auth));
    /* 账号密码只从环境变量来，绝不写进源码：这个仓库是要公开的，
       而 git 历史删不干净。 */
    memset(userpass, 0, sizeof(userpass));

    for (;;) {
        int sock, hdr_done = 0, ok = 0;
        long last_gga = 0, last_stat = 0;
        unsigned long stat_base = 0;

        sock = tcp_connect(host, port);
        if (sock < 0) {
            fprintf(stderr, "[ntrip] 连不上 %s:%d，%d 秒后重试\n", host, port, backoff);
            sleep(backoff);
            if (backoff < 60) backoff *= 2;   /* 退避：基准站维护时别每秒砸一次 */
            continue;
        }
        {   /* 挂载点名字里可能有 '/'，原样拼进去即可 */
            int kb = 1;
            setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &kb, sizeof(kb));
        }
        snprintf(req, sizeof(req),
                 "GET /%s HTTP/1.0\r\n"
                 "User-Agent: NTRIP EdgeMonitor/1.0\r\n"
                 /* 不发 Connection: close——NTRIP 的响应体是一条**永不结束**的
                    RTCM 流，声明 close 等于告诉服务端"发完这批就可以断"，
                    有的 caster 会照做，表现是每十几秒被掐一次、反复重连。 */
                 "Accept: */*\r\n"
                 "Authorization: Basic %s\r\n"
                 "Ntrip-Version: Ntrip/1.0\r\n\r\n",
                 mount, auth);
        if (write(sock, req, strlen(req)) < 0) { close(sock); sleep(2); continue; }

        /* 看门狗计时从"连上"这一刻算起。不置的话 g_rtcm_last 还是 0，
           now_sec()-0 是个巨大的数，连上第一秒就会判定"30 秒无数据"。 */
        g_rtcm_last = now_sec();
        printf("[ntrip] 已连接 %s:%d /%s\n", host, port, mount);
        fflush(stdout);

        for (;;) {
            fd_set rf;
            struct timeval tv;
            int n, r;

            FD_ZERO(&rf); FD_SET(sock, &rf);
            tv.tv_sec = 1; tv.tv_usec = 0;
            r = select(sock + 1, &rf, NULL, NULL, &tv);

            /* 上行 GGA：网络 RTK 要知道我们在哪才能生成虚拟基站。
               10 秒一次是通行做法——发太密基准站会嫌，太疏在移动中
               会离虚拟基站越来越远，精度掉。 */
            if (now_sec() - last_gga >= 10) {
                char gga[160];
                pthread_mutex_lock(&g_gga_lock);
                snprintf(gga, sizeof(gga), "%s", g_last_gga);
                pthread_mutex_unlock(&g_gga_lock);
                if (gga[0]) {
                    char out[176];
                    int len = snprintf(out, sizeof(out), "%s\r\n", gga);
                    if (write(sock, out, len) < 0) break;
                }
                last_gga = now_sec();
            }

            /* 30 秒报一次量。差分链路是"看不见"的——不打这行的话，
               现场只能靠"定位质量有没有变成 4"来反推，而那时候
               已经分不清是链路没数据、账号不对，还是天线收不到星。 */
            if (now_sec() - last_stat >= 30) {
                if (last_stat)
                    printf("[ntrip] 30s 内收到 %lu 字节（累计 %lu）\n",
                           g_rtcm_bytes - stat_base, g_rtcm_bytes);
                stat_base = g_rtcm_bytes;
                last_stat = now_sec();
                fflush(stdout);
            }

            if (r < 0) { if (errno == EINTR) continue; break; }
            if (r == 0) {
                /* 30 秒一个字节都没来：基准站那边多半已经悄悄断了。
                   TCP 不会主动告诉我们这件事——不设这个超时，
                   进程会永远"连着"一个死链路，而且看不出异常。 */
                if (ok && now_sec() - g_rtcm_last > 30) {
                    fprintf(stderr, "[ntrip] 30 秒无数据，重连\n");
                    break;
                }
                continue;
            }

            n = (int)read(sock, buf, sizeof(buf));
            if (n <= 0) break;

            if (!hdr_done) {
                /* 响应头和第一批 RTCM 可能在同一个包里，
                   必须找到空行的位置，把它之后的字节当数据。 */
                char *body = NULL;
                buf[(n < (int)sizeof(buf)) ? n : (int)sizeof(buf) - 1] = '\0';
                if (strncmp(buf, "ICY 200", 7) == 0 || strstr(buf, " 200 ")) {
                    ok = 1;
                } else {
                    fprintf(stderr, "[ntrip] 服务端拒绝：%.80s\n", buf);
                    break;      /* 401 之类，重连也没用，但退避后再试 */
                }
                body = strstr(buf, "\r\n\r\n");
                if (body) body += 4;
                else { body = strstr(buf, "\n\n"); if (body) body += 2; }
                hdr_done = 1;
                backoff = 2;
                if (body) {
                    int left = n - (int)(body - buf);
                    if (left > 0) {
                        g_rtcm_bytes += left;
                        g_rtcm_last = now_sec();
                        if (g_fd >= 0) (void)write(g_fd, body, left);
                    }
                }
                continue;
            }

            /* RTCM 原样转给模块。串口没开就直接丢——
               缓存起来等串口回来没有意义：差分数据几秒就过期了，
               喂一段陈旧的 RTCM 反而会让解算变差。 */
            /* 先记账再转发。计数器量的是**链路**是否在供数，
               不是串口写没写成——串口临时不在（设备被拔了、正在重开）
               时把链路也判成死的，会让看门狗白白重连，
               而且前端显示的故障方向是错的。 */
            g_rtcm_bytes += n;
            g_rtcm_last = now_sec();
            if (g_fd >= 0) (void)write(g_fd, buf, n);
        }

        close(sock);
        fprintf(stderr, "[ntrip] 链路断开，2 秒后重连（已收 %lu 字节）\n", g_rtcm_bytes);
        sleep(2);
    }
    return NULL;
}

/* ============ 串口 ============ */
static int open_serial(const char *dev)
{
    struct termios tio;
    int fd = open(dev, O_RDONLY | O_NOCTTY);
    if (fd < 0) return -1;

    memset(&tio, 0, sizeof(tio));
    if (tcgetattr(fd, &tio) != 0) { close(fd); return -1; }
    cfmakeraw(&tio);
    /* OEM700 外接口和 USB 透传口的波特率都固定 115200，手册第 2 节 */
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 10;      /* 1 秒超时：读不到也要回来，好让主循环有机会退出 */
    tcsetattr(fd, TCSANOW, &tio);
    tcflush(fd, TCIFLUSH);
    return fd;
}

/* ============ 找到 NMEA 那个口 ============
 *
 * UM982 是 4 口 CDC-ACM，只有其中一个吐 NMEA，其余是配置口/RTCM 口。
 * 写死 /dev/ttyACM3 有两个问题：
 *   ① 哪个口吐 NMEA 取决于模块内部配置，换一台就未必是 3；
 *   ② **重新插拔后编号会整体后移**——本来是 ACM0~3，拔了再插常常变成
 *      ACM4~7，因为旧的还没被完全回收。现场"插回去就是不出数据"，
 *      而设备明明枚举成功了，是最难查的一类问题。
 * 所以：挨个打开、听 2 秒，谁吐 $..GGA/$..RMC 就用谁。
 */
static int looks_like_nmea(int fd)
{
    char buf[512];
    int  waited = 0;

    while (waited < 2000) {
        int n = (int)read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            int i;
            buf[n] = '\0';
            for (i = 0; i + 6 < n; i++)
                if (buf[i] == '$' &&
                    (memcmp(buf + i + 3, "GGA", 3) == 0 ||
                     memcmp(buf + i + 3, "RMC", 3) == 0 ||
                     memcmp(buf + i + 3, "GSA", 3) == 0))
                    return 1;
        }
        usleep(100000);
        waited += 100;      /* read 有 VTIME，这里只是兜底节流 */
    }
    return 0;
}

static int find_nmea_port(char *out, size_t outsz)
{
    int i;
    for (i = 0; i < 12; i++) {
        char dev[32];
        int fd;
        snprintf(dev, sizeof(dev), "/dev/ttyACM%d", i);
        if (access(dev, R_OK | W_OK) != 0) continue;
        fd = open_serial(dev);
        if (fd < 0) continue;
        if (looks_like_nmea(fd)) {
            close(fd);
            snprintf(out, outsz, "%s", dev);
            printf("[gps] 自动识别到 NMEA 口：%s\n", dev);
            fflush(stdout);
            return 0;
        }
        close(fd);
    }
    return -1;
}

int main(int argc, char *argv[])
{
    const char *dev = (argc > 1) ? argv[1] : TTY_DEFAULT;
    char devbuf[32];
    const char *host = getenv("MQTT_HOST");
    const char *user = getenv("MQTT_USER");
    const char *pass = getenv("MQTT_PASS");
    const char *dev_id = getenv("DEVICE_ID");
    char client_id[64];
    char buf[512];
    char line[256];
    int  line_len = 0;

    if (!host || !host[0]) host = MQTT_HOST_DEFAULT;
    if (!dev_id || !dev_id[0]) dev_id = DEV_DEFAULT;
    snprintf(g_tp_gps, sizeof(g_tp_gps), "monitor/%s/data/gps", dev_id);
    snprintf(client_id, sizeof(client_id), "gps_%s", dev_id);

    mosquitto_lib_init();
    g_mosq = mosquitto_new(client_id, true, NULL);
    if (!g_mosq) { fprintf(stderr, "mosquitto_new failed\n"); return 1; }
    if (user && user[0]) mosquitto_username_pw_set(g_mosq, user, pass);

    /* 自动重连交给库：设备侧网络是会断的，断了要能自己回来 */
    if (mosquitto_connect(g_mosq, host, MQTT_PORT, 60) != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "[gps] 首次连接 %s:%d 失败，进入重连等待\n", host, MQTT_PORT);
    mosquitto_loop_start(g_mosq);

    printf("[gps] topic=%s  serial=%s\n", g_tp_gps, dev);

    {   /* NTRIP 拉差分。分离线程：它自己会无限重连，主线程不需要 join */
        pthread_t th;
        if (pthread_create(&th, NULL, ntrip_thread, NULL) == 0) pthread_detach(th);
        else fprintf(stderr, "[ntrip] 线程创建失败，差分未启用\n");
    }

    for (;;) {
        int i, n;

        /* 命令行给了设备就用给的（调试时要能指定）；
           没给就每轮重新找一次——设备可能刚插上、编号也可能变了。 */
        if (argc > 1) {
            snprintf(devbuf, sizeof(devbuf), "%s", argv[1]);
        } else if (find_nmea_port(devbuf, sizeof(devbuf)) != 0) {
            fprintf(stderr, "[gps] 没找到吐 NMEA 的串口，5 秒后重试\n");
            sleep(5);
            continue;
        }
        dev = devbuf;

        g_fd = open_serial(dev);
        if (g_fd < 0) {
            /* 串口打不开就等着重试，不退出：USB 设备可能被拔了再插，
               进程死掉的话守护脚本重拉也要一个周期，不如自己等。 */
            fprintf(stderr, "[gps] 打不开 %s (%s)，5 秒后重试\n", dev, strerror(errno));
            sleep(5);
            continue;
        }
        printf("[gps] serial opened\n");
        fflush(stdout);
        line_len = 0;

        for (;;) {
            n = (int)read(g_fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) continue;
                break;                      /* 设备掉了，回到外层重开 */
            }
            if (n == 0) continue;           /* VTIME 超时，正常 */

            for (i = 0; i < n; i++) {
                char c = buf[i];
                if (c == '\r' || c == '\n') {
                    if (line_len > 0) {
                        line[line_len] = '\0';
                        handle_line(line);
                        line_len = 0;
                    }
                } else if (line_len < (int)sizeof(line) - 1) {
                    line[line_len++] = c;
                } else {
                    line_len = 0;           /* 行超长，丢弃重来 */
                }
            }
        }

        fprintf(stderr, "[gps] 串口读取中断，重新打开\n");
        {   /* 先置 -1 再 close：NTRIP 线程随时可能往里写，
               顺序反了会有一瞬间写到已关闭（甚至已被别人复用）的 fd 上。 */
            int old_fd = g_fd;
            g_fd = -1;
            close(old_fd);
        }
        sleep(2);
    }

    /* 不会走到这里，留着是为了对称 */
    mosquitto_loop_stop(g_mosq, true);
    mosquitto_destroy(g_mosq);
    mosquitto_lib_cleanup();
    return 0;
}
