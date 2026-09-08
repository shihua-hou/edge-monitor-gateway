/**
 * ota_service.c - i.MX6ULL OTA 触发服务（配合 web/index.html 的"固件 OTA 升级"卡片）
 *
 * 背景：uds_tool 是个命令行工具，网页端没法直接执行它。这个服务订阅
 * monitor/<dev>/ota/cmd，收到网页发来的固件文件名后，fork+exec 起 uds_tool，
 * 边读它的标准输出边解析进度，实时发布到 monitor/<dev>/ota/progress，
 * 结束后发布 monitor/<dev>/ota/status。
 * <dev> 取自环境变量 DEVICE_ID，默认 robot01——见下面 TP_OTA_* 的说明。
 *
 * 安全设计：
 *   1. 不用 system()/popen() 拼命令行字符串——网页传来的文件名本质是不可信输入，
 *      拼进 shell 命令行是命令注入的经典坑。这里用 fork()+execv() 直接传 argv
 *      数组给 uds_tool，完全绕开 shell 解析，不存在注入面。
 *   2. 网页只允许传"文件名"，不允许传路径：服务端固定拼到 OTA_FW_DIR 下，
 *      并且拒绝包含 '/'、".." 或其他路径字符的文件名，防止越权读取
 *      固件目录之外的任意文件。
 *
 * 编译：
 *   arm-linux-gnueabihf-gcc ota_service.c -o ota_service -lmosquitto -lpthread
 *   （跟 gateway_mqtt 一样，板子上跑动态链接可能遇到 GLIBC 版本不匹配，
 *    需要的话参照 gateway_mqtt 的静态链接方案重新链 libmosquitto.a）
 *
 * 运行：
 *   export MQTT_USER=em MQTT_PASS=yourpass MQTT_HOST=192.168.x.x
 *   export OTA_FW_DIR=/home/root/ota_fw/     # 固件文件存放目录，缺省同默认值
 *   ./ota_service [can0]
 *
 * 固件文件要求：跟 uds_tool 一样，先用 Keil 的 fromelf --bin 把 App 工程的
 * .axf 转成 .bin，放到 OTA_FW_DIR 下，网页填文件名即可（不是完整路径）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <mosquitto.h>

/* OTA 的三个 topic 带设备号：monitor/<dev>/ota/{cmd,progress,status}。
 *
 * 原来是全局的 monitor/ota/cmd —— 数据和控制 topic 早就按设备拆开了，
 * 唯独 OTA 没拆，这是个实打实的隐患而不只是不统一：多台机器人接同一个
 * broker 时，网页上给 A 车点"开始升级"，B 车的 ota_service 同样收得到，
 * 于是所有车一起进入升级流程。而升级过程中 STM32 会复位、擦写 Flash，
 * 这是最不能"顺带波及"的操作。
 *
 * 进度和结果也必须按设备分开，否则两台车同时升级时进度条会互相覆盖，
 * 网页上看到的是两条流混在一起的乱序数字。
 *
 * 注意：这个改动要和 web/app.js 一起部署，两边的 topic 必须同时更新。 */
static char TP_OTA_CMD[96];
static char TP_OTA_PROGRESS[96];
static char TP_OTA_STATUS[96];
#define MQTT_HOST_DEFAULT "localhost"
#define MQTT_PORT        1883
#define OTA_FW_DIR_DEFAULT "/home/root/ota_fw/"
#define UDS_TOOL_PATH_DEFAULT "./uds_tool"
#define MAX_LINE 256

static struct mosquitto *g_mosq = NULL;
static char g_can_if[32] = "can0";
static char g_fw_dir[256] = OTA_FW_DIR_DEFAULT;
static char g_uds_tool[256] = UDS_TOOL_PATH_DEFAULT;
static int  g_busy = 0;   /* 同一时间只允许一次升级，避免并发 fork 多个 uds_tool 抢 CAN */
static pthread_mutex_t g_busy_lock = PTHREAD_MUTEX_INITIALIZER;

