/**
 * mobile_ctrl.c - 手机端遥控服务（跑在 i.MX6ULL 上）
 *
 * 目标：**车一开机，手机连上同一个 WiFi 打开网页就能开**，不依赖服务器、
 * 不依赖 MQTT broker。
 *
 * ── 为什么不复用已有的 Web 大屏 ──
 * 大屏那条链路是 手机 → broker(VM) → MQTT → 网关 → CAN。中间任何一环
 * （VM 关机、NAT 转发没起、WiFi 换网段）断掉，车就遥控不了。
 * 而"车开着、人就在旁边、想让它动一下"这个场景，本来不该依赖机房里的东西。
 * 所以这个服务直接在板子上开 HTTP，收到指令直接写 CAN，**链路只有一跳**。
 *
 * 两条路径共存不冲突：SocketCAN 允许多个 writer，大屏走 MQTT 那条照常用。
 *
 * ── 死人开关（这个服务最重要的部分）──
 * 手机遥控和网页点按钮有个本质区别：**手机会息屏、会锁屏、会走出 WiFi 覆盖**。
 * 如果"前进"是一条一次性命令，那么手机一断，车就带着上一条命令一直往前开，
 * 直到撞上什么东西。
 *
 * 所以协议设计成**持续心跳式**：按住按钮期间网页每 200ms 发一次同样的指令，
 * 服务端只要 CTRL_TIMEOUT_MS 内没收到新指令，立刻下发停止。
 * 松手、息屏、断网、关页面——全都落到同一个安全行为上。
 *
 * 用法：
 *   ./mobile_ctrl [端口] [token] [can接口]
 *   默认 8085、无 token（内网免验证）、can0
 *
 * 编译：
 *   $CC mobile_ctrl.c -o mobile_ctrl -lpthread
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#define CAN_ID_CTRL_CMD     0x110
#define CAN_ID_STATUS       0x100
#define CAN_ID_MOTOR_STATE  0x102
#define CAN_ID_IMU_FRAME    0x101

/* 协议动作码，和 firmware_stm32/can.h 一一对应 */
#define DEV_MOTOR   0x03
#define ACT_ON      0x01
#define ACT_OFF     0x02
#define ACT_BACK    0x03
#define ACT_LEFT    0x04
#define ACT_RIGHT   0x05
#define ACT_ESTOP   0x07
#define ACT_RESUME  0x08

/* 死人开关超时。取 600ms 是因为网页每 200ms 发一次，
   允许丢两拍——WiFi 抖一下不至于让车一顿一顿的，
   但真断了最多 0.6 秒就停。 */
#define CTRL_TIMEOUT_MS  600

static int   g_can = -1;
static char  g_token[64] = "";
static volatile long long g_last_cmd_ms = 0;   /* 最后一条运动指令的时刻 */
static volatile int       g_moving = 0;        /* 当前是否处于运动指令中 */
static pthread_mutex_t    g_can_lock = PTHREAD_MUTEX_INITIALIZER;

/* 最近一次从 CAN 收到的车辆状态，给网页轮询用 */
static volatile int g_rpm_l = 0, g_rpm_r = 0;
static volatile int g_odom_l = 0, g_odom_r = 0;
static volatile int g_dist = 0, g_batt_dV = 0, g_health = 0;
static volatile int g_have_data = 0;

static long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ============ CAN ============ */
static int can_open(const char *ifname)
{
    struct sockaddr_can addr;
    struct ifreq ifr;
    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) return -1;

    memset(&ifr, 0, sizeof(ifr));
    memset(&addr, 0, sizeof(addr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) { close(s); return -1; }
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(s); return -1; }
    return s;
}

static void can_send_cmd(int act, int p1, int p2)
{
    struct can_frame f;
    if (g_can < 0) return;
    memset(&f, 0, sizeof(f));
    f.can_id = CAN_ID_CTRL_CMD;
    f.can_dlc = 4;
    f.data[0] = DEV_MOTOR;
    f.data[1] = (unsigned char)act;
    f.data[2] = (unsigned char)p1;
    f.data[3] = (unsigned char)p2;
    /* 加锁：死人开关线程和 HTTP 线程都会写同一个 socket。
       write 本身对 SocketCAN 是原子的，但两边还会改 g_moving，
       用同一把锁把"发帧 + 改状态"这对操作绑在一起，
       免得出现"刚发了停止、状态却被另一边改回运动中"。 */
    pthread_mutex_lock(&g_can_lock);
    if (write(g_can, &f, sizeof(f)) < 0)
        fprintf(stderr, "[ctrl] CAN write: %s\n", strerror(errno));
    pthread_mutex_unlock(&g_can_lock);
}

