/**
 * video_v4l2.c - 自写 V4L2 摄像头采集 + HTTP MJPEG 推流（阶段 7）
 *
 * 功能：
 *   1. V4L2 打开 /dev/videoX，协商 MJPEG 格式（UVC 摄像头硬件编码 JPEG）
 *   2. mmap 映射帧缓冲，采集线程循环取帧
 *   3. 主线程 select 管理多 HTTP 客户端，打包 multipart/x-mixed-replace 推流
 *   4. 浏览器 <img src="http://IP:PORT/?action=stream"> 直接显示
 *
 * 编译（交叉）：
 *   arm-linux-gnueabihf-gcc video_v4l2.c -o video_v4l2 -lpthread
 *
 * 运行：
 *   ./video_v4l2 /dev/video0 8081 [token]
 *
 * v2 变更：URL token 鉴权，浏览器访问需带 ?token=xxx
 * v2.1 变更：token 不再直接用登录密码明文——密码一旦出现在 URL 里就会被浏览器
 *   历史、代理/服务器访问日志、Referer 头记下来。这里的 token 应该填
 *   SHA-256("用户名:密码") 十六进制串的前 16 位（web/index.html 的
 *   deriveVideoToken() 用同样的算法在浏览器端算，两边必须一致）：
 *     python3 -c "import hashlib;print(hashlib.sha256(b'em:yourpass').hexdigest()[:16])"
 *
 * 依赖：仅 Linux 标准头 + pthread，无第三方库（摄像头需支持 MJPEG）。
 * 若摄像头只支持 YUYV：需在采集线程加 libjpeg 压缩（apt install libjpeg-dev, -ljpeg）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>       /* time() —— 待鉴权连接的超时回收 */
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/videodev2.h>

#define MAX_CLIENTS      8            /* 最大同时观看人数 */
#define MAX_FRAME       (1024*1024)   /* 单帧上限 1MB */
#define PIX_W           640
#define PIX_H           480
#define BOUNDARY        "frame"
#define HEADER_TOP      "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=" BOUNDARY "\r\n\r\n"
#define HEADER_FRAME    "--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n"

/* ============ 全局共享：最新帧（采集线程写入，推流主线程读取） ============ */
static unsigned char *g_frame = NULL;    /* 最新一帧 MJPEG 数据 */
static int   g_frame_len = 0;
static int   g_frame_seq = 0;            /* 帧序号，主线程据此判断是否有新帧 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_running = 1;

/* ============ V4L2 设备状态 ============ */
static int dev_fd = -1;
static void *buf_start[4];
static size_t buf_len[4];
static int nbufs = 0;

/* ============ v2: HTTP token 鉴权（校验 URL ?token=xxx） ============ */
static int http_token_ok(const char *req, const char *expect)
{
    const char *p;
    if (!expect || expect[0] == 0) return 1;      /* 未配置 token 则放行（联调用） */
    p = strstr(req, "token=");
    if (!p) return 0;
    p += 6;
    /* 逐字符比较到 & / 空格 / 换行 */
    while (*p && *p != '&' && *p != ' ' && *p != '\r' && *p != '\n') {
        if (*p != *expect) return 0;
        p++; expect++;
    }
    return (*expect == 0);                        /* 必须完全匹配 */
}

static void sig_handler(int s) { g_running = 0; }

