/**
 * gateway_can.c - i.MX6ULL CAN 网关程序（阶段 2）
 * 功能：监听 can0，接收 STM32 上报帧，按协议解析并打印
 * 用法：
 *   ./gateway_can [can0]        # 默认 can0
 *
 * 编译（板载 gcc）：
 *   gcc gateway_can.c -o gateway_can
 *
 * 交叉编译（PC 上）：
 *   arm-linux-gnueabihf-gcc gateway_can.c -o gateway_can
 *
 * 运行前配置 can0：
 *   sudo ip link set can0 type can bitrate 500000
 *   sudo ip link set can0 up
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>      /* EINTR：区分"被信号打断"和真正的读取错误 */
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>

/* ===== 帧 ID（与 docs/CAN通信协议.md 一致） ===== */
#define CAN_ID_STATUS_REPORT   0x100
#define CAN_ID_IMU_FRAME       0x101
#define CAN_ID_MOTOR_STATE     0x102
#define CAN_ID_CTRL_CMD        0x110
#define CAN_ID_PARAM_SET       0x111

/* ===== 解析 0x100 状态上报 ===== */
static void parse_status(const struct can_frame *f)
{
    /* 温度是 data[0](高字节)+data[7](低字节) 大端两字节——跟 STM32 端/
       gateway_mqtt.c 同步改过，原来只用 data[0] 一字节 (int8_t) 强转，
       超过 12.7℃ 的正常室温都会变成负数 */
    int16_t temp = (int16_t)((f->data[0] << 8) | f->data[7]);  /* 有符号, 0.1℃ */
    uint8_t  light  = f->data[1];
    uint16_t dist   = (f->data[2] << 8) | f->data[3];
    uint8_t  state  = f->data[4];

    /* 用浮点打印，不要用 "%d.%d" 拆整数部分和小数部分。
       C 的整数除法和取模对负数都是向零取整：temp = -5（即 -0.5℃）时
       -5/10 = 0、-5%10 = -5，打出来是 "0.-5℃"——【符号整个丢了】；
       temp = -55 更直接打成 "-5.-5℃"。
       零下温度在户外巡检场景是常态，这个格式一到冬天就全错。 */
    printf("[0x100] 温度:%.1f℃ 光照:%d%% 距离:%dcm 状态字:0x%02X\n",
           temp / 10.0, light, dist, state);
}

/* ===== 解析 0x101 IMU 姿态 ===== */
static void parse_imu(const struct can_frame *f)
{
    int16_t pitch = (int16_t)((f->data[0] << 8) | f->data[1]);
    int16_t roll  = (int16_t)((f->data[2] << 8) | f->data[3]);
    int16_t yaw   = (int16_t)((f->data[4] << 8) | f->data[5]);

    /* 同 parse_status：姿态角本来就有正有负，而且【现在就是负的】——
       这块板子的横滚常年在 -125° 左右，原来的 "%d.%02d" 打出来是
       "-125.-33°"，这个工具的输出一直是错的，只是没人细看。 */
    printf("[0x101] 俯仰:%.2f° 横滚:%.2f° 偏航:%.2f°\n",
           pitch / 100.0, roll / 100.0, yaw / 100.0);
}

/* ===== 解析 0x102 小车运动状态 ===== */
static void parse_motor(const struct can_frame *f)
{
    /* 有符号（协议 v2.4）：倒车、差速转向、一侧轮子反转，都靠符号位区分。
       按 uint16_t 读的话，-30 RPM 会打成 65506 */
    int16_t rpm_l = (int16_t)((f->data[0] << 8) | f->data[1]);
    int16_t rpm_r = (int16_t)((f->data[2] << 8) | f->data[3]);
    uint16_t odom_l = (f->data[4] << 8) | f->data[5];
    uint16_t odom_r = (f->data[6] << 8) | f->data[7];

    printf("[0x102] 左轮:%dRPM 右轮:%dRPM 左里程:%dcm 右里程:%dcm\n",
           rpm_l, rpm_r, odom_l, odom_r);
}

/* ===== 发送控制命令 =====
 * 这个工具是只读的诊断程序，下行命令走 gateway_mqtt。这里保留一份最小
 * 实现，是为了在"怀疑 MQTT 那条链路有问题"时能绕开它直接对 CAN 发一帧。
 * 加 __attribute__((unused)) 消掉 -Wall 的 unused-function 告警：
 * 留着一个永远会告警的函数，久了会让人对告警整体麻木——而告警一旦被
 * 习惯性忽略，它就不再有任何价值。 */
__attribute__((unused))
static int send_ctrl(int s, uint8_t dev, uint8_t act, uint8_t p1, uint8_t p2)
{
    struct can_frame f;
    memset(&f, 0, sizeof(f));
    f.can_id = CAN_ID_CTRL_CMD;
    f.can_dlc = 4;
    f.data[0] = dev; f.data[1] = act; f.data[2] = p1; f.data[3] = p2;
    return write(s, &f, sizeof(f));
}

int main(int argc, char *argv[])
{
    const char *ifname = (argc > 1) ? argv[1] : "can0";
    int s;
    struct sockaddr_can addr;
    struct ifreq ifr;
    struct can_frame frame;

    /* 创建 RAW socket */
    s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) { perror("socket"); return 1; }

    /* 绑定到指定 CAN 接口。
       memset 不能省（gateway_mqtt.c 里专门修过同一个问题并写了注释，
       这个文件当时漏了）：ifreq 是个 union，ioctl 只填 ifr_ifindex，
       其余字节若是栈上的垃圾会一起传进内核；而且 strncpy 在源串刚好占满
       IFNAMSIZ-1 时不补结束符，接口名就没有终止。先清零两个结构体，
       这两件事一起解决。 */
    memset(&ifr, 0, sizeof(ifr));
    memset(&addr, 0, sizeof(addr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) { perror("ioctl"); close(s); return 1; }

    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(s); return 1;
    }

    printf("CAN gateway listening on %s ... (Ctrl+C 退出)\n", ifname);

    while (1) {
        ssize_t n = read(s, &frame, sizeof(frame));
        if (n < 0) {
            if (errno == EINTR) continue;          /* 被信号打断，不是错误 */
            /* 持续性错误（最常见的是接口被 down 掉，read 一直返回 ENETDOWN）
               下，光 continue 会变成 100% CPU 的忙循环，同时把错误刷满屏幕。
               歇一秒再试：既不占 CPU，也让错误信息保持可读的节奏。
               这是个诊断工具，不做自动恢复——接口没了就该让人看见，
               自动重连反而会把"接口断过"这个事实藏起来。 */
            perror("read");
            sleep(1);
            continue;
        }
        if (n < (ssize_t)sizeof(struct can_frame)) continue;

        switch (frame.can_id & CAN_EFF_MASK) {
        case CAN_ID_STATUS_REPORT: parse_status(&frame); break;
        case CAN_ID_IMU_FRAME:     parse_imu(&frame);    break;
        case CAN_ID_MOTOR_STATE:   parse_motor(&frame);  break;
        default:
            printf("[0x%03X] ", frame.can_id & CAN_SFF_MASK);
            for (int i = 0; i < frame.can_dlc; i++) printf("%02X ", frame.data[i]);
            printf("\n");
            break;
        }
        fflush(stdout);
    }

    close(s);
    return 0;
}