/* CAN 接收线程：把车辆状态缓存下来，网页轮询 /st 时直接读 */
static void *can_rx_thread(void *arg)
{
    struct can_frame f;
    (void)arg;
    for (;;) {
        ssize_t n = read(g_can, &f, sizeof(f));
        if (n < (ssize_t)sizeof(f)) {
            if (errno == EINTR) continue;
            usleep(200000);
            continue;
        }
        switch (f.can_id & CAN_SFF_MASK) {
        case CAN_ID_STATUS:
            g_dist = (f.data[2] << 8) | f.data[3];
            g_have_data = 1;
            break;
        case CAN_ID_IMU_FRAME:
            g_health  = f.data[6];
            g_batt_dV = f.data[7];      /* 0.1V 单位 */
            break;
        case CAN_ID_MOTOR_STATE:
            /* 转速有符号（协议 v2.4） */
            g_rpm_l  = (short)((f.data[0] << 8) | f.data[1]);
            g_rpm_r  = (short)((f.data[2] << 8) | f.data[3]);
            g_odom_l = (f.data[4] << 8) | f.data[5];
            g_odom_r = (f.data[6] << 8) | f.data[7];
            break;
        default: break;
        }
    }
    return NULL;
}

/* 死人开关线程 */
static void *deadman_thread(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(100000);                       /* 100ms 查一次，足够细 */
        if (!g_moving) continue;
        if (now_ms() - g_last_cmd_ms > CTRL_TIMEOUT_MS) {
            g_moving = 0;
            can_send_cmd(ACT_OFF, 0, 0);
            printf("[ctrl] 死人开关触发：%dms 没收到指令，已停车\n", CTRL_TIMEOUT_MS);
            fflush(stdout);
        }
    }
    return NULL;
}

/* ============ HTTP 小工具 ============ */
static int qs_get(const char *req, const char *key, char *out, size_t outlen)
{
    char pat[64];
    const char *p, *e;
    size_t n;
    snprintf(pat, sizeof(pat), "%s=", key);
    p = strstr(req, pat);
    if (!p) return -1;
    p += strlen(pat);
    e = p;
    while (*e && *e != '&' && *e != ' ' && *e != '\r' && *e != '\n') e++;
    n = (size_t)(e - p);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

static int check_token(const char *req)
{
    char tok[64];
    if (!g_token[0]) return 1;                 /* 没配 token 就不校验 */
    if (qs_get(req, "token", tok, sizeof(tok)) != 0) return 0;
    return strcmp(tok, g_token) == 0;
}

static void send_all(int c, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(c, buf + off, len - off);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

static void http_text(int c, const char *status, const char *ctype, const char *body)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        status, ctype, (unsigned)strlen(body));
    send_all(c, hdr, (size_t)n);
    send_all(c, body, strlen(body));
}

/* ============ 手机页面 ============
 * 整页内联，不引任何 CDN——车在园区里跑，板子未必能上外网，
 * 而遥控页恰恰是最不能"因为加载不出 jQuery 所以打不开"的东西。
 * 样式沿用板子中控那套 iOS 语言，保持一致。 */