/* ioctl 封装：自动重试 EINTR */
static int xioctl(int fd, int req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

/* ============ V4L2 初始化：打开 + MJPEG 协商 + mmap 缓冲 ============ */
static int v4l2_init(const char *dev, int w, int h)
{
    struct v4l2_capability  cap;
    struct v4l2_format      fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer      buf;

    dev_fd = open(dev, O_RDWR | O_NONBLOCK);
    if (dev_fd < 0) { perror("open video"); return -1; }

    if (xioctl(dev_fd, VIDIOC_QUERYCAP, &cap) < 0) { perror("QUERYCAP"); return -1; }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) { fprintf(stderr, "not a capture device\n"); return -1; }
    if (!(cap.capabilities & V4L2_CAP_STREAMING))     { fprintf(stderr, "no streaming support\n"); return -1; }

    /* 协商 MJPEG 格式（UVC 硬件直接输出 JPEG，无需软件压缩） */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = w;
    fmt.fmt.pix.height      = h;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    if (xioctl(dev_fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT(MJPEG)"); return -1; }

    printf("format: %dx%d  fourcc=%c%c%c%c\n",
           fmt.fmt.pix.width, fmt.fmt.pix.height,
           fmt.fmt.pix.pixelformat & 0xff,
           (fmt.fmt.pix.pixelformat >> 8)  & 0xff,
           (fmt.fmt.pix.pixelformat >> 16) & 0xff,
           (fmt.fmt.pix.pixelformat >> 24) & 0xff);

    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
        fprintf(stderr, "[!] 摄像头未输出 MJPEG（可能只支持 YUYV），请检查:\n"
                        "    v4l2-ctl --list-formats -d %s\n", dev);
        return -1;
    }

    /* 申请 mmap 缓冲 */
    memset(&req, 0, sizeof(req));
    req.count  = 4;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(dev_fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return -1; }
    nbufs = req.count;

    for (int i = 0; i < nbufs; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (xioctl(dev_fd, VIDIOC_QUERYBUF, &buf) < 0) { perror("QUERYBUF"); return -1; }
        buf_len[i]   = buf.length;
        buf_start[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, dev_fd, buf.m.offset);
        if (buf_start[i] == MAP_FAILED) { perror("mmap"); return -1; }
        /* 入队，等待驱动填充 */
        if (xioctl(dev_fd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF"); return -1; }
    }
    return 0;
}


/* ============ 自动找 USB 摄像头 ============
 *
 * 为什么不写死 /dev/video2：
 * 实测过——USB 摄像头在总线上掉线一下（供电抖动时会这样），一秒后又
 * 重新枚举回来，但**编号变成了 video3**，video2 已经不存在。
 * 写死编号的话，摄像头明明好了，服务却永远打开不到它。
 * （RTK 模块的串口 ttyACM3 是同一类问题，见 gps_mqtt.c 的 find_nmea_port。）
 *
 * 这块板子上的 video 节点不止一个：video0 是 PxP（图像处理单元）、
 * video1 是板载 CSI 并口摄像头接口，都不是我们要的。
 * 按**驱动名 uvcvideo** 找，比按编号或按设备名都可靠——
 * 换一个别的牌子的 USB 摄像头，名字会变，驱动不会。
 */
static int find_uvc_camera(char *out, size_t outsz)
{
    int i;
    for (i = 0; i < 16; i++) {
        char link[128], target[256];
        ssize_t n;
        const char *base;
        snprintf(link, sizeof(link), "/sys/class/video4linux/video%d/device/driver", i);
        n = readlink(link, target, sizeof(target) - 1);
        if (n <= 0) continue;
        target[n] = 0;
        base = strrchr(target, '/');
        base = base ? base + 1 : target;
        if (strcmp(base, "uvcvideo") != 0) continue;

        /* 新内核的 UVC 设备会多出一个"元数据"节点，它也挂在 uvcvideo 下，
           但不能采图。用 QUERYCAP 过滤一下，只要真能 CAPTURE 的那个。 */
        {
            char dev[32];
            struct v4l2_capability cap;
            int fd;
            snprintf(dev, sizeof(dev), "/dev/video%d", i);
            fd = open(dev, O_RDWR | O_NONBLOCK);
            if (fd < 0) continue;
            memset(&cap, 0, sizeof(cap));
            if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
                unsigned caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                              ? cap.device_caps : cap.capabilities;
                close(fd);
                if (caps & V4L2_CAP_VIDEO_CAPTURE) {
                    snprintf(out, outsz, "%s", dev);
                    return 0;
                }
                continue;
            }
            close(fd);
        }
    }
    return -1;
}

/* ============ 采集线程：取帧 → 存全局最新帧 ============ */
static void *capture_thread(void *arg)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct v4l2_buffer buf;
    struct timeval tv;
    fd_set fds;

    if (xioctl(dev_fd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return NULL; }
    printf("[capture] streaming started\n");

    while (g_running) {
        /* 等待帧就绪（非阻塞 DQBUF 在 EAGAIN 时 select 等待） */
        FD_ZERO(&fds);
        FD_SET(dev_fd, &fds);
        tv.tv_sec  = 1;
        tv.tv_usec = 0;
        if (select(dev_fd + 1, &fds, NULL, NULL, &tv) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (!FD_ISSET(dev_fd, &fds)) continue;

        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(dev_fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            perror("DQBUF");
            /* **采集失败就让整个进程退出**，别只结束这个线程。
               原来是 break 出循环、线程结束——但 HTTP 那边还活着：
               端口在监听、浏览器连得上、连接也建立了，就是永远不给画面。
               从外面看"服务在跑"，其实早就死了一半，这是最难发现的状态。
               进程退出后守护脚本 10 秒内拉起，启动时重新找摄像头，
               摄像头换了编号也能接上。
               不在进程内重新初始化：要拆 mmap、要处理已连着的观众，
               复杂度远高于"重启一次"，而重启只要几秒。 */
            fprintf(stderr, "[capture] 摄像头采集失败（%s），退出等待守护脚本重启\n",
                    strerror(errno));
            exit(2);
        }

        /* 拷贝到全局缓冲（加锁） */
        pthread_mutex_lock(&g_lock);
        if (buf.bytesused <= MAX_FRAME) {
            memcpy(g_frame, buf_start[buf.index], buf.bytesused);
            g_frame_len = buf.bytesused;
            g_frame_seq++;
        }
        pthread_mutex_unlock(&g_lock);

        /* 归还缓冲 */
        if (xioctl(dev_fd, VIDIOC_QBUF, &buf) < 0) {
            perror("QBUF");
            fprintf(stderr, "[capture] 归还缓冲失败，退出等待重启\n");
            exit(2);
        }
    }

    xioctl(dev_fd, VIDIOC_STREAMOFF, &type);
    return NULL;
}

/* ============ 主线程：HTTP 多客户端 MJPEG 推流 ============ */
int main(int argc, char *argv[])
{
    /* 设备参数：不给或给 "auto" 就按驱动名自动找 USB 摄像头（推荐）；
       也可以直接给 /dev/videoN，调试时用。 */
    const char *devarg = (argc > 1) ? argv[1] : "auto";
    char devbuf[32];
    const char *dev = devarg;
    int         port = (argc > 2) ? atoi(argv[2]) : 8081;
    const char *token = (argc > 3) ? argv[3] : "";   /* v2: 访问令牌 */

    /* 行缓冲：常驻后台、stdout 重定向到日志文件时，
       全缓冲会让输出一直卡在缓冲区里，看不到任何日志 */
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
    setvbuf(stderr, NULL, _IOLBF, BUFSIZ);

    int listen_fd, i, maxfd, ret;
    int clients[MAX_CLIENTS];
    /* 已 accept 但请求行还没到、尚未通过 token 校验的连接。
       跟 clients[] 分开放：它们还不能收帧，混在一起会误推数据给未鉴权的连接 */
    int    pend_fd[MAX_CLIENTS];
    time_t pend_at[MAX_CLIENTS];   /* 登记时刻，用于超时回收 */
    struct sockaddr_in addr;
    fd_set rfds;
    int last_sent_seq = -1;
    unsigned char *sendbuf = malloc(MAX_FRAME);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    if (!sendbuf) { fprintf(stderr, "malloc fail\n"); return 1; }

    if (strcmp(devarg, "auto") == 0) {
        if (find_uvc_camera(devbuf, sizeof(devbuf)) != 0) {
            /* 找不到也不要立刻退出狂刷：摄像头可能正在重新枚举。
               退出码非 0，守护脚本下一轮会再拉起来重试。 */
            fprintf(stderr, "[capture] 没找到 USB 摄像头（uvcvideo），稍后由守护脚本重试\n");
            sleep(5);
            return 1;
        }
        dev = devbuf;
    }
    printf("[capture] 使用摄像头 %s\n", dev);

    /* V4L2 初始化 */
    if (v4l2_init(dev, PIX_W, PIX_H) < 0) return 1;
    g_frame = malloc(MAX_FRAME);
    if (!g_frame) return 1;

    /* 启动采集线程 */
    pthread_t tid;
    pthread_create(&tid, NULL, capture_thread, NULL);

    /* HTTP 监听 */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }
    int on = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(listen_fd, MAX_CLIENTS) < 0) { perror("listen"); return 1; }
    printf("[http ] listening on port %d\n", port);
    printf("open in browser: http://<IP>:%d/?action=stream\n", port);

    for (i = 0; i < MAX_CLIENTS; i++) clients[i] = -1;
    for (i = 0; i < MAX_CLIENTS; i++) { pend_fd[i] = -1; pend_at[i] = 0; }

    /* 主循环：select 监听 + 有新帧则广播 */
    while (g_running) {
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        maxfd = listen_fd;
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i] >= 0) {
                FD_SET(clients[i], &rfds);
                if (clients[i] > maxfd) maxfd = clients[i];
            }
        }
        /* 待鉴权的连接也放进 select：它们的请求行还没到，要等可读 */
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (pend_fd[i] >= 0) {
                FD_SET(pend_fd[i], &rfds);
                if (pend_fd[i] > maxfd) maxfd = pend_fd[i];
            }
        }
        struct timeval tv = { 0, 100000 };   /* 100ms 轮询帧 */
        ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) { if (errno == EINTR) continue; break; }

        /* 接受新连接。
           这里【只登记，不读】——原来是 accept 之后立刻同步 read 请求行，
           带 5 秒读超时。问题在于这是个单线程 select 循环：一个连上却不发
           数据的客户端（半开连接、扫端口的、网络中途卡住的）会把整个循环
           堵死 5 秒，期间所有正在观看的客户端一帧都收不到——一个坏连接
           拖垮全部观看者，是无意间造出来的拒绝服务。
           改成先扔进待鉴权表，等 select 说它可读了再读，读操作就永远不会
           阻塞主循环；一直不说话的连接由下面的超时统一回收。 */
        if (FD_ISSET(listen_fd, &rfds)) {
            int c = accept(listen_fd, NULL, NULL);
            if (c >= 0) {
                for (i = 0; i < MAX_CLIENTS; i++) {
                    if (pend_fd[i] < 0) { pend_fd[i] = c; pend_at[i] = time(NULL); break; }
                }
                if (i == MAX_CLIENTS) {   /* 待鉴权坑位也满了，直接拒绝 */
                    write(c, "HTTP/1.1 503 Busy\r\n\r\n", 21);
                    close(c);
                }
            }
        }

        /* 处理待鉴权连接：只有 select 说可读时才去读，不会阻塞 */
        for (i = 0; i < MAX_CLIENTS; i++) {
            int c = pend_fd[i];
            if (c < 0) continue;

            if (!FD_ISSET(c, &rfds)) {
                /* 还没数据。超过 5 秒就当它不打算说话了，回收掉——
                   没有这个清理，静默连接会一直占着坑位不放 */
                if (time(NULL) - pend_at[i] > 5) {
                    printf("[http ] auth timeout, drop (%d)\n", c);
                    close(c);
                    pend_fd[i] = -1;
                }
                continue;
            }

            {
                char req[512];
                ssize_t rn = read(c, req, sizeof(req) - 1);
                int slot;
                req[(rn > 0 && rn < (ssize_t)sizeof(req)) ? rn : 0] = 0;
                pend_fd[i] = -1;               /* 无论结果如何，先摘出待鉴权表 */

                if (rn <= 0 || !http_token_ok(req, token)) {
                    write(c, "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n", 48);
                    printf("[http ] auth denied (%d)\n", c);
                    close(c);
                    continue;
                }
                for (slot = 0; slot < MAX_CLIENTS; slot++)
                    if (clients[slot] < 0) { clients[slot] = c; break; }
                if (slot == MAX_CLIENTS) {     /* 观看名额已满 */
                    write(c, "HTTP/1.1 503 Busy\r\n\r\n", 21);
                    close(c);
                } else {
                    write(c, HEADER_TOP, strlen(HEADER_TOP));
                    printf("[http ] client joined (%d)\n", c);
                    /* 必须把它从 rfds 里摘掉。
                       这个 fd 之所以在 rfds 里，是因为 select 说"它有请求可读"，
                       而那份数据上面刚刚已经读走了。不清掉的话，紧接着的
                       "检测断开"循环会看到 FD_ISSET 为真，对一个已经没有数据的
                       socket 再做一次【阻塞 read】——整个单线程 select 循环就此
                       冻住，直到对端超时关闭为止。
                       表现是：浏览器一连上，所有观看者（包括新连接）全都收不到
                       任何数据，日志里只见 client joined 紧跟着 client left。
                       本次改动本意是消除"一个坏连接拖垮所有观看者"，漏了这一下
                       反而变成"每一个正常连接都拖垮所有人"。 */
                    FD_CLR(c, &rfds);
                }
            }
        }

        /* 检测断开：客户端 socket 可读说明对端关闭 */
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i] >= 0 && FD_ISSET(clients[i], &rfds)) {
                char tmp[64];
                /* 用 recv(MSG_DONTWAIT) 而不是 read()：这只是一次"探一下对端
                   是不是关了"的探测，绝不能因为它把整个服务卡住。
                   上面的 FD_CLR 已经堵住了已知的那条路径，但探测性读取本身
                   就不该有阻塞的可能——select 的可读标志随时可能是过期的
                   （同一轮里被别的分支消费掉了就会这样）。
                   EAGAIN 表示"暂时没数据但连接还在"，不是断开。 */
                ssize_t rn2 = recv(clients[i], tmp, sizeof(tmp), MSG_DONTWAIT);
                if (rn2 == 0 || (rn2 < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    close(clients[i]);
                    printf("[http ] client left (%d)\n", clients[i]);
                    clients[i] = -1;
                }
            }
        }

        /* 有新帧则向所有客户端推送 */
        pthread_mutex_lock(&g_lock);
        int has_new = (g_frame_seq != last_sent_seq);
        int flen = g_frame_len;
        if (has_new && flen > 0) {
            memcpy(sendbuf, g_frame, flen);
            last_sent_seq = g_frame_seq;
        }
        pthread_mutex_unlock(&g_lock);

        if (has_new && flen > 0) {
            char hdr[128];
            int hlen = snprintf(hdr, sizeof(hdr), HEADER_FRAME, flen);
            for (i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i] < 0) continue;
                if (write(clients[i], hdr, hlen) < 0) {
                    close(clients[i]); clients[i] = -1; continue;
                }
                if (write(clients[i], sendbuf, flen) < 0) {
                    close(clients[i]); clients[i] = -1; continue;
                }
                if (write(clients[i], "\r\n", 2) < 0) {
                    close(clients[i]); clients[i] = -1;
                }
            }
        }
    }

    /* 清理 */
    for (i = 0; i < MAX_CLIENTS; i++)
        if (clients[i] >= 0) close(clients[i]);
    /* 待鉴权的连接同样要关。虽然进程退出时内核会回收所有 fd，
       但显式关掉能让"哪些资源归谁管"在代码里一目了然，
       以后这段逻辑被搬进别的长生命周期对象时也不会漏 */
    for (i = 0; i < MAX_CLIENTS; i++)
        if (pend_fd[i] >= 0) close(pend_fd[i]);
    close(listen_fd);
    for (i = 0; i < nbufs; i++)
        munmap(buf_start[i], buf_len[i]);
    close(dev_fd);
    printf("exit\n");
    return 0;
}
