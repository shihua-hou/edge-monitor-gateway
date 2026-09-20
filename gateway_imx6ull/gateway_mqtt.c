/**
 * gateway_mqtt.c - i.MX6ULL MQTT 网关（阶段 5/6）
 *
 * 功能：
 *   1. CAN→MQTT 上行：读 can0，解析 STM32 上报帧，发布 JSON 到
 *      monitor/data/status|imu|motor
 *   2. MQTT→CAN 下行：订阅 monitor/cmd，收到控制命令转 CAN 0x110 下发 STM32
 *
 * 依赖：libmosquitto
 *   Ubuntu: apt install libmosquitto-dev mosquitto
 *
 * 编译：
 *   gcc gateway_mqtt.c -o gateway_mqtt -lmosquitto
 *   arm-linux-gnueabihf-gcc gateway_mqtt.c -o gateway_mqtt -lmosquitto
 *
 * 运行（broker 已启动）：
 *   export MQTT_USER=em MQTT_PASS=yourpass MQTT_HOST=192.168.x.x
 *   ./gateway_mqtt [can0]
 *
 * v2 变更：broker 认证（配合 mosquitto 禁匿名）+ 0x100 上报湿度字段
 * v2.1 变更：broker 地址从写死的 localhost 改成可通过 MQTT_HOST 环境变量配置——
 *   网关程序和 broker 不一定跑在同一台机器上（比如 broker 放 PC/云端，网关跑板子上），
 *   写死 localhost 只覆盖了两者同机部署这一种情况
 *
 * MQTT 消息格式（v3：topic 带设备号，支持一个 broker 接多台机器人）：
 *   monitor/<dev>/data/status: {"t":25.3,"h":56.2,"l":80,"d":45,"s":0}
 *   monitor/<dev>/data/imu:    {"p":1.2,"r":-0.5,"y":30.1,"hf":0,"bat":12.6}
 *       hf = 传感器健康位，bit0 温湿度 bit1 超声波 bit2 光敏 bit3 IMU bit4 电机；
 *       bat = 电池电压 V，**0 = ADC 线未接**（不是 0V）；
 *       置 1 表示该路连续读取失败、数值是陈旧值（不是当前实测）
 *   monitor/<dev>/data/motor:  {"rl":300,"rr":305,"ol":120,"or":125}
 *   monitor/<dev>/cmd (下发):   {"d":1,"a":1,"p1":0,"p2":0}
 *   monitor/online/<dev>:      {"online":1,"dev":"robot01","ip":"...","video":8081}
 * <dev> 取自 DEVICE_ID 环境变量，默认 robot01。
 * 大屏侧用 monitor/+/data/# 通配订阅所有设备。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <mosquitto.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <sys/file.h>
#include <time.h>          /* time()：上线通告的重发节流 */
#include <sys/time.h>      /* struct timeval：给 CAN socket 设接收超时 */

/* ============ 常量 ============ */
#define CAN_ID_STATUS_REPORT  0x100
#define CAN_ID_IMU_FRAME      0x101
#define CAN_ID_MOTOR_STATE    0x102
#define CAN_ID_MOTOR_DISP     0x103
#define CAN_ID_CTRL_CMD       0x110

#define MQTT_HOST_DEFAULT  "localhost"
#define MQTT_PORT  1883
/* Topic 带设备号：monitor/<device_id>/data/xxx 和 monitor/<device_id>/cmd。
   原来是全局的 monitor/data/status、monitor/cmd，只能支撑单台设备——
   两台机器人接同一个 broker 时，上行数据会混成一锅粥分不清是谁的，
   下行命令更糟：发一条"前进"，在线的所有机器人都会一起动。
   设备号在启动时从 DEVICE_ID 取，运行期不变，所以 topic 拼一次存起来。 */
#define TP_PREFIX  "monitor/"
static char g_tp_status[128];
static char g_tp_imu[128];
static char g_tp_motor[128];

/* 0x103 的有符号位移。缓存下来随 0x102 一起发：两帧是背靠背发出的，
   拆成两条 MQTT 消息的话，上位机得自己对齐时序才能用——
   而"对齐两条异步消息"正是最容易写出竞态的地方。合并成一条就没这问题。
   初值 0 且 g_have_disp=0：**没收到过 0x103 时字段整个不出现**，
   而不是填 0 冒充——老固件没有这一帧，填 0 会让上位机以为车一直没动。 */