/* ============ 极简 JSON 字符串字段提取：{"path":"xxx"} ============ */
static int json_get_str(const char *json, const char *key, char *out, size_t outsz)
{
    char pat[32];
    const char *p, *start, *end;
    size_t len;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    start = p + 1;
    end = strchr(start, '"');
    if (!end) return -1;
    len = (size_t)(end - start);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return 0;
}

/* JSON 字符串转义：这几个字段虽然来自 uds_tool 自己的固定文案，不是用户输入，
   但既然最终要塞进发给浏览器解析的 JSON 里，还是老老实实转义一下双引号和
   反斜杠，不依赖"输出内容永远不会出现这些字符"这种脆弱假设 */
static void json_escape(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (; *in && o + 2 < outsz; in++) {
        if (*in == '"' || *in == '\\') { out[o++] = '\\'; out[o++] = *in; }
        else if ((unsigned char)*in >= 0x20) out[o++] = *in;
        /* 控制字符直接丢弃，避免生成非法 JSON */
    }
    out[o] = '\0';
}

/* 文件名合法性检查：只允许字母数字和 . _ -，不允许 '/' 或以 '.' 开头
   （挡掉 ".." 路径穿越和隐藏文件），长度也限制一下 */
static int valid_filename(const char *name)
{
    size_t i, len = strlen(name);
    if (len == 0 || len > 128) return 0;
    if (name[0] == '.') return 0;
    for (i = 0; i < len; i++) {
        char c = name[i];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static void publish_progress(const char *stage, int pct)
{
    char esc[192], payload[256];
    json_escape(stage, esc, sizeof(esc));
    if (pct >= 0)
        snprintf(payload, sizeof(payload), "{\"stage\":\"%s\",\"pct\":%d}", esc, pct);
    else
        snprintf(payload, sizeof(payload), "{\"stage\":\"%s\"}", esc);
    mosquitto_publish(g_mosq, NULL, TP_OTA_PROGRESS, (int)strlen(payload), payload, 0, false);
    printf("[progress] %s\n", payload);
}

static void publish_status(int ok, const char *msg)
{
    char esc[192], payload[256];
    json_escape(msg, esc, sizeof(esc));
    snprintf(payload, sizeof(payload), "{\"ok\":%s,\"msg\":\"%s\"}", ok ? "true" : "false", esc);
    mosquitto_publish(g_mosq, NULL, TP_OTA_STATUS, (int)strlen(payload), payload, 0, false);
    printf("[status] %s\n", payload);
}

/* "[OK ] 0x10 编程会话" 这类已知步骤名映射一个大致进度百分比，让进度条走得
   有节奏感；分块传输阶段的百分比由 uds_tool 自己按字节数算好直接用，
   这里只覆盖"传输"前后那些一次性步骤 */
static int stage_pct(const char *name)
{
    static const struct { const char *name; int pct; } tbl[] = {
        { "0x10 编程会话", 10 }, { "0x27 请求种子", 15 }, { "0x27 发送密钥", 20 },
        { "0x31 擦除Flash", 25 }, { "0x34 请求下载", 30 },
        { "0x37 退出传输", 92 }, { "0x31 CRC校验", 96 }, { "0x11 ECU复位", 99 },
    };
    size_t i;
    for (i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
        if (strcmp(name, tbl[i].name) == 0) return tbl[i].pct;
    return -1;   /* 未知步骤名：只更新文字，不动进度条 */
}

/* 从一行文本里找出形如 "NN%" 的百分比（可能带前缀，比如首次是
   "transfer: 0%"），提取失败返回 -1 */
static int extract_pct(const char *line)
{
    const char *p = strchr(line, '%');
    const char *start;
    if (!p) return -1;
    start = p;
    while (start > line && isdigit((unsigned char)start[-1])) start--;
    if (start == p) return -1;
    return atoi(start);
}

/* 逐行解析 uds_tool 的标准输出，转成进度/状态事件。返回 1=看到成功结束标记，
   0=看到失败结束标记，-1=流结束了但没看到明确的成功/失败标记(异常退出) */
static int parse_output_line(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
    if (len == 0) return -2;   /* 空行，忽略 */

    if (strncmp(line, "===", 3) == 0) {
        int ok = strstr(line, "SUCCESS") != NULL;
        publish_status(ok, line);
        return ok ? 1 : 0;
    }
    if (strncmp(line, "  [OK ] ", 8) == 0) {
        publish_progress(line + 8, stage_pct(line + 8));
        return -2;
    }
    if (strncmp(line, "  [ERR] ", 8) == 0) {
        char buf[MAX_LINE];
        snprintf(buf, sizeof(buf), "失败: %s", line + 8);
        publish_progress(buf, -1);
        return -2;
    }
    if (strchr(line, '%')) {
        int pct = extract_pct(line);
        if (pct >= 0) publish_progress("传输固件", 30 + pct * 60 / 100);
        return -2;
    }
    /* 其它行（[probe]/[timeout]/[retry]/[NEG]/firmware:/CAN xx ready/done 等）
       原样转发文字，不强行猜百分比，好过丢掉信息 */
    publish_progress(line, -1);
    return -2;
}

static void run_upgrade(const char *filename)
{
    char fw_path[512];
    int pipefd[2];
    pid_t pid;
    FILE *fp;
    char line[MAX_LINE];
    int result = -1;   /* -1=未看到明确结束标记 */

    if (!valid_filename(filename)) {
        publish_status(0, "固件文件名不合法（只能是纯文件名，不能带路径）");
        return;
    }
    snprintf(fw_path, sizeof(fw_path), "%s%s", g_fw_dir, filename);
    if (access(fw_path, R_OK) != 0) {
        /* A/B 模式下网页传的是基名（如 atk_f103），真正的文件是
           atk_f103_a.bin / atk_f103_b.bin —— 到底用哪个由 uds_tool 问过
           设备之后才知道，这里只确认"至少有一份在"，别把请求提前挡掉 */
        char probe[600];
        int found = 0;
        snprintf(probe, sizeof(probe), "%s_a.bin", fw_path);
        if (access(probe, R_OK) == 0) found = 1;
        snprintf(probe, sizeof(probe), "%s_b.bin", fw_path);
        if (access(probe, R_OK) == 0) found = 1;
        if (!found) {
            char msg[400];
            snprintf(msg, sizeof(msg),
                     "固件不存在或不可读: %s（A/B 模式请放 %s_a.bin 和 %s_b.bin）",
                     fw_path, filename, filename);
            publish_status(0, msg);
            return;
        }
    }

    if (pipe(pipefd) != 0) { publish_status(0, "内部错误: pipe 失败"); return; }

    pid = fork();
    if (pid < 0) { publish_status(0, "内部错误: fork 失败"); return; }

    if (pid == 0) {
        /* 子进程：stdout/stderr 都重定向进管道，uds_tool 的报错(fprintf stderr)
           也能被父进程看到、转发出去，而不是消失在子进程自己的 stderr 里 */
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execl(g_uds_tool, g_uds_tool, fw_path, g_can_if, (char *)NULL);
        /* execl 失败才会走到这里 */
        fprintf(stderr, "=== Upgrade FAILED (exec %s: %s) ===\n", g_uds_tool, strerror(errno));
        _exit(127);
    }

    /* 父进程 */
    close(pipefd[1]);
    fp = fdopen(pipefd[0], "r");
    if (!fp) { publish_status(0, "内部错误: fdopen 失败"); close(pipefd[0]); waitpid(pid, NULL, 0); return; }

    publish_progress("已启动升级进程", 0);
    while (fgets(line, sizeof(line), fp)) {
        int r = parse_output_line(line);
        if (r == 0 || r == 1) result = r;
    }
    fclose(fp);

    int wstatus = 0;
    waitpid(pid, &wstatus, 0);

    if (result < 0) {
        /* uds_tool 中途被杀/崩掉，既没打印 SUCCESS 也没打印 FAILED */
        char msg[128];
        if (WIFEXITED(wstatus))
            snprintf(msg, sizeof(msg), "uds_tool 异常退出（返回码 %d），未看到明确的成功/失败标记", WEXITSTATUS(wstatus));
        else
            snprintf(msg, sizeof(msg), "uds_tool 被信号终止，未看到明确的成功/失败标记");
        publish_status(0, msg);
    }
}

/* 订阅放在连接回调里：mosquitto_loop_forever 断线后会自动重连，而
   clean_session=true 的会话重连后 broker 不保留任何订阅。如果只在 main 里
   订一次，网络抖动恢复之后进程还在、日志也正常，但 OTA 命令再也收不到——
   偏偏 OTA 又是最不常用、最容易到用时才发现坏了的功能 */
static void on_connect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)userdata;
    if (rc != 0) {
        fprintf(stderr, "[mqtt] broker 拒绝连接: %s\n", mosquitto_connack_string(rc));
        return;
    }
    mosquitto_subscribe(mosq, NULL, TP_OTA_CMD, 1);
    printf("[mqtt] connected, subscribed: %s\n", TP_OTA_CMD);
}

static void on_disconnect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)mosq; (void)userdata;
    if (rc != 0) fprintf(stderr, "[mqtt] 连接断开(%s)，等待自动重连…\n", mosquitto_strerror(rc));
}

