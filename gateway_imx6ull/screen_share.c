/**
 * screen_share.c - 6U 屏幕远程查看 + 远程触摸
 *
 * 做什么：
 *   1. 抓 /dev/fb0 的画面，降采样后通过 HTTP 发给浏览器（远程"看屏幕"）
 *   2. 接收浏览器发来的坐标，写 /dev/input/eventN 注入触摸（远程"点屏幕"）
 *
 * 为什么不用 VNC：板子是 linuxfb 直出，根本没跑 X server，x11vnc 这类
 * 工具无从下手；直接抓 framebuffer 反而是最短路径，也不用在板子上多装东西。
 *
 * 为什么发原始 RGB565 而不是 JPEG：板子上不一定有 libjpeg，为了零依赖，
 * 这里降采样之后直接把 RGB565 字节发出去，由浏览器用 Canvas 转成图像。
 * 1024x600 原始帧是 1.2MB，2x2 降采样后 512x300 = 300KB，配合 1~2fps
 * 的刷新率在 WiFi 上够用了——这个功能是"远程看一眼屏幕/点两下"，
 * 不是看视频，不需要高帧率。
 *
 * 编译：
 *   arm-linux-gnueabihf-gcc -Wall screen_share.c -o screen_share -lpthread
 *
 * 运行：
 *   ./screen_share [端口] [token] [触摸设备] [固件目录]
 *   ./screen_share 8082 abc123 /dev/input/event1 /home/root/
 *
 * 接口：
 *   GET  /frame?token=xxx                     取一帧（有 libjpeg 则为 JPEG，
 *                                             否则是降采样 RGB565 原始数据）
 *   POST /touch?token=xxx&x=&y=               在 (x,y) 点一下
 *   POST /touch?token=xxx&x=&y=&x2=&y2=       从 (x,y) 滑到 (x2,y2)
 *   POST /upload?token=xxx&name=xx.bin        上传固件，body 为文件原始字节
 *   GET  /fwlist?token=xxx                    列出固件目录里的 .bin
 *   GET  /info?token=xxx                      屏幕尺寸等信息
 *
 * 编译（板子有 libjpeg 时加 -DUSE_JPEG，画面全分辨率且更省带宽）：
 *   $CC -Wall -DUSE_JPEG screen_share.c -o screen_share -ljpeg -lpthread
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>      /* struct timeval，给 socket 设收发超时用 */
#include <netinet/in.h>
#include <linux/fb.h>
#include <linux/input.h>

#ifdef USE_JPEG
#include <jpeglib.h>
#endif

#define FB_DEV        "/dev/fb0"
#define TOUCH_DEV     "/dev/input/event1"   /* goodix-ts，见 /proc/bus/input/devices */
#define MAX_CLIENTS   4

/* 降采样倍数。有 libjpeg 时不降采样（全分辨率 + JPEG 压缩后反而更小、更清楚），
   没有时降一半，否则 1024x600 的原始 RGB565 一帧 1.2MB，帧率根本上不去 */
#ifdef USE_JPEG
#define DOWNSCALE     1
#define JPEG_QUALITY  72   /* 屏幕内容以文字线条为主，72 已经很清楚，再高只是白涨体积 */
#else
#define DOWNSCALE     2
#endif

static int   g_fb_fd = -1;
static uint8_t *g_fb_mem = NULL;
static size_t g_fb_size = 0;
static int   g_w = 0, g_h = 0, g_bpp = 0, g_line_len = 0;
static int   g_out_w = 0, g_out_h = 0;
static char  g_token[64] = "";
static const char *g_touch_dev = TOUCH_DEV;
/* 固件上传落地目录，要和 uds_tool 找固件的位置一致，末尾必须带 '/' */
static char  g_fw_dir[192] = "/home/root/";

/* 降采样后的一帧缓冲，所有请求共用，加锁保护 */
static uint16_t *g_frame = NULL;
static pthread_mutex_t g_frame_lock = PTHREAD_MUTEX_INITIALIZER;