static int g_disp_l = 0, g_disp_r = 0;
static int g_have_disp = 0;
static char g_tp_cmd[128];

/* 设备在线状态 topic 前缀。broker 现在部署在服务器侧（不再跟网关同机），
   才谈得上"设备掉线检测"——broker 要是跟着设备一起断电，遗嘱消息根本
   没人替你发。DEVICE_ID 留了环境变量，为将来一个大屏看多台设备做准备 */
#define TP_ONLINE_PREFIX   "monitor/online/"
#define DEVICE_ID_DEFAULT  "robot01"

static int  g_can_fd = -1;
static struct mosquitto *g_mosq = NULL;
/* 设备号。原本只是 main 里的一个局部变量，但重发上线通告时也要用到它，
   而那个函数在主循环里被调用——提成全局比一路往下传参干净 */
static char g_dev_id[64] = "";
static char g_tp_cmdecho[128];   /* 命令回显，见 publish_frame 的 0x110 分支 */
static char g_online_topic[128];
static char g_online_payload[192];   /* 上线通告，内容见 build_online_payload */
static char g_will_payload[96];      /* 掉线遗嘱，broker 代发 */

/* 单实例锁。防的是"同一台设备上不小心起了两个网关"——两个实例的 MQTT
   client_id 相同，broker 按协议必须踢掉先连的那个，于是两个实例无限互踢，
   日志刷满 connected / 连接断开，数据时断时续。这个现象很有迷惑性：
   进程都在、CAN 也正常、broker 也没问题，光看单个实例的日志根本看不出
   是"有人在跟我抢同一个身份"。（实测踩过一次，手动启动 + 守护脚本各起了
   一个。）
   用 flock 而不是 pid 文件：进程被 kill -9 时锁会由内核自动释放，
   不会留下需要手工清理的陈旧 pid 文件。 */
static int acquire_single_instance_lock(const char *dev_id)
{
    char path[128];
    int fd;
    snprintf(path, sizeof(path), "/tmp/gateway_mqtt_%s.lock", dev_id);
    fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) return 0;          /* 锁文件都建不了就别拦着程序跑了 */
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;                 /* 已经有一个实例在跑 */
    }
    /* 故意不 close：进程活着期间要一直持有这个锁 */
    return 0;
}

/* 取本机第一个非 loopback 的 IPv4，用于上线时"自报家门"。
   新架构下 broker 在服务器、视频服务留在机器人本机，两者不再是同一个
   地址，大屏没法再拿 broker 地址去拼视频 URL。与其让人在设置里多填一个
   IP（多设备时还得填 N 个），不如让设备上线时把自己的地址一起通告出去，
   大屏照着取即可——多一台设备就自动多一路，不用改配置 */
static void get_local_ip(char *out, size_t outlen)
{
    struct ifaddrs *ifs = NULL, *p;
    snprintf(out, outlen, "%s", "");
    if (getifaddrs(&ifs) != 0) return;
    for (p = ifs; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (p->ifa_flags & IFF_LOOPBACK) continue;
        if (!(p->ifa_flags & IFF_UP)) continue;
        inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, out, (socklen_t)outlen);
        break;
    }
    freeifaddrs(ifs);
}

/* ============ SocketCAN 打开 ============ */
static int open_can(const char *ifname)
{
    int s;
    struct sockaddr_can addr;
    struct ifreq ifr;
    s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) { perror("socket"); return -1; }
    /* memset 不能省：ifreq 是个 union，ioctl 只填 ifr_ifindex，其余字节
       如果是栈上的垃圾会一起传进内核；顺带也保证了 ifr_name 一定有结尾的
       '\0'（strncpy 在源串刚好占满时不会补） */
    memset(&ifr, 0, sizeof(ifr));
    memset(&addr, 0, sizeof(addr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr, "ioctl(SIOCGIFINDEX, %s): %s（接口是不是没 up？"
                        "试试 ip link set %s up type can bitrate 500000）\n",
                ifname, strerror(errno), ifname);
        close(s);
        return -1;
    }
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(s);
        return -1;
    }

    /* 给 read() 加 1 秒超时，让主循环有机会做例行检查（见 refresh_online_if_ip_changed）。
       不加的话 read 会一直阻塞：STM32 没上电或 CAN 线没接时，主循环整个停住，
       什么周期性的事都做不了。1 秒一次的空转开销可以忽略，换来的是
       "没有 CAN 数据时程序依然活着并且能做事"。 */
    {
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    return s;
}