static const char *PAGE =
"<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,user-scalable=no,viewport-fit=cover\">"
"<title>EdgeMonitor 遥控</title><style>"
"*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;-webkit-user-select:none;user-select:none}"
"body{margin:0;background:#0B0B0F;color:#fff;font:16px -apple-system,BlinkMacSystemFont,'PingFang SC',sans-serif;"
"padding:env(safe-area-inset-top) 16px calc(env(safe-area-inset-bottom) + 16px)}"
"h1{font-size:20px;margin:18px 0 4px}.sub{color:#8E8E93;font-size:13px;margin-bottom:14px}"
".card{background:#1C1C1E;border-radius:16px;padding:14px 16px;margin-bottom:12px}"
".row{display:flex;align-items:center;justify-content:space-between;padding:6px 0}"
".row b{font-variant-numeric:tabular-nums;font-size:17px}"
".dim{color:#8E8E93;font-size:13px}"
"#pad{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin-top:6px}"
"button{border:none;border-radius:18px;background:#2C2C2E;color:#fff;font-size:17px;"
"height:76px;font-family:inherit;touch-action:none}"
"button:active{background:#3A3A3C}"
"#stop{background:#FF453A;font-weight:600}#stop:active{background:#d33)}"
".sp{width:100%;-webkit-appearance:none;height:4px;border-radius:2px;background:#3A3A3C;outline:none}"
".sp::-webkit-slider-thumb{-webkit-appearance:none;width:28px;height:28px;border-radius:14px;background:#fff}"
"#warn{background:#3A2A00;color:#FF9F0A;border-radius:12px;padding:10px 12px;font-size:13px;margin-bottom:12px}"
"</style></head><body>"
"<h1>EdgeMonitor 遥控</h1><div class=\"sub\" id=\"link\">直连车载网关</div>"
"<div id=\"warn\">按住方向键才会走，松手立即停。手机息屏或断网时车也会自动停下。</div>"
"<div class=\"card\">"
"<div class=\"row\"><span class=\"dim\">车速</span><b id=\"spd\">0.00 km/h</b></div>"
"<div class=\"row\"><span class=\"dim\">左 / 右轮 RPM</span><b id=\"rpm\">0 / 0</b></div>"
"<div class=\"row\"><span class=\"dim\">前方障碍</span><b id=\"dist\">-- cm</b></div>"
"<div class=\"row\"><span class=\"dim\">电池</span><b id=\"bat\">--</b></div>"
"</div>"
"<div class=\"card\"><div class=\"row\"><span class=\"dim\">速度</span><b id=\"spv\">40%</b></div>"
"<input class=\"sp\" id=\"sl\" type=\"range\" min=\"10\" max=\"100\" value=\"40\"></div>"
"<div id=\"pad\">"
"<div></div><button data-a=\"1\">前进</button><div></div>"
"<button data-a=\"4\">左转</button><button id=\"stop\">停止</button><button data-a=\"5\">右转</button>"
"<div></div><button data-a=\"3\">后退</button><div></div>"
"</div>"
"<script>"
"var TK=new URLSearchParams(location.search).get('token')||'';"
"var q=TK?('&token='+encodeURIComponent(TK)):'';"
"var sl=document.getElementById('sl'),spv=document.getElementById('spv');"
"sl.oninput=function(){spv.textContent=sl.value+'%'};"
"function send(a){fetch('/cmd?act='+a+'&spd='+sl.value+q,{method:'POST'}).catch(function(){})}"
/* 按住期间每 200ms 重发一次：服务端的死人开关就是靠这个心跳判断
   "人还在按着"。只发一次的话，手机一断车就停不下来了。 */
"var t=null;"
"function hold(a){stopHold();send(a);t=setInterval(function(){send(a)},200)}"
"function stopHold(){if(t){clearInterval(t);t=null}}"
"function release(){stopHold();send(2)}"
"document.querySelectorAll('#pad button[data-a]').forEach(function(b){"
"var a=b.getAttribute('data-a');"
"b.addEventListener('touchstart',function(e){e.preventDefault();hold(a)},{passive:false});"
"b.addEventListener('touchend',function(e){e.preventDefault();release()},{passive:false});"
"b.addEventListener('touchcancel',function(){release()});"
"b.addEventListener('mousedown',function(){hold(a)});"
"b.addEventListener('mouseup',function(){release()});"
"b.addEventListener('mouseleave',function(){if(t)release()});});"
"document.getElementById('stop').addEventListener('click',function(){release()});"
/* 息屏/切到后台时立刻停：iOS 上切走之后 setInterval 会被节流甚至暂停，
   心跳一停死人开关也会兜住，但主动发一条停止更快、更明确。 */
"document.addEventListener('visibilitychange',function(){if(document.hidden)release()});"
"window.addEventListener('pagehide',function(){release()});"
"function poll(){fetch('/st?'+Date.now()+q).then(function(r){return r.json()}).then(function(d){"
"var kmh=(Math.abs((d.rl+d.rr)/2)*0.204/60*3.6).toFixed(2);"
"document.getElementById('spd').textContent=kmh+' km/h';"
"document.getElementById('rpm').textContent=d.rl+' / '+d.rr;"
"document.getElementById('dist').textContent=(d.ok?d.dist+' cm':'-- cm');"
"document.getElementById('bat').textContent=(d.bat>0?d.bat.toFixed(1)+' V':'未接入');"
"}).catch(function(){})}"
"setInterval(poll,500);poll();"
"</script></body></html>";