/* ============ framebuffer ============ */
static int fb_open(void)
{
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    g_fb_fd = open(FB_DEV, O_RDONLY);
    if (g_fb_fd < 0) { perror("open " FB_DEV); return -1; }
    if (ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) { perror("FBIOGET_VSCREENINFO"); return -1; }
    if (ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) { perror("FBIOGET_FSCREENINFO"); return -1; }

    g_w = vinfo.xres;
    g_h = vinfo.yres;
    g_bpp = vinfo.bits_per_pixel;
    g_line_len = finfo.line_length;

    if (g_bpp != 16) {
        /* 这块屏实测是 RGB565。其它色深不是不能做，但转换分支得另写，
           先明确报错，免得悄悄输出一堆花屏 */
        fprintf(stderr, "只支持 16bpp RGB565，当前是 %d bpp\n", g_bpp);
        return -1;
    }

    /* 映射的是可见区域那一屏。vinfo.yres_virtual 可能是 yres 的两倍
       （双缓冲），但我们只读当前显示的这一屏 */
    g_fb_size = (size_t)g_line_len * g_h;
    g_fb_mem = mmap(NULL, g_fb_size, PROT_READ, MAP_SHARED, g_fb_fd, 0);
    if (g_fb_mem == MAP_FAILED) { perror("mmap fb"); g_fb_mem = NULL; return -1; }

    g_out_w = g_w / DOWNSCALE;
    g_out_h = g_h / DOWNSCALE;
    g_frame = malloc((size_t)g_out_w * g_out_h * 2);
    if (!g_frame) return -1;

    printf("framebuffer %dx%d %dbpp (line=%d) -> 输出 %dx%d\n",
           g_w, g_h, g_bpp, g_line_len, g_out_w, g_out_h);
    return 0;
}

/* 抓一帧并降采样。取样而不是求平均：屏幕内容是文字和线条，
   平均会让细线糊掉，直接取点反而更清楚，也快得多 */
static void fb_grab(void)
{
    int y, x;
    pthread_mutex_lock(&g_frame_lock);
    for (y = 0; y < g_out_h; y++) {
        const uint16_t *src = (const uint16_t *)(g_fb_mem + (size_t)(y * DOWNSCALE) * g_line_len);
        uint16_t *dst = g_frame + (size_t)y * g_out_w;
        for (x = 0; x < g_out_w; x++) {
            dst[x] = src[x * DOWNSCALE];
        }
    }
    pthread_mutex_unlock(&g_frame_lock);
}

#ifdef USE_JPEG
/* RGB565 → JPEG。压出来的一帧大约 40~80KB，比原始数据小一个数量级，
   所以有 libjpeg 时就不降采样了：全分辨率 + 压缩，又清楚又省带宽。
   返回 malloc 的缓冲，调用方负责 free */