/* ============ 上线通告：IP 变了就重发 ============
 *
 * 踩到的真实问题：开机时守护脚本先把本程序拉起来，WiFi 三分钟后才关联上。
 * 网关启动那一刻 get_local_ip() 什么也取不到，于是通告发出去是 "ip":"" ——
 * 而它是一条 retained 消息，会一直挂在 broker 上不再更新。
 * 大屏靠这个字段拿机器人的地址去接视频流，ip 空了视频就永远连不上，
 * 而且两端都不报错，表现成"视频功能坏了"，实际只是那条通告发早了。
 *
 * DHCP 续租拿到新地址、WiFi 自愈重连之后也是同一个问题。
 * 所以不能"启动时发一次就完事"，必须盯着 IP 变化重发。
 */
static void refresh_online_if_ip_changed(void)
{
    static time_t last_check = 0;
    static char   last_ip[64] = "";
    char ip[64];
    time_t now = time(NULL);

    if (now - last_check < 10) return;   /* 10 秒查一次足够 */
    last_check = now;

    get_local_ip(ip, sizeof(ip));
    if (strcmp(ip, last_ip) == 0) return;
    snprintf(last_ip, sizeof(last_ip), "%s", ip);

    snprintf(g_online_payload, sizeof(g_online_payload),
             "{\"online\":1,\"dev\":\"%s\",\"ip\":\"%s\",\"video\":8081}",
             g_dev_id, ip);
    mosquitto_publish(g_mosq, NULL, g_online_topic,
                      (int)strlen(g_online_payload), g_online_payload, 1, true);
    printf("[mqtt] 本机 IP 变为 %s，已重发上线通告\n", ip[0] ? ip : "(空)");
}

/* ============ 轻量 JSON 整数提取 ============ */
static int json_get_int(const char *json, const char *key)
{
    char pat[32];
    char *p;
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    p = strstr(json, pat);
    if (!p) return 0;
    return atoi(p + strlen(pat));
}

/* ============ MQTT 订阅回调：收到控制命令 → 转 CAN 下发 ============ */
static void on_message(struct mosquitto *mosq, void *userdata,
                       const struct mosquitto_message *msg)
{
    struct can_frame f;
    int d, a, p1, p2;
    char buf[256];
    size_t n;

    (void)mosq; (void)userdata;   /* 回调签名要求，这里用不到 */
    if (msg->payloadlen <= 0) return;
    if (strcmp(msg->topic, g_tp_cmd) != 0) return;

    /* 先拷进本地缓冲并补 '\0' 再用 strstr：payload 是按长度给的二进制块，
       直接当 C 字符串扫描要赌它后面正好有结束符。这是别人能往 broker 上
       随便发的数据，不该拿它赌 */
    n = (size_t)msg->payloadlen < sizeof(buf) - 1 ? (size_t)msg->payloadlen : sizeof(buf) - 1;
    memcpy(buf, msg->payload, n);
    buf[n] = '\0';

    d  = json_get_int(buf, "d");
    a  = json_get_int(buf, "a");
    p1 = json_get_int(buf, "p1");
    p2 = json_get_int(buf, "p2");

    /* 基本范围校验：这是控制下行链路唯一的入口，之前是不管三七二十一直接
       透传上 CAN 总线的，任何客户端只要连上 broker 就能塞任意字节过去 */
    /* 动作码上限跟着协议走：1~6 是最初的 LED/蜂鸣器/运动，
       后来又加了 7=急停 8=解除急停 9=坦克式差速。
       这一行当时没跟着改，结果是新动作全被静静丢掉——
       而且只在网关日志里打一行 rejected，上层无从得知。
       **白名单式校验必须和协议同步扩展，否则它会从安全措施变成隐形障碍。** */
    if (d < 1 || d > 4 || a < 1 || a > 9 || p1 < 0 || p1 > 255 || p2 < 0 || p2 > 255) {
        printf("[cmd] rejected: dev=%d act=%d p1=%d p2=%d out of range\n", d, a, p1, p2);
        return;
    }
    printf("[cmd] dev=%d act=%d p1=%d p2=%d\n", d, a, p1, p2);

    memset(&f, 0, sizeof(f));
    f.can_id = CAN_ID_CTRL_CMD;
    f.can_dlc = 4;
    f.data[0] = (uint8_t)d;
    f.data[1] = (uint8_t)a;
    f.data[2] = (uint8_t)p1;
    f.data[3] = (uint8_t)p2;
    /* CAN 正在恢复中（见 can_thread）时 g_can_fd 是 -1，此时不能下发。
       不判的话 write(-1, ...) 只是返回 EBADF，日志里报一句"发送失败"，
       意思却完全不同——那是"设备暂时不可用"，不是"总线接线有问题" */
    if (g_can_fd < 0) {
        fprintf(stderr, "[cmd] CAN 接口正在恢复中，本条命令已丢弃\n");
        return;
    }
    /* 检查返回值：CAN 接口没 up、总线上没有其它节点应答导致发送缓冲堆满时
       write 会失败，命令就这么无声无息地丢了，界面上却显示"已下发" */
    if (write(g_can_fd, &f, sizeof(f)) != (ssize_t)sizeof(f)) {
        fprintf(stderr, "[cmd] CAN 发送失败: %s（总线是否接好、对端是否在线）\n",
                strerror(errno));
    }
}