/* run_upgrade 一次完整升级少则几十秒多则几分钟(几万字节固件、每块只 6
   字节、还要等每块 CAN 响应)，不能直接在 on_message 里同步跑——on_message
   是 mosquitto 网络线程的回调，阻塞在这里的话 mosquitto_loop_forever 那段
   时间不会发 PINGREQ，很容易把配置的 60s keepalive 撑爆，broker 会认为
   连接断了直接把这个客户端踢掉，升级到一半 MQTT 连接却先掉了。所以升级
   本体放单独线程跑，网络线程继续正常收发心跳。 */
typedef struct { char filename[160]; } OtaJob;

static void *ota_worker(void *arg)
{
    OtaJob *job = (OtaJob *)arg;
    printf("[ota] start, file=%s\n", job->filename);
    run_upgrade(job->filename);
    free(job);
    pthread_mutex_lock(&g_busy_lock);
    g_busy = 0;
    pthread_mutex_unlock(&g_busy_lock);
    return NULL;
}

static void on_message(struct mosquitto *mosq, void *userdata,
                       const struct mosquitto_message *msg)
{
    OtaJob *job;
    pthread_t tid;
    (void)mosq; (void)userdata;
    if (strcmp(msg->topic, TP_OTA_CMD) != 0) return;
    if (msg->payloadlen <= 0) return;

    pthread_mutex_lock(&g_busy_lock);
    if (g_busy) {
        pthread_mutex_unlock(&g_busy_lock);
        publish_status(0, "已有升级任务在进行，请稍后再试");
        return;
    }
    g_busy = 1;
    pthread_mutex_unlock(&g_busy_lock);

    job = malloc(sizeof(OtaJob));
    if (!job) {
        pthread_mutex_lock(&g_busy_lock); g_busy = 0; pthread_mutex_unlock(&g_busy_lock);
        publish_status(0, "内部错误: 内存分配失败");
        return;
    }
    if (json_get_str((char *)msg->payload, "path", job->filename, sizeof(job->filename)) != 0) {
        free(job);
        pthread_mutex_lock(&g_busy_lock); g_busy = 0; pthread_mutex_unlock(&g_busy_lock);
        publish_status(0, "请求格式错误：缺少 path 字段");
        return;
    }

    if (pthread_create(&tid, NULL, ota_worker, job) != 0) {
        free(job);
        pthread_mutex_lock(&g_busy_lock); g_busy = 0; pthread_mutex_unlock(&g_busy_lock);
        publish_status(0, "内部错误: 创建升级线程失败");
        return;
    }
    pthread_detach(tid);   /* 不需要 join，跑完自己收尾 */
}