static uint8_t *encode_jpeg(const uint16_t *src, int w, int h, unsigned long *out_len)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    uint8_t *outbuf = NULL;
    uint8_t *row = malloc((size_t)w * 3);
    int y, x;

    if (!row) return NULL;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &outbuf, out_len);
    cinfo.image_width = w;
    cinfo.image_height = h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, JPEG_QUALITY, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    for (y = 0; y < h; y++) {
        const uint16_t *s = src + (size_t)y * w;
        for (x = 0; x < w; x++) {
            uint16_t v = s[x];
            uint8_t r5 = (v >> 11) & 0x1F, g6 = (v >> 5) & 0x3F, b5 = v & 0x1F;
            /* 低位用高位补齐，不是直接补 0——否则纯白 0xFFFF 会变成
               248,252,248 这种发灰的白，整屏看着蒙了一层灰 */
            row[x * 3 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
            row[x * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
            row[x * 3 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
        }
        JSAMPROW rp = row;
        jpeg_write_scanlines(&cinfo, &rp, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    free(row);
    return outbuf;
}
#endif

/* ============ 触摸注入 ============
 * Goodix 这类电容屏走的是多点触控 Type B 协议。为了兼容性，
 * 这里把 MT 事件和传统单点事件(BTN_TOUCH + ABS_X/Y)一起发：
 * 有的应用只认 MT，有的只认单点，两套都发最保险。
 */
static int emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    /* time 留 0 由内核填 */
    return write(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev) ? 0 : -1;
}

static int touch_tap(int x, int y)
{
    int fd = open(g_touch_dev, O_WRONLY);
    if (fd < 0) { perror("open touch dev"); return -1; }

    /* 按下 */
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, 1);
    emit(fd, EV_ABS, ABS_MT_POSITION_X, x);
    emit(fd, EV_ABS, ABS_MT_POSITION_Y, y);
    emit(fd, EV_ABS, ABS_MT_TOUCH_MAJOR, 10);
    emit(fd, EV_KEY, BTN_TOUCH, 1);
    emit(fd, EV_ABS, ABS_X, x);
    emit(fd, EV_ABS, ABS_Y, y);
    emit(fd, EV_SYN, SYN_REPORT, 0);

    usleep(60 * 1000);   /* 按住 60ms，太短的话有些控件不认这次点击 */

    /* 抬起 */
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit(fd, EV_KEY, BTN_TOUCH, 0);
    emit(fd, EV_SYN, SYN_REPORT, 0);

    close(fd);
    printf("[touch] tap (%d,%d)\n", x, y);
    return 0;
}

/* 滑动。浏览器里在画面上滚滚轮就走这里——远程操作时"滚动列表"和
   "点一下"同样常用，只有 tap 的话像设置页那种长列表根本翻不动。
   中间要发若干个移动事件，直接从起点跳到终点的话，应用侧会当成
   一次瞬移，惯性滚动之类的手势识别不出来 */
static int touch_swipe(int x1, int y1, int x2, int y2)
{
    const int STEPS = 8;
    int fd = open(g_touch_dev, O_WRONLY);
    int i;
    if (fd < 0) { perror("open touch dev"); return -1; }

    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, 1);
    emit(fd, EV_ABS, ABS_MT_POSITION_X, x1);
    emit(fd, EV_ABS, ABS_MT_POSITION_Y, y1);
    emit(fd, EV_ABS, ABS_MT_TOUCH_MAJOR, 10);
    emit(fd, EV_KEY, BTN_TOUCH, 1);
    emit(fd, EV_ABS, ABS_X, x1);
    emit(fd, EV_ABS, ABS_Y, y1);
    emit(fd, EV_SYN, SYN_REPORT, 0);

    for (i = 1; i <= STEPS; i++) {
        int x = x1 + (x2 - x1) * i / STEPS;
        int y = y1 + (y2 - y1) * i / STEPS;
        emit(fd, EV_ABS, ABS_MT_POSITION_X, x);
        emit(fd, EV_ABS, ABS_MT_POSITION_Y, y);
        emit(fd, EV_ABS, ABS_X, x);
        emit(fd, EV_ABS, ABS_Y, y);
        emit(fd, EV_SYN, SYN_REPORT, 0);
        usleep(12 * 1000);
    }

    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit(fd, EV_KEY, BTN_TOUCH, 0);
    emit(fd, EV_SYN, SYN_REPORT, 0);

    close(fd);
    printf("[touch] swipe (%d,%d)->(%d,%d)\n", x1, y1, x2, y2);
    return 0;
}

/* ============ HTTP ============ */
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
    if (!g_token[0]) return 1;             /* 没配 token 就不校验 */
    if (qs_get(req, "token", tok, sizeof(tok)) != 0) return 0;
    return strcmp(tok, g_token) == 0;
}

static void send_simple(int fd, const char *status, const char *ctype, const char *body)
{
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n", status, ctype, (int)strlen(body));
    write(fd, hdr, n);
    write(fd, body, strlen(body));
}

static void handle_frame(int fd)
{
    char hdr[512];
    int n;

    fb_grab();

#ifdef USE_JPEG
    {
        unsigned long jlen = 0;
        uint8_t *jbuf;
        pthread_mutex_lock(&g_frame_lock);
        jbuf = encode_jpeg(g_frame, g_out_w, g_out_h, &jlen);
        pthread_mutex_unlock(&g_frame_lock);
        if (!jbuf) { send_simple(fd, "500 Internal Server Error", "text/plain", "jpeg encode failed"); return; }
        n = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %lu\r\n"
            "X-Frame-Width: %d\r\n"
            "X-Frame-Height: %d\r\n"
            "X-Screen-Width: %d\r\n"
            "X-Screen-Height: %d\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Expose-Headers: X-Frame-Width,X-Frame-Height,X-Screen-Width,X-Screen-Height\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n\r\n",
            jlen, g_out_w, g_out_h, g_w, g_h);
        write(fd, hdr, n);
        write(fd, jbuf, jlen);
        free(jbuf);
        return;
    }