/* ============ 连接成功回调 ============
   订阅必须放在这里，不能只在 main 里调一次：mosquitto_loop_forever 掉线后
   会自动重连，而我们用的是 clean_session=true 的会话，重连拿到的是一个全新
   会话，broker 那边不保留任何订阅。只在 main 里订阅的话，第一次断网之后
   程序看着还在正常跑、上行数据也照发，但下行控制命令再也收不到了——
   这种"半死不活"比直接崩掉更难查 */
static void on_connect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)userdata;
    if (rc != 0) {
        fprintf(stderr, "[mqtt] broker 拒绝连接: %s\n", mosquitto_connack_string(rc));
        return;
    }
    mosquitto_subscribe(mosq, NULL, g_tp_cmd, 1);
    /* 上线通告：retained=true，这样大屏什么时候打开都能立刻读到当前状态，
       不用干等下一次上报。掉线时由 broker 按遗嘱把同一个 topic 覆盖成
       online:0 —— 同一个 topic 上"最后一条 retained 消息"就是设备此刻的
       真实状态，大屏订阅一次就够 */
    mosquitto_publish(mosq, NULL, g_online_topic,
                      (int)strlen(g_online_payload), g_online_payload, 1, true);
    printf("[mqtt] connected, subscribed: %s, online -> %s\n", g_tp_cmd, g_online_topic);
}

static void on_disconnect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)mosq; (void)userdata;
    /* rc==0 是我们自己调 disconnect，其它都是意外断开 */
    if (rc != 0) fprintf(stderr, "[mqtt] 连接断开(%s)，等待自动重连…\n", mosquitto_strerror(rc));
}