int main(int argc, char *argv[])
{
    int rc;
    const char *env;

    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);

    if (argc > 1) strncpy(g_can_if, argv[1], sizeof(g_can_if) - 1);
    if ((env = getenv("OTA_FW_DIR")) && env[0]) {
        strncpy(g_fw_dir, env, sizeof(g_fw_dir) - 2);
        /* 保证目录路径以 '/' 结尾，拼文件名时不用再判断 */
        size_t l = strlen(g_fw_dir);
        if (l == 0 || g_fw_dir[l - 1] != '/') strcat(g_fw_dir, "/");
    }
    if ((env = getenv("OTA_UDS_TOOL")) && env[0]) strncpy(g_uds_tool, env, sizeof(g_uds_tool) - 1);

    const char *mqtt_host = getenv("MQTT_HOST");
    if (!mqtt_host || !mqtt_host[0]) mqtt_host = MQTT_HOST_DEFAULT;
    const char *mqtt_user = getenv("MQTT_USER");
    const char *mqtt_pass = getenv("MQTT_PASS");

    mosquitto_lib_init();
    /* client_id 带上设备号：MQTT 不允许同一 broker 上重名，重名会互相踢，
       现象是连上就断、反复循环。多设备接同一个 broker 时必然踩到 */
    {
        const char *dev_id = getenv("DEVICE_ID");
        static char client_id[96];
        if (!dev_id || !dev_id[0]) dev_id = "robot01";
        snprintf(client_id, sizeof(client_id), "ota_%s", dev_id);
        /* 三个 topic 在这里一次性拼好，后面直接用 */
        snprintf(TP_OTA_CMD,      sizeof(TP_OTA_CMD),      "monitor/%s/ota/cmd",      dev_id);
        snprintf(TP_OTA_PROGRESS, sizeof(TP_OTA_PROGRESS), "monitor/%s/ota/progress", dev_id);
        snprintf(TP_OTA_STATUS,   sizeof(TP_OTA_STATUS),   "monitor/%s/ota/status",   dev_id);
        printf("[ota] 设备 %s，命令 topic: %s\n", dev_id, TP_OTA_CMD);
        g_mosq = mosquitto_new(client_id, true, NULL);
    }
    if (!g_mosq) { fprintf(stderr, "mosquitto_new fail\n"); return 1; }
    mosquitto_message_callback_set(g_mosq, on_message);
    mosquitto_connect_callback_set(g_mosq, on_connect);
    mosquitto_disconnect_callback_set(g_mosq, on_disconnect);
    mosquitto_threaded_set(g_mosq, true);   /* publish_progress/publish_status 会从升级工作线程里调用 */

    if (mqtt_user && mqtt_pass) {
        mosquitto_username_pw_set(g_mosq, mqtt_user, mqtt_pass);
    } else {
        fprintf(stderr, "[warn] 未设置 MQTT_USER/MQTT_PASS，broker 若禁匿名将连接失败\n");
    }

    rc = mosquitto_connect(g_mosq, mqtt_host, MQTT_PORT, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        /* 同 gateway_mqtt：不退出，交给 loop_forever 的重连退避慢慢等 broker */
        fprintf(stderr, "[mqtt] 首次连接 %s:%d 失败(%s)，进入重连等待\n",
                mqtt_host, MQTT_PORT, mosquitto_strerror(rc));
    }
    /* 订阅在 on_connect 回调里做，不在这里——见回调注释：重连后会拿到全新
       会话，只在这订一次的话，断网恢复后 OTA 命令就永远收不到了，而进程
       看起来一切正常 */
    printf("ota_service ready: fw_dir=%s uds_tool=%s can_if=%s\n",
           g_fw_dir, g_uds_tool, g_can_if);

    mosquitto_loop_forever(g_mosq, -1, 1);

    mosquitto_destroy(g_mosq);
    mosquitto_lib_cleanup();
    return 0;
}