#else
    size_t bytes = (size_t)g_out_w * g_out_h * 2;

    n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %zu\r\n"
        "X-Frame-Width: %d\r\n"
        "X-Frame-Height: %d\r\n"
        "X-Screen-Width: %d\r\n"
        "X-Screen-Height: %d\r\n"
        /* 这几个自定义头要显式放行，否则浏览器跨域时读不到 */
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Expose-Headers: X-Frame-Width,X-Frame-Height,X-Screen-Width,X-Screen-Height\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        bytes, g_out_w, g_out_h, g_w, g_h);
    write(fd, hdr, n);

    pthread_mutex_lock(&g_frame_lock);
    write(fd, g_frame, bytes);
    pthread_mutex_unlock(&g_frame_lock);
#endif
}

/* 坐标夹到屏幕范围内：越界坐标写进 input 子系统，轻则事件被丢弃，
   重则让上层应用收到莫名其妙的触摸位置 */
static void clamp_xy(int *x, int *y)
{
    if (*x < 0) *x = 0;
    if (*x >= g_w) *x = g_w - 1;
    if (*y < 0) *y = 0;
    if (*y >= g_h) *y = g_h - 1;
}

static void handle_touch(int fd, const char *req)
{
    char xs[16], ys[16], x2s[16], y2s[16];
    int x, y, ok;

    if (qs_get(req, "x", xs, sizeof(xs)) != 0 || qs_get(req, "y", ys, sizeof(ys)) != 0) {
        send_simple(fd, "400 Bad Request", "text/plain", "missing x/y");
        return;
    }
    x = atoi(xs); y = atoi(ys);
    clamp_xy(&x, &y);

    /* 带 x2/y2 就是滑动，否则是点击 */
    if (qs_get(req, "x2", x2s, sizeof(x2s)) == 0 && qs_get(req, "y2", y2s, sizeof(y2s)) == 0) {
        int x2 = atoi(x2s), y2 = atoi(y2s);
        clamp_xy(&x2, &y2);
        ok = (touch_swipe(x, y, x2, y2) == 0);
    } else {
        ok = (touch_tap(x, y) == 0);
    }

    if (ok) send_simple(fd, "200 OK", "text/plain", "ok");
    else send_simple(fd, "500 Internal Server Error", "text/plain", "touch inject failed");
}

/* ============ 固件上传 ============
 * 让浏览器能直接把固件传到板子，省掉"先 scp 到板子再远程升级"这一步。
 * 复用这个已有的 HTTP 服务，而不是再起一个进程——板子上常驻进程已经不少了。
 *
 * 协议故意用最简单的形式：POST /upload?name=xxx.bin，body 就是文件原始字节。
 * 不用 multipart/form-data——那个要解析 boundary、逐段扫描，在 C 里写一套
 * 健壮的解析器不划算，而这里只需要传一个文件。
 */
#define UPLOAD_MAX  (512 * 1024)   /* 固件槽才 240KB，512KB 足够且能挡住误传大文件 */

static int safe_filename(const char *name)
{
    size_t i, n = strlen(name);
    if (n == 0 || n > 64) return 0;
    if (name[0] == '.') return 0;                 /* 挡掉 .. 和隐藏文件 */
    for (i = 0; i < n; i++) {
        char c = name[i];
        /* 只允许这几类字符，'/' 不在其中，路径穿越无从谈起 */
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
    }
    /* 只收 .bin，避免这个接口被拿来往板子上丢任意文件 */
    if (n < 4 || strcmp(name + n - 4, ".bin") != 0) return 0;
    return 1;
}