/* ============ 解析并发布一帧 CAN ============ */
static void publish_frame(const struct can_frame *f)
{
    char payload[128];
    switch (f->can_id & CAN_EFF_MASK) {
    case CAN_ID_STATUS_REPORT: {
        /* 温度改成 data[0](高字节)+data[7](低字节) 大端两字节，跟 STM32 端
           app_task.c 的 task_can_report 同步改过——原来只用 data[0] 一个
           字节、(int8_t) 强转，任何超过 12.7℃ 的正常室温都会被解释成负数，
           是必现的协议设计缺陷，不是偶发问题 */
        int16_t temp = (int16_t)((f->data[0] << 8) | f->data[7]);
        int light = f->data[1];
        int dist  = (f->data[2] << 8) | f->data[3];
        int state = f->data[4];
        int16_t hum = (int16_t)((f->data[5] << 8) | f->data[6]);  /* 湿度 0.1% */
        snprintf(payload, sizeof(payload), "{\"t\":%.1f,\"h\":%.1f,\"l\":%d,\"d\":%d,\"s\":%d}",
                 temp / 10.0, hum / 10.0, light, dist, state);
        mosquitto_publish(g_mosq, NULL, g_tp_status, strlen(payload), payload, 0, false);
        break;
    }
    case CAN_ID_IMU_FRAME: {
        int16_t p = (int16_t)((f->data[0] << 8) | f->data[1]);
        int16_t r = (int16_t)((f->data[2] << 8) | f->data[3]);
        int16_t y = (int16_t)((f->data[4] << 8) | f->data[5]);
        /* data[6] = 传感器健康位（STM32 侧 app_task.h 的 SENS_FAULT_*）：
             bit0 温湿度  bit1 超声波  bit2 光敏  bit3 IMU
           置 1 表示该传感器连续读取失败，对应的数值是【上一次的陈旧值】。
           这个字节走 IMU 帧只是因为这一帧还有空位（0x100 的 8 字节已排满），
           它描述的是全部四路传感器，不只是姿态。
           前端拿到 hf 后要把对应的数值标成"陈旧"——否则传感器坏了，
           界面上照样是个稳定的数字，没人看得出来。 */
        int hf = f->data[6];
        /* data[7] = 电池电压，0.1V 为单位。**0 表示未接线，不是 0V**——
           前端要把这两种情况显示成不同的样子，否则每台没接 ADC 线的设备
           都在报低电量，假告警看多了真告警也就没人信了。 */
        int bat = f->data[7];
        snprintf(payload, sizeof(payload),
                 "{\"p\":%.2f,\"r\":%.2f,\"y\":%.2f,\"hf\":%d,\"bat\":%.1f}",
                 p / 100.0, r / 100.0, y / 100.0, hf, bat / 10.0);
        mosquitto_publish(g_mosq, NULL, g_tp_imu, strlen(payload), payload, 0, false);
        break;
    }
    case CAN_ID_MOTOR_STATE: {
        /* 变量别叫 or：<iso646.h> 里 or 是 || 的替代拼写，在 C 里只要有谁
           间接包含了这个头（或哪天这份代码被 C++ 编译）就会直接编译失败。
           JSON 字段名保持 "or" 不变，前端不用改 */
        /* 转速是有符号的（协议 v2.4）：先拼成 16 位再按 int16_t 解释。
           不转的话倒车会变成 65000 多——一个完全合法、不会报错、只是完全
           错了的数字，而且“一侧轮子反转”（抱死、接线反了）这种真正要看的故障
           就此隐形。同 parse_status 里温度那一处的处理。 */
        int rl = (int)(short)((f->data[0] << 8) | f->data[1]);
        int rr = (int)(short)((f->data[2] << 8) | f->data[3]);
        int odo_l = (f->data[4] << 8) | f->data[5];
        int odo_r = (f->data[6] << 8) | f->data[7];
        if (g_have_disp)
            snprintf(payload, sizeof(payload),
                     "{\"rl\":%d,\"rr\":%d,\"ol\":%d,\"or\":%d,\"dl\":%d,\"dr\":%d}",
                     rl, rr, odo_l, odo_r, g_disp_l, g_disp_r);
        else
            snprintf(payload, sizeof(payload), "{\"rl\":%d,\"rr\":%d,\"ol\":%d,\"or\":%d}",
                     rl, rr, odo_l, odo_r);
        mosquitto_publish(g_mosq, NULL, g_tp_motor, strlen(payload), payload, 0, false);
        break;
    }
    case CAN_ID_MOTOR_DISP: {
        /* 左右轮有符号位移，各 int32 大端，单位 mm。
           必须按 int32_t 解释：倒车时是负数，当无符号读会变成 40 多亿，
           轨迹瞬间被甩出画布——和转速那处是同一类错误。 */
        g_disp_l = (int)(int32_t)(((uint32_t)f->data[0] << 24) | ((uint32_t)f->data[1] << 16) |
                                  ((uint32_t)f->data[2] << 8)  |  (uint32_t)f->data[3]);
        g_disp_r = (int)(int32_t)(((uint32_t)f->data[4] << 24) | ((uint32_t)f->data[5] << 16) |
                                  ((uint32_t)f->data[6] << 8)  |  (uint32_t)f->data[7]);
        g_have_disp = 1;
        break;   /* 不单独发消息，等下一帧 0x102 一起带出去 */
    }
    case CAN_ID_CTRL_CMD: {
        /* 命令回显。
         *
         * 这一帧是**我们自己发出去**的，不是 STM32 上报的——SocketCAN 默认开
         * 本机回环，同一接口上其它 socket 能收到本机发的帧，所以在这里一并抓到。
         *
         * 为什么要回显：车有两个控制入口——\"t UI 走 M\"TT 到本网关，
         * 手机控制页(mobile_ctrl)在板子上**直接写 CAN**。后者 \"t 完全看不见。
         * 而"路线录制"要录的恰恰是人怎么开的，漏掉手机那路就等于什么都没录到。
         * CAN 总线是两路唯一的汇合点，在这里回显能**录全**，
         * 而且录到的是原始指令本身：毫秒级时刻、精确数值。
         *
         * 之前录 0x102 的实测转速为什么不行：那个帧固定 200ms 一发，
         * 而人点一下转向也就 300~500ms——三次采样都不到，转向时长被量化到
         * 200ms 的格子上，误差直接 20~30%；再加上采到的都是 PI 还在爬坡
         * 途中的转速（比指令低），回放时转角系统性偏小。
         * 录指令没有这个问题：指令是阶跃的，没有"中间值"可采错。
         *
         * ts 用**单调时钟**而不是墙上时间：录制中途 NTP 校时跳一下，
         * 用 CLOCK_REALTIME 算出来的段长可能变成负数或几十秒。
         */
        struct timespec ts;
        long long ms;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        snprintf(payload, sizeof(payload),
                 "{\"ts\":%lld,\"dev\":%d,\"act\":%d,\"p1\":%d,\"p2\":%d}",
                 ms, f->data[0], f->data[1], f->data[2], f->data[3]);
        mosquitto_publish(g_mosq, NULL, g_tp_cmdecho, strlen(payload), payload, 0, false);
        break;
    }
    default:
        break;
    }
}

