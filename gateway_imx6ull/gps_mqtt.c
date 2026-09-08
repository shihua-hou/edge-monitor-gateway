/**
 * gps_mqtt.c - i.MX6ULL GPS 解析 + MQTT 发布（阶段 8）
 *
 * 功能：
 *   1. 打开串口（默认 /dev/ttymxc2，9600 8N1）
 *   2. 逐行解析 NMEA 0183：$GPGGA（经纬度/fix/卫星/海拔）+ $GPRMC（速度）
 *   3. 发布 JSON 到 monitor/data/gps：{"lat":..,"lon":..,"fix":1,"sat":..,"alt":..,"speed":..}
 *   4. Web 端订阅该 topic，Leaflet 地图实时打点
 *
 * 编译：
 *   gcc gps_mqtt.c -o gps_mqtt -lmosquitto
 *   arm-linux-gnueabihf-gcc gps_mqtt.c -o gps_mqtt -lmosquitto
 *
 * 运行：
 *   ./gps_mqtt /dev/ttymxc2
 *
 * 接线：GPS TX→板子 RX（交叉），GPS 3.3V，共地。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <mosquitto.h>

#define MQTT_HOST_DEFAULT "localhost"   /* 实际以 MQTT_HOST 环境变量为准 */
#define MQTT_PORT 1883
#define TP_GPS    "monitor/data/gps"

static struct mosquitto *g_mosq = NULL;

/* ============ 串口打开（9600 8N1） ============ */
static int open_serial(const char *dev)
{
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    struct termios tio;
    if (fd < 0) { perror("open serial"); return -1; }
    tcgetattr(fd, &tio);
    cfsetispeed(&tio, B9600);
    cfsetospeed(&tio, B9600);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CSIZE;  tio.c_cflag |= CS8;    /* 8 bit */
    tio.c_cflag &= ~PARENB; tio.c_cflag &= ~CSTOPB; /* 无校验 1停止 */
    tio.c_lflag &= ~(ICANON | ECHO | ISIG);
    tio.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL);
    tio.c_oflag &= ~OPOST;
    tcsetattr(fd, TCSANOW, &tio);
    return fd;
}

/* ============ NMEA 工具 ============ */
/* "4807.038" -> 48 + 07.038/60 = 48.1173 */
static double nmea_to_deg(const char *s)
{
    double d = atof(s);
    int deg = (int)(d / 100);
    double min = d - deg * 100;
    return deg + min / 60.0;
}

/* 按逗号拆字段，最多 16 个 */
static int split_nmea(char *line, char f[][32])
{
    int n = 0;
    char *p = strtok(line, ",");
    while (p && n < 16) {
        strncpy(f[n], p, 31);
        f[n][31] = 0;
        n++;
        p = strtok(NULL, ",");
    }
    return n;
}

/* ============ 解析并发布 ============ */
static void publish_gps(double lat, double lon, int fix, int sat,
                        double alt, double speed)
{
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"lat\":%.6f,\"lon\":%.6f,\"fix\":%d,\"sat\":%d,\"alt\":%.1f,\"speed\":%.1f}",
             lat, lon, fix, sat, alt, speed);
    mosquitto_publish(g_mosq, NULL, TP_GPS, strlen(payload), payload, 0, false);
    printf("[gps] %.6f,%.6f fix=%d sat=%d alt=%.1f speed=%.1fkm/h\n",
           lat, lon, fix, sat, alt, speed);
}