static void handle_upload(int fd, const char *req, size_t req_len)
{
    char name[80], path[256], tmp_path[288], msg[320];
    const char *hdr_end, *body;
    const char *cl;
    long content_len, got, remain;
    int out;
    char buf[4096];

    if (qs_get(req, "name", name, sizeof(name)) != 0 || !safe_filename(name)) {
        send_simple(fd, "400 Bad Request", "text/plain",
                    "文件名不合法（只允许字母数字 . _ - 且必须以 .bin 结尾）");
        return;
    }

    cl = strstr(req, "Content-Length:");
    if (!cl) { send_simple(fd, "411 Length Required", "text/plain", "缺少 Content-Length"); return; }
    content_len = atol(cl + 15);
    if (content_len <= 0 || content_len > UPLOAD_MAX) {
        snprintf(msg, sizeof(msg), "文件大小 %ld 超出范围（上限 %d 字节）", content_len, UPLOAD_MAX);
        send_simple(fd, "413 Payload Too Large", "text/plain", msg);
        return;
    }

    hdr_end = strstr(req, "\r\n\r\n");
    if (!hdr_end) { send_simple(fd, "400 Bad Request", "text/plain", "HTTP 头不完整"); return; }
    body = hdr_end + 4;

    /* 先写临时文件，收完整了再 rename 到目标名。
       直接往目标文件写的话，传到一半断线会留下一个"看起来存在、其实残缺"
       的固件，之后拿它去刷设备就是一次注定失败的升级 */
    snprintf(path, sizeof(path), "%s%s", g_fw_dir, name);
    snprintf(tmp_path, sizeof(tmp_path), "%s.part", path);
    out = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        snprintf(msg, sizeof(msg), "无法写入 %s: %s", tmp_path, strerror(errno));
        send_simple(fd, "500 Internal Server Error", "text/plain", msg);
        return;
    }

    /* 第一次 read 已经连 body 的开头一起读进来了，先把这部分写掉 */
    got = (long)(req_len - (size_t)(body - req));
    if (got > content_len) got = content_len;
    if (got > 0 && write(out, body, (size_t)got) != got) {
        close(out); unlink(tmp_path);
        send_simple(fd, "500 Internal Server Error", "text/plain", "写文件失败");
        return;
    }

    remain = content_len - got;
    while (remain > 0) {
        ssize_t n = read(fd, buf, remain > (long)sizeof(buf) ? sizeof(buf) : (size_t)remain);
        if (n <= 0) break;
        if (write(out, buf, (size_t)n) != n) { n = -1; break; }
        remain -= n;
    }
    close(out);

    if (remain != 0) {
        unlink(tmp_path);
        snprintf(msg, sizeof(msg), "接收不完整，还差 %ld 字节", remain);
        send_simple(fd, "400 Bad Request", "text/plain", msg);
        return;
    }

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        snprintf(msg, sizeof(msg), "重命名失败: %s", strerror(errno));
        send_simple(fd, "500 Internal Server Error", "text/plain", msg);
        return;
    }

    printf("[upload] %s (%ld 字节)\n", path, content_len);
    snprintf(msg, sizeof(msg), "{\"ok\":true,\"path\":\"%s\",\"size\":%ld}", path, content_len);
    send_simple(fd, "200 OK", "application/json", msg);
}

/* 列出已上传的固件。除了文件名，还要带上大小和修改时间——
   传同名文件覆盖时，光看名字完全分不出到底传上去没有，
   有了时间戳才能一眼确认"这就是我刚传的那份" */
static void handle_fwlist(int fd)
{
    DIR *d = opendir(g_fw_dir);
    struct dirent *e;
    char body[4096];
    int n = 0, first = 1;

    n += snprintf(body + n, sizeof(body) - n, "{\"dir\":\"%s\",\"files\":[", g_fw_dir);
    if (d) {
        while ((e = readdir(d)) != NULL && n < (int)sizeof(body) - 200) {
            char full[320];
            struct stat st;
            size_t l = strlen(e->d_name);
            if (l < 4 || strcmp(e->d_name + l - 4, ".bin") != 0) continue;
            snprintf(full, sizeof(full), "%s%s", g_fw_dir, e->d_name);
            if (stat(full, &st) != 0) continue;
            n += snprintf(body + n, sizeof(body) - n,
                          "%s{\"name\":\"%s\",\"size\":%ld,\"mtime\":%ld}",
                          first ? "" : ",", e->d_name,
                          (long)st.st_size, (long)st.st_mtime);
            first = 0;
        }
        closedir(d);
    }
    snprintf(body + n, sizeof(body) - n, "]}");
    send_simple(fd, "200 OK", "application/json", body);
}