/* ============ CAN 读取线程（持续收帧并发布） ============ */

/* 读失败之后尝试把 CAN 接口救回来。
 *
 * 为什么需要：CAN 控制器在总线错误累积到一定程度后会进入 bus-off 状态
 * （干扰、线松了、对端掉电都会导致），此后 socket 上的读写全部报错，而且
 * 【不会自己恢复】——必须把网络接口 down 一次再 up，控制器才会重新初始化。
 *
 * 原来的处理是打一行日志然后让线程退出。那之后进程还活着、MQTT 还连着、
 * 网页上设备显示"在线"，只是永远不再有数据上来——从外面看跟"传感器坏了"
 * 一模一样，最误导人的一种状态。既然故障是可恢复的，就该自己恢复。
 *
 * 用 system() 调 ip 命令而不是自己发 netlink：一来 busybox 的 ip 一定在，
 * 二来这里的参数全是本程序自己拼的常量，不存在注入面（网页传来的不可信
 * 输入一律不走 shell，见 ota_service.c 的说明）。 */
static int can_recover(const char *ifname)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "ip link set %s down 2>/dev/null", ifname);
    if (system(cmd) == -1) return -1;
    sleep(1);
    snprintf(cmd, sizeof(cmd),
             "ip link set %s up type can bitrate 500000 2>/dev/null", ifname);
    if (system(cmd) == -1) return -1;
    sleep(1);
    return 0;
}

static void *can_thread(void *arg)
{
    struct can_frame f;
    const char *ifname = (const char *)arg;
    int backoff = 2;                 /* 恢复失败时的退避秒数，指数增长 */

    while (1) {
        ssize_t n;

        /* 上一轮没能重新打开接口，直接进恢复流程。
           不加这个判断的话会去 read(-1)，日志里报一句 "Bad file descriptor"——
           那看着像是又冒出了一个新故障，实际只是"还没恢复过来"，
           排查时会被这条误导性的错误带偏 */
        if (g_can_fd < 0) goto recover;

        n = read(g_can_fd, &f, sizeof(f));
        if (n >= 0) {
            if (n >= (ssize_t)sizeof(f)) publish_frame(&f);
            backoff = 2;             /* 收到数据说明链路正常，退避计时清零 */
            refresh_online_if_ip_changed();
            continue;
        }
        if (errno == EINTR) continue;

        /* SO_RCVTIMEO 到期。这【不是错误】，只是这一秒里没有 CAN 数据——
           必须在走进下面的恢复流程之前拦掉，否则每秒都会"检测到 CAN 故障"
           并重开一次接口，日志刷满假故障，真正的问题反而被淹没。
           这一秒的空档正好用来做例行检查。 */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            refresh_online_if_ip_changed();
            continue;
        }

        fprintf(stderr, "[can] 读取失败: %s，尝试恢复接口 %s\n",
                strerror(errno), ifname);
recover:

        /* 顺序很关键：先把全局 fd 置为 -1，再关闭本地保存的那份。
           反过来写的话，close 之后到置 -1 之前有一个窗口，此时 g_can_fd 还是
           那个已经关掉的号码；如果 mosquitto 的重连恰好在这一瞬 open 到同一个
           fd 号（内核总是分配当前最小的空闲号，很容易撞上），下行命令线程的
           write(g_can_fd, ...) 就会把 CAN 帧直接写进 MQTT 的 TCP 连接里。
           这种 bug 只在"恢复 CAN"和"MQTT 重连"同时发生时才出现，
           复现概率极低、后果却是协议流被污染，只能靠写对顺序来避免。 */
        {
            int old_fd = g_can_fd;
            g_can_fd = -1;
            if (old_fd >= 0) close(old_fd);
        }
        can_recover(ifname);
        g_can_fd = open_can(ifname);
        if (g_can_fd >= 0) {
            fprintf(stderr, "[can] 接口已恢复，上行数据继续\n");
            backoff = 2;
            continue;
        }

        /* 还是打不开——多半是接口被人为 down 了，或者硬件真的有问题。
           退避重试，不要疯狂 loop 把 CPU 占满；封顶 60 秒，别退避到
           "接口早恢复了、它还在睡"的程度 */
        fprintf(stderr, "[can] 恢复失败，%d 秒后重试\n", backoff);
        sleep(backoff);
        if (backoff < 60) backoff *= 2;
    }
    return NULL;
}