/* ============ 请求处理 ============ */
static void handle(int c, char *req)
{
    char act[16], spd[16];
    int a, p1;

    if (strncmp(req, "GET / ", 6) == 0 || strncmp(req, "GET /?", 6) == 0) {
        http_text(c, "200 OK", "text/html; charset=utf-8", PAGE);
        return;
    }

    if (!check_token(req)) {
        http_text(c, "403 Forbidden", "text/plain; charset=utf-8", "token 不对");
        return;
    }

    if (strstr(req, "/st")) {
        char body[256];
        snprintf(body, sizeof(body),
                 "{\"rl\":%d,\"rr\":%d,\"ol\":%d,\"or\":%d,"
                 "\"dist\":%d,\"bat\":%.1f,\"hf\":%d,\"ok\":%d}",
                 g_rpm_l, g_rpm_r, g_odom_l, g_odom_r,
                 g_dist, g_batt_dV / 10.0, g_health, g_have_data);
        http_text(c, "200 OK", "application/json", body);
        return;
    }

    if (strstr(req, "/cmd")) {
        if (qs_get(req, "act", act, sizeof(act)) != 0) {
            http_text(c, "400 Bad Request", "text/plain", "missing act");
            return;
        }
        a = atoi(act);
        p1 = (qs_get(req, "spd", spd, sizeof(spd)) == 0) ? atoi(spd) : 40;
        if (p1 < 0) p1 = 0;
        if (p1 > 100) p1 = 100;

        /* 只放行运动相关的动作码。不做白名单的话，这个接口等于把
           整个 0x110 协议暴露给任何能访问端口的人（包括 OTA 之外的
           各种设备控制），风险面平白扩大一圈。 */
        if (a == ACT_ON || a == ACT_BACK || a == ACT_LEFT || a == ACT_RIGHT) {
            g_last_cmd_ms = now_ms();
            g_moving = 1;
            /* 转向的 p2 给 0（持续转），由死人开关负责停——
               不用协议里的定时转向：那个 ms 参数只有一个字节，最多 255ms，
               松手前就自己停了，手感是一顿一顿的。 */
            can_send_cmd(a, p1, 0);
        } else if (a == ACT_OFF) {
            g_moving = 0;
            can_send_cmd(ACT_OFF, 0, 0);
        } else if (a == ACT_ESTOP || a == ACT_RESUME) {
            g_moving = 0;
            can_send_cmd(a, 0, 0);
        } else {
            http_text(c, "400 Bad Request", "text/plain", "act not allowed");
            return;
        }
        http_text(c, "200 OK", "text/plain", "ok");
        return;
    }

    http_text(c, "404 Not Found", "text/plain", "not found");
}

int main(int argc, char *argv[])
{
    int port = (argc > 1) ? atoi(argv[1]) : 8085;
    const char *tok = (argc > 2) ? argv[2] : "";
    const char *can_if = (argc > 3) ? argv[3] : "can0";
    int srv, opt = 1;
    struct sockaddr_in addr;
    pthread_t th_rx, th_dm;

    snprintf(g_token, sizeof(g_token), "%s", tok);

    g_can = can_open(can_if);
    if (g_can < 0) {
        fprintf(stderr, "[ctrl] 打不开 %s: %s\n", can_if, strerror(errno));
        return 1;
    }
    printf("[ctrl] CAN %s ready\n", can_if);

    pthread_create(&th_rx, NULL, can_rx_thread, NULL);
    pthread_create(&th_dm, NULL, deadman_thread, NULL);

    srv = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[ctrl] bind %d: %s\n", port, strerror(errno));
        return 1;
    }
    listen(srv, 8);
    printf("[ctrl] 手机遥控页: http://<板子IP>:%d/%s\n",
           port, g_token[0] ? "?token=<你的token>" : "");
    printf("[ctrl] 死人开关: %dms 无指令自动停车\n", CTRL_TIMEOUT_MS);
    fflush(stdout);

    for (;;) {
        char req[2048];
        ssize_t n;
        int c = accept(srv, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; break; }

        /* 收请求头就够了：这个接口没有 body。
           设个超时，免得半开连接把单线程的循环卡死——
           手机在弱网下很容易留下这种连了但不发数据的连接。 */
        struct timeval tv = { 2, 0 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        n = read(c, req, sizeof(req) - 1);
        if (n > 0) {
            req[n] = '\0';
            handle(c, req);
        }
        close(c);
    }

    close(srv);
    close(g_can);
    return 0;
}