static void *client_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char req[2048];
    ssize_t n;
    size_t total = 0;
    struct timeval tv;

    /* 收发都设超时。没有超时的话，一个连上来却不发数据的客户端会让这个线程
       永远阻塞在 read() 上——线程是 detach 的、数量也没有上限，几个卡住的
       连接就是几个再也回收不了的线程。单次访问看不出问题，但这个进程要连续
       跑几个月，泄漏是会累积的。半开连接（对端拔网线/断电）尤其常见：
       TCP 不会主动告诉你对面没了，不设超时就是无限期等待。 */
    tv.tv_sec = 10; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* 循环读到请求头结束（\r\n\r\n）为止，而不是读一次就当收全了。
       TCP 是字节流，没有"一个 read 对应一个请求"这回事：请求头完全可能被
       拆成两个报文。局域网上小请求通常一次就到，所以平时看不出问题——
       这正是它讨厌的地方：一个依赖时序的偶发失败，上传大文件时（请求头
       后面紧跟着几百 KB 的 body）触发概率明显升高，表现为莫名其妙的
       "HTTP 头不完整"，而重试一次往往又好了。 */
    for (;;) {
        n = read(fd, req + total, sizeof(req) - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
        req[total] = '\0';
        if (strstr(req, "\r\n\r\n")) break;          /* 头收全了 */
        if (total >= sizeof(req) - 1) break;         /* 缓冲满了，交给下面按非法请求处理 */
    }
    if (total == 0) { close(fd); return NULL; }
    n = (ssize_t)total;   /* 下面 handle_upload 要用它算已经读进来多少 body */

    if (strncmp(req, "OPTIONS ", 8) == 0) {
        /* 浏览器跨域预检 */
        const char *r = "HTTP/1.1 204 No Content\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Access-Control-Allow-Methods: GET,POST,OPTIONS\r\n"
                        "Access-Control-Allow-Headers: *\r\n"
                        "Connection: close\r\n\r\n";
        write(fd, r, strlen(r));
    } else if (!check_token(req)) {
        send_simple(fd, "403 Forbidden", "text/plain", "bad token");
    } else if (strstr(req, "GET /frame")) {
        handle_frame(fd);
    } else if (strstr(req, "/touch")) {
        handle_touch(fd, req);
    } else if (strncmp(req, "POST /upload", 12) == 0) {
        handle_upload(fd, req, (size_t)n);
    } else if (strstr(req, "GET /fwlist")) {
        handle_fwlist(fd);
    } else if (strstr(req, "GET /info")) {
        char body[256];
        snprintf(body, sizeof(body),
                 "{\"screen_w\":%d,\"screen_h\":%d,\"frame_w\":%d,\"frame_h\":%d,\"format\":\"rgb565\"}",
                 g_w, g_h, g_out_w, g_out_h);
        send_simple(fd, "200 OK", "application/json", body);
    } else {
        send_simple(fd, "404 Not Found", "text/plain", "not found");
    }

    close(fd);
    return NULL;
}

int main(int argc, char *argv[])
{
    int port = (argc > 1) ? atoi(argv[1]) : 8082;
    int srv, on = 1;
    struct sockaddr_in addr;

    /* 行缓冲：常驻后台跑，输出重定向到日志文件时全缓冲会让日志一直空着 */
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
    setvbuf(stderr, NULL, _IOLBF, BUFSIZ);

    if (argc > 2) snprintf(g_token, sizeof(g_token), "%s", argv[2]);
    if (argc > 3) g_touch_dev = argv[3];
    if (argc > 4) {
        size_t l;
        snprintf(g_fw_dir, sizeof(g_fw_dir) - 2, "%s", argv[4]);
        l = strlen(g_fw_dir);
        /* 补上结尾的 '/'，后面直接拼文件名，不用每处都判断 */
        if (l && g_fw_dir[l - 1] != '/') { g_fw_dir[l] = '/'; g_fw_dir[l + 1] = '\0'; }
    }

    if (fb_open() != 0) return 1;
    printf("触摸注入设备: %s\n", g_touch_dev);
    if (access(g_touch_dev, W_OK) != 0)
        fprintf(stderr, "[warn] %s 不可写，远程点击会失败: %s\n", g_touch_dev, strerror(errno));

    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(srv, MAX_CLIENTS) < 0) { perror("listen"); return 1; }

    printf("固件上传目录: %s\n", g_fw_dir);
    printf("screen_share 已启动: http://0.0.0.0:%d/frame%s\n",
           port, g_token[0] ? "?token=..." : "");

    while (1) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; break; }
        pthread_t t;
        /* 每个请求一个短命线程：请求本身很轻（抓一帧就返回），
           不需要连接池那种复杂度 */
        if (pthread_create(&t, NULL, client_thread, (void *)(intptr_t)c) != 0) {
            close(c);
            continue;
        }
        pthread_detach(t);
    }

    close(srv);
    return 0;
}