/* ============ 主流程 ============ */
int main(int argc, char *argv[])
{
    const char *can_if = (argc > 1) ? argv[1] : "can0";
    pthread_t tid;
    int rc;

    /* 行缓冲。这个程序常驻后台跑，stdout 基本都是重定向到日志文件的，
       而 libc 对非终端输出默认是全缓冲（4KB）——启动信息会一直积在
       缓冲区里不落盘，看日志只能看到一片空白，还以为程序没起来。
       实际排查时就被这个坑了一次。 */
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
    setvbuf(stderr, NULL, _IOLBF, BUFSIZ);

    const char *mqtt_host = getenv("MQTT_HOST");
    if (!mqtt_host || !mqtt_host[0]) mqtt_host = MQTT_HOST_DEFAULT;

    /* 1. 打开 CAN */
    g_can_fd = open_can(can_if);
    if (g_can_fd < 0) return 1;
    printf("CAN %s ready\n", can_if);

    /* 2. 初始化 mosquitto */
    /* client_id 必须带上设备号。MQTT 规定同一个 broker 上 client_id 不能重复，
       后连的会把先连的踢掉——写死成 "gateway_mqtt" 的话：
         · 多台机器人接同一个 broker 时会无限互踢
         · 本机不小心起了两个实例，现象是连上就断、反复循环，很难一眼看出原因
       （实测踩过：日志刷满 "connected" / "连接断开(The connection was lost.)"） */
    const char *dev_id = getenv("DEVICE_ID");
    if (!dev_id || !dev_id[0]) dev_id = DEVICE_ID_DEFAULT;
    char client_id[96];
    snprintf(client_id, sizeof(client_id), "gateway_%s", dev_id);

    if (acquire_single_instance_lock(dev_id) != 0) {
        fprintf(stderr,
            "已经有一个 gateway_mqtt (DEVICE_ID=%s) 在运行了，本次启动退出。\n"
            "  两个实例会用同一个 MQTT client_id 互相踢下线，导致反复重连。\n"
            "  查看：ps -ef | grep gateway_mqtt   （busybox 的 ps 要带 -ef）\n", dev_id);
        return 1;
    }

    mosquitto_lib_init();
    g_mosq = mosquitto_new(client_id, true, NULL);
    if (!g_mosq) { fprintf(stderr, "mosquitto_new fail\n"); return 1; }
    printf("MQTT client_id=%s\n", client_id);
    mosquitto_message_callback_set(g_mosq, on_message);
    mosquitto_connect_callback_set(g_mosq, on_connect);       /* 重连后恢复订阅，见回调注释 */
    mosquitto_disconnect_callback_set(g_mosq, on_disconnect);
    mosquitto_threaded_set(g_mosq, true);   /* 允许多线程 publish */

    /* v2: broker 认证（配合 mosquitto.conf 禁匿名） */
    const char *mqtt_user = getenv("MQTT_USER");
    const char *mqtt_pass = getenv("MQTT_PASS");
    if (mqtt_user && mqtt_pass) {
        mosquitto_username_pw_set(g_mosq, mqtt_user, mqtt_pass);
        printf("MQTT auth: user=%s\n", mqtt_user);
    } else {
        fprintf(stderr, "[warn] 未设置 MQTT_USER/MQTT_PASS，broker 若禁匿名将连接失败\n");
    }

    /* 遗嘱消息(LWT)必须在 connect 之前交给 broker：它是"我要是断了，你替我
       发这条"的委托。网线拔了、板子断电、进程被 kill 这些情况客户端自己
       已经发不出任何东西了，只能靠 broker 在 keepalive 超时后代发。
       这也是 broker 必须部署在设备之外的原因——同机部署时 broker 跟设备
       一起没了，没人执行遗嘱，大屏永远停在"最后一次数据"上，分不清是
       设备静默还是真的掉线 */
    /* dev_id 在上面构造 client_id 时已经取过了，这里直接复用。
       每台设备一组独立 topic，大屏用 monitor/+/data/# 通配订阅全部设备，
       下发命令时指定具体设备号，不会误伤别的机器人 */
    snprintf(g_dev_id, sizeof(g_dev_id), "%s", dev_id);   /* 重发上线通告时要用，见 refresh_online_if_ip_changed */
    snprintf(g_tp_status, sizeof(g_tp_status), "%s%s/data/status", TP_PREFIX, dev_id);
    snprintf(g_tp_imu,    sizeof(g_tp_imu),    "%s%s/data/imu",    TP_PREFIX, dev_id);
    snprintf(g_tp_motor,  sizeof(g_tp_motor),  "%s%s/data/motor",  TP_PREFIX, dev_id);
    snprintf(g_tp_cmd,    sizeof(g_tp_cmd),    "%s%s/cmd",         TP_PREFIX, dev_id);
    snprintf(g_tp_cmdecho, sizeof(g_tp_cmdecho), "%s%s/data/cmdecho", TP_PREFIX, dev_id);
    snprintf(g_online_topic, sizeof(g_online_topic), "%s%s", TP_ONLINE_PREFIX, dev_id);
    printf("topics: %s | %s\n", g_tp_status, g_tp_cmd);

    char ip[INET_ADDRSTRLEN] = "";
    get_local_ip(ip, sizeof(ip));
    snprintf(g_online_payload, sizeof(g_online_payload),
             "{\"online\":1,\"dev\":\"%s\",\"ip\":\"%s\",\"video\":8081}", dev_id, ip);
    snprintf(g_will_payload, sizeof(g_will_payload),
             "{\"online\":0,\"dev\":\"%s\"}", dev_id);
    mosquitto_will_set(g_mosq, g_online_topic,
                       (int)strlen(g_will_payload), g_will_payload, 1, true);
    printf("device=%s ip=%s, online topic=%s\n", dev_id, ip[0] ? ip : "(未取到)", g_online_topic);

    rc = mosquitto_connect(g_mosq, mqtt_host, MQTT_PORT, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        /* 不退出：板子开机时本程序很可能比 broker 先起来，这时直接退出就
           要靠人去手动重启。下面的 loop_forever 自带重连退避，让它慢慢等
           broker 起来即可，CAN 上行也不会因此中断 */
        fprintf(stderr, "[mqtt] 首次连接 %s:%d 失败(%s)，进入重连等待\n",
                mqtt_host, MQTT_PORT, mosquitto_strerror(rc));
    } else {
        printf("MQTT connecting to %s:%d ...\n", mqtt_host, MQTT_PORT);
    }
    /* 这里不再手动 subscribe：on_connect 回调会做，首次连接和之后每次
       自动重连都走同一条路径，不会出现"首次订阅了、重连后忘了订"的偏差 */

    /* 3. 启动 CAN 读取线程 + MQTT 网络循环
       把接口名传进线程：它在 CAN 出错时要靠这个名字把接口 down/up 一次
       重新初始化控制器（bus-off 状态不会自己恢复）。can_if 指向 argv 或
       字符串常量，生命周期跟进程一样长，直接传指针是安全的 */
    pthread_create(&tid, NULL, can_thread, (void *)can_if);
    mosquitto_loop_forever(g_mosq, -1, 1);

    mosquitto_destroy(g_mosq);
    mosquitto_lib_cleanup();
    return 0;
}