/* ============ 处理一行 NMEA ============ */
static void handle_line(char *line)
{
    char f[16][32];
    int n = split_nmea(line, f);
    if (n < 2) return;

    if (strcmp(f[0], "$GPGGA") == 0 && n >= 10) {
        int fix = atoi(f[6]);
        if (fix > 0 && strlen(f[2]) > 0) {
            double lat = nmea_to_deg(f[2]);
            double lon = nmea_to_deg(f[4]);
            if (f[3][0] == 'S') lat = -lat;
            if (f[5][0] == 'W') lon = -lon;
            int    sat = atoi(f[7]);
            double alt = atof(f[9]);
            /* 速度由 $GPRMC 更新，这里用全局；简单起见 alt/speed 用 0 兜底 */
            publish_gps(lat, lon, fix, sat, alt, 0.0);
        }
    } else if (strcmp(f[0], "$GPRMC") == 0 && n >= 8) {
        /* 有效定位且带速度时发布（可配合 GGA） */
        if (strcmp(f[2], "A") == 0 && strlen(f[3]) > 0) {
            double lat = nmea_to_deg(f[3]);
            double lon = nmea_to_deg(f[5]);
            if (f[4][0] == 'S') lat = -lat;
            if (f[6][0] == 'W') lon = -lon;
            double speed = atof(f[7]) * 1.852;   /* 节 → km/h */
            publish_gps(lat, lon, 1, 0, 0.0, speed);
        }
    }
}

/* ============ 主流程 ============ */
int main(int argc, char *argv[])
{
    const char *dev = (argc > 1) ? argv[1] : "/dev/ttymxc2";
    int fd;

    /* 行缓冲：常驻后台、stdout 重定向到日志文件时，
       全缓冲会让输出一直卡在缓冲区里，看不到任何日志 */
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
    setvbuf(stderr, NULL, _IOLBF, BUFSIZ);

    fd = open_serial(dev);
    if (fd < 0) return 1;

    mosquitto_lib_init();
    /* client_id 带上设备号：MQTT 不允许同一 broker 上重名，重名会互相踢，
       现象是连上就断、反复循环。多设备接同一个 broker 时必然踩到 */
    {
        const char *dev_id = getenv("DEVICE_ID");
        static char client_id[96];
        if (!dev_id || !dev_id[0]) dev_id = "robot01";
        snprintf(client_id, sizeof(client_id), "gps_%s", dev_id);
        g_mosq = mosquitto_new(client_id, true, NULL);
    }
    if (!g_mosq) { fprintf(stderr, "mosquitto_new fail\n"); return 1; }

    /* broker 地址/账号从环境变量取，跟 gateway_mqtt、ota_service 保持一致。
       原来这里写死 localhost，是"broker 跟网关同机"那版架构的遗留；
       broker 迁到服务器之后，写死 localhost 会直接连不上 */
    const char *mqtt_host = getenv("MQTT_HOST");
    if (!mqtt_host || !mqtt_host[0]) mqtt_host = MQTT_HOST_DEFAULT;
    const char *mqtt_user = getenv("MQTT_USER");
    const char *mqtt_pass = getenv("MQTT_PASS");
    if (mqtt_user && mqtt_pass) {
        mosquitto_username_pw_set(g_mosq, mqtt_user, mqtt_pass);
    } else {
        fprintf(stderr, "[warn] 未设置 MQTT_USER/MQTT_PASS，broker 若禁匿名将连接失败\n");
    }

    /* 连不上不退出：GPS 串口照读，等 broker 起来自动重连。
       loop_start 另起后台线程做收发和重连，主线程继续阻塞读串口 */
    if (mosquitto_connect(g_mosq, mqtt_host, MQTT_PORT, 60) != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[mqtt] 首次连接 %s:%d 失败，进入重连等待\n", mqtt_host, MQTT_PORT);
    }
    mosquitto_loop_start(g_mosq);
    printf("GPS reader on %s, publishing to %s (broker %s) ...\n", dev, TP_GPS, mqtt_host);

    /* 行缓冲解析（串口可能任意截断） */
    char line[128];
    int  lpos = 0;
    char buf[256];

    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                char c = buf[i];
                if (c == '\n' || c == '\r') {
                    if (lpos > 0) {
                        line[lpos] = 0;
                        handle_line(line);
                        lpos = 0;
                    }
                } else if (lpos < (int)sizeof(line) - 1) {
                    line[lpos++] = c;
                }
            }
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
            break;
        }
        usleep(10000);   /* 10ms 轮询 */
    }

    close(fd);
    mosquitto_destroy(g_mosq);
    mosquitto_lib_cleanup();
    return 0;
}
