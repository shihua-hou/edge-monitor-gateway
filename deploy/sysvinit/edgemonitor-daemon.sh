#!/bin/sh
#
# EdgeMonitor 进程守护脚本（i.MX6ULL 板子侧）
#
# 板子上没有 systemd（sysvinit + busybox），用不了 Restart=always，
# 所以这里自己做一个：每 10 秒巡一遍，进程不在就拉起来。
#
# 这个脚本要解决的实际问题：WiFi 断开 → SSH 会话终止 → 在 SSH 里前台启动的
# 进程收到 SIGHUP 一起死掉，而且再也没人拉起来。现场表现是"屏幕卡住不动、
# 数据不再更新"，实际上进程早就没了。用 setsid 脱离终端 + 定期巡检，
# 这两件事一起解决。
#
# 部署：
#   cp edgemonitor-daemon.sh /home/root/
#   chmod +x /home/root/edgemonitor-daemon.sh
#   然后在 /etc/rc.local 里加一行（见 deploy/README）：
#       setsid /home/root/edgemonitor-daemon.sh >/tmp/em_daemon.log 2>&1 &

# 显式指定 PATH，绝不继承。
#
# 这一行是本脚本最重要的一行，它防的是一次真实事故：
# 从 SSH 的非登录 shell 用 setsid 启动本脚本时，继承到的 PATH 只有
# /usr/bin:/bin，没有 /sbin 和 /usr/sbin。于是 ip / wpa_supplicant / udhcpc
# 全都 "command not found"，而 killall 恰好在 /usr/bin 里【能找到】——
# 结果就是：网络探测因为 ip 找不到而永远判定"无默认路由"（网络其实好好的），
# 触发自愈；自愈把 wpa_supplicant 杀掉，然后发现自己拉不起任何东西。
# 网络就此断掉，而且每 5 分钟重复一次，永远回不来。
#
# 开机走 rc.local 时因为有 `source /etc/profile` 反而正常——同一个脚本，
# 换个启动方式行为就完全不同，这种依赖环境的脆弱性必须在源头掐掉。
PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/local/sbin:/usr/local/bin
export PATH

# HOME 也必须显式指定，理由和 PATH 完全一样。
#
# 开机由 rc.local -> init 启动时 HOME 是 "/"，而从 SSH 启动时是 "/home/root"。
# Qt 的 QSettings 把配置存在 $HOME/.config/<组织>/<应用>.conf ——
# HOME 不同就是两个完全不同的文件。
#
# 实际后果：讯飞密钥明明好好地存在 /home/root/.config/EdgeMonitor/ 里，
# 开机自启的 UI 却去 /.config/ 找，读到一片空白，于是报
# "语音助手未配置 APPID/APIKey/APISecret"。手动重启守护脚本时又一切正常
# （因为 SSH 会话的 HOME 是对的）——这种"手动测就好、开机就坏"的差异
# 最难排查，因为最自然的验证方式恰好绕开了问题。
HOME=/home/root
export HOME

# ======================= Qt 运行环境 =======================
#
# 跟 PATH 完全同一个病根：不能依赖"谁启动了我"。
#
# 这三个变量定义在 /etc/profile 里。开机走 rc.local 时因为那里有
# `source /etc/profile` 才正常；用 setsid 从 SSH 启动守护脚本时一个都没有，
# 于是 Qt 找不到字体目录，界面【能画出布局和矢量图标，但所有文字都不见了】
# ——面板全是空框。
#
# 这个现象比"程序起不来"难查得多：进程在跑、日志没有报错、屏幕也亮着，
# 只有真正看一眼屏幕才会发现不对。日志里其实有一行
# "QFontDatabase: Cannot find font directory"，但它混在启动信息里毫不起眼。
export QT_QPA_FONTDIR=/usr/share/fonts/ttf
export QT_QPA_PLATFORM=linuxfb:tty=/dev/fb0
export QT_QPA_FB_TSLIB=1

# 系统输入法。板子的 rootfs 自带 Qt VirtualKeyboard（含拼音插件），
# 再补一个 QML 模块就能用——比自绘键盘多出中文输入，而地图搜索没中文没法用。
# 不设这个变量时，界面会退回自绘键盘（仅英文/符号），不会起不来。
# 系统输入法（Qt VirtualKeyboard）试过了，**不用**：
#   - 它是 QtQuick 的独立窗口，在 600px 高的屏上几乎占满，挡住正文内容
#   - 语言列表一长串，而这台设备只需要中/英
#   - 选了简体中文候选词出不来（插件、词库、布局都在，解码器没被激活）
# 结论是杀鸡用牛刀还没杀成。改回自绘键盘 + 自己的拼音候选，
# 见 imx6ull_ui/virtualkeyboard.cpp 和 pinyinime.cpp。
# 想再试官方那套就把下面这行取消注释：
#export QT_IM_MODULE=qtvirtualkeyboard
# 这两种屏（fbset timings 第三列为 220/213）用 tslib 反而不对，
# 照抄 /etc/profile 里的判断，保持和厂家环境一致
case "$(fbset 2>/dev/null | grep -E 'timings' | awk '{print $3}')" in
    220|213) unset QT_QPA_FB_TSLIB ;;
esac

BIN_DIR=/home/root
LOG_DIR=/tmp
CHECK_INTERVAL=10

# 配置（broker 地址、账号、设备 id）。放在函数里、每轮巡检都重新读一次，
# 而不是启动时读一次就完事：
#   踩过的坑——守护脚本先起来了，配置文件是之后才创建的，于是它拉起的
#   gateway 始终读不到 MQTT_HOST，一直去连 localhost 然后失败重连，
#   看日志只见 "Connection refused"，很难联想到是"脚本启动早于配置文件"。
#   每轮重读之后，改完配置最多等一个巡检周期就生效，也不用重启守护进程。
load_env() {
    if [ -f /etc/edgemonitor.env ]; then
        . /etc/edgemonitor.env
        # 这个 export 列表必须覆盖所有子进程要用的变量。漏掉的那些会悄悄
        # 退回程序里的默认值——踩过一次：漏了 OTA_FW_DIR，ota_service 就去
        # 默认的 /home/root/ota_fw/ 找固件，而固件在 /home/root/，
        # 网页上只显示"已触发"，得翻到板子日志才看得到真正原因
        export MQTT_HOST MQTT_USER MQTT_PASS DEVICE_ID
        export OTA_FW_DIR OTA_UDS_TOOL
        # NTRIP（CORS 差分）账号。gps_mqtt 内置的 NTRIP 客户端要用。
        # 不配的话它只打印一行"差分未启用"，照常出单点解，不会崩。
        export NTRIP_HOST NTRIP_PORT NTRIP_MOUNT NTRIP_USER NTRIP_PASS
        # 手机遥控页的访问令牌。**不配就是任何人都能开车**——
        # 车和手机在同一个 WiFi 上，扫到 8085 端口打开就能操控。
        export CTRL_TOKEN
        # 界面和桥接各自的 broker 账号。按"读写方向"分开，见 deploy/board/acl-board。
        export MQTT_UI_USER MQTT_UI_PASS MQTT_BRIDGE_PASS
    fi
}
load_env

# 时区。板子的 RTC 存的是本地时间，系统却默认按 UTC 解读，不设 TZ 的话
# 日志时间戳和 date 的输出会差 8 小时——更要命的是讯飞那类云服务的鉴权
# 签名用的是 UTC，时刻错了会被判定超出有效窗口，报 401（看着完全像密钥填错）
export TZ=CST-8

log() {
    echo "$(date '+%F %T') $*"
}

# ======================= 网络自愈 =======================
#
# 现场遇到过的真实故障：随身 WiFi/路由器挂久了之后，板子在 AP 的设备列表里
# 还是"已连接"，二层也还能跟同网段的机器通信，但到网关的转发已经失效——
# ping 网关 100% 丢包、ARP 表里网关是 00:00:00:00:00:00，公网自然全不通。
# 表现是"MQTT 还在连着（broker 在内网），可是天气、语音助手全哑了"。
#
# 这种半死不活的状态 wpa_supplicant 自己不会察觉，因为关联状态是好的。
# 只有重新走一遍认证握手才能恢复。所以这里主动探测 + 自动重连。
#
# 为什么必须放在守护脚本里而不是让人手动执行：重连过程中 WiFi 会断，
# 在 SSH 里跑的话命令自己会被 SIGHUP 打断在半路，板子从此彻底失联。
# 守护脚本是 setsid 脱离终端跑的，不受这个影响。

NET_CHECK_EVERY=6          # 每 6 轮巡检（约 60s）探测一次网络
NET_FAIL_LIMIT=3           # 连续 3 次失败才动手，约 3 分钟——避免偶发丢包就重连
RECONNECT_COOLDOWN=300     # 重连【成功】后的冷却：5 分钟，防止 AP 真的关机时反复折腾
# 重连【失败】后的冷却要短得多。失败通常是握手/DHCP 没赶上这种偶发问题，
# 再试一次往往就好；让它干等 5 分钟，等于把一次小挫折变成 5 分钟的断网。
# 而 AP 真的关机的场景，失败会一直持续，这时 60 秒一次的重试也不算折腾。
RECONNECT_COOLDOWN_FAIL=60
cur_cooldown=$RECONNECT_COOLDOWN

net_fail=0
last_reconnect=0
time_synced=0
first_check=1
tick=0
# 开机阶段标志。开机时网络没起来是【预期内的常态】，不是故障——这块板子
# 没有任何东西负责在启动时拉 WiFi（实测：重启后 wlan0 接口都不存在，
# 是本脚本的自愈把它拉起来的）。按常规的"连续 3 次失败才动手"走，
# 要等满 3 分钟才恢复联网，这 3 分钟里 MQTT、天气、语音全是不可用的。
# 首轮单独处理：约 30 秒时探一次，不通就立刻拉，之后再进常规节奏。
boot_phase=1

default_gw() {
    ip route 2>/dev/null | awk '/^default/ { print $3; exit }'
}

# 探测到网关的连通性，而不是探测公网：公网不通可能是运营商侧的问题，
# 重连 WiFi 解决不了；网关不通才是本机链路的问题，重连才有意义
net_ok() {
    gw=$(default_gw)
    [ -n "$gw" ] || return 1
    ping -c 2 -W 2 "$gw" >/dev/null 2>&1
}

wifi_reconnect() {
    conf=/etc/wpa_supplicant.conf
    [ -f "$conf" ] || conf=/etc/wpa_supplicant/wpa_supplicant.conf
    if [ ! -f "$conf" ]; then
        log "网络自愈：找不到 wpa_supplicant 配置，跳过重连"
        return 1
    fi

    # 【动手之前先确认自己拆得开也装得回】
    #
    # 这个检查是用一次真实事故换来的：PATH 不全时 killall 找得到、
    # 而 ip/wpa_supplicant/udhcpc 找不到，于是这个函数把 wpa_supplicant
    # 杀掉之后就再也拉不起来了——网络被自愈逻辑亲手弄断，而且每轮重复一次。
    #
    # 通用原则：任何"先拆后装"的恢复流程，都必须在拆之前验证装得回去。
    # 拆到一半发现工具不在手边，比一开始就不动更糟——不动至少还保留着
    # 一个能用的系统。
    for c in ip wpa_supplicant udhcpc killall; do
        if ! command -v "$c" >/dev/null 2>&1; then
            log "网络自愈：找不到 $c，放弃重连（不做任何破坏性操作）。PATH=$PATH"
            return 1
        fi
    done

    log "网络自愈：开始重连 WiFi（配置 $conf）"

    # udhcpc 要先杀：反过来的话网卡刚断它就开始疯狂重试，日志刷屏
    killall udhcpc 2>/dev/null
    killall wpa_supplicant 2>/dev/null
    sleep 2
    ip addr flush dev wlan0
    ip link set wlan0 down
    sleep 2
    ip link set wlan0 up
    sleep 1

    # -D wext 不能省。这块 RTL8188EU 用默认的 nl80211 会报
    # "Driver does not support authentication"，关联不上
    # 同 start_if_dead：9>&- 关掉继承来的锁描述符。
    # wpa_supplicant -B 会自己变成守护进程长期存在，它要是攥着这把锁，
    # 那就是最难解的一种——想释放锁就得断网，而断网正是最不能做的事
    wpa_supplicant -B -D wext -i wlan0 -c "$conf" 9>&-

    # 等【关联真正完成】再跑 DHCP，而不是盲等固定秒数。
    #
    # 原来是 `sleep 6` 然后直接 udhcpc——那是在赌"四次 EAPOL 握手能在 6 秒内
    # 做完"。开机时系统正忙（五个程序刚拉起、USB 枚举、驱动加载），握手比平时
    # 慢，赌输了：udhcpc 在一条还没准备好的链路上发 DISCOVER，而 busybox 的
    # udhcpc 默认只发 3 次（约 9 秒）就 "No lease, failing"，加上 -n 直接退出。
    # 实测开机后就是这样失败的，之后要等满 5 分钟冷却才会重试。
    #
    # 固定 sleep 又一次成了问题根源——跟 PATH 那次同一类错误：
    # 把"在我这儿碰巧成立"当成了"总是成立"。改成轮询真实状态。
    i=0
    while [ $i -lt 20 ]; do
        ip link show wlan0 2>/dev/null | grep -q "LOWER_UP" && break
        sleep 1
        i=$((i + 1))
    done
    log "网络自愈：关联耗时约 ${i}s，开始请求 DHCP"

    # -t 8 -T 2：最多 8 次、每次间隔 2 秒（约 16 秒），比默认的 3 次宽裕得多。
    # 保留 -n（要不到就退出）是必须的：不加它 udhcpc 会在前台无限重试，
    # 把整个守护脚本的巡检循环堵死
    udhcpc -i wlan0 -n -q -t 8 -T 2 9>&-
    sleep 1

    addr=$(ip addr show wlan0 2>/dev/null | awk '/inet /{print $2}')
    if [ -n "$addr" ]; then
        log "网络自愈：重连成功，地址 $addr"
        return 0
    fi
    # 拿不到地址要明确说出来。原来无论成败都打"重连完成"，
    # 地址那一栏空着——日志上看像是成功了，实际上网络还是断的
    log "网络自愈：重连后仍未取得 IPv4 地址（关联可能成功但 DHCP 失败）"
    return 1
}

# 用 HTTP 响应头里的 Date 校时。板子没装 ntpd，而 RTC 掉电后会跑偏，
# 时刻不准会让云服务鉴权直接失败——这是最省事的替代方案，零依赖。
sync_time_http() {
    # 用 nc 手写一个 HTTP 请求，而不是 wget：busybox 的 wget 没有 -S 选项，
    # 拿不到响应头（它只能给你正文），而我们要的恰恰是头里的 Date。
    #
    # 必须用 HTTP/1.0：1.1 默认长连接，服务器回完不关闭，nc 会一直挂在那里
    # 等到天荒地老。1.0 回完即断，nc 自然退出。
    hdr=$(printf 'HEAD / HTTP/1.0\r\nHost: www.baidu.com\r\n\r\n' \
          | nc www.baidu.com 80 2>/dev/null \
          | sed -n 's/^[Dd]ate: *//p' | head -1 | tr -d '\r')
    [ -n "$hdr" ] || return 1

    # hdr 形如：Sat, 05 Sep 2026 17:00:00 GMT
    # busybox 的 date -s 不认这个格式，拆开重排成 "YYYY-MM-DD hh:mm:ss"
    set -- $hdr
    d=$2; mon=$3; y=$4; hms=$5
    case "$mon" in
        Jan) m=01;; Feb) m=02;; Mar) m=03;; Apr) m=04;;
        May) m=05;; Jun) m=06;; Jul) m=07;; Aug) m=08;;
        Sep) m=09;; Oct) m=10;; Nov) m=11;; Dec) m=12;;
        *) return 1;;
    esac
    date -u -s "$y-$m-$d $hms" >/dev/null 2>&1 || return 1
    # -u 告诉 hwclock 按 UTC 写 RTC。不加的话下次开机读出来又偏 8 小时，
    # 而且 date 显示的数字看着还是"对"的，极难发现
    hwclock -w -u 2>/dev/null
    log "校时完成：本地 $(date)，UTC $(date -u)"
}

# 每轮巡检调一次，内部自己控制真正的探测频率
network_watchdog() {
    tick=$((tick + 1))
    if [ "$boot_phase" -eq 1 ]; then
        # 开机后第 3 轮（约 30 秒）探一次。不更早是因为要给 USB 枚举、
        # 驱动加载、udhcpc 留出时间——太早探测只会白白触发一次注定失败的重连
        [ "$tick" -ge 3 ] || return
    else
        [ $((tick % NET_CHECK_EVERY)) -eq 0 ] || return
    fi

    if net_ok; then
        boot_phase=0
        # 首次探测无论结果都记一笔：这个自愈逻辑平时是静默的，没有这行的话
        # 日志里"一切正常"和"这段代码压根没执行"长得一模一样
        if [ "$first_check" -eq 1 ]; then
            first_check=0
            log "网络自愈已启用：网关 $(default_gw) 可达，此后仅在异常时记录"
        fi
        if [ "$net_fail" -gt 0 ]; then
            log "网络已恢复（之前连续失败 $net_fail 次）"
            net_fail=0
        fi
        # 网络可用且还没校过时，校一次。放在这里而不是脚本开头：
        # 开机时 WiFi 多半还没关联上，那时候校时必定失败
        if [ "$time_synced" -eq 0 ]; then
            if sync_time_http; then
                time_synced=1
            else
                # 失败也要留痕。成功才打日志、失败悄无声息，是排查时最要命的
                # 组合——看不到日志你分不清"没跑"和"跑了但失败"
                log "校时失败（取不到 HTTP Date，或格式无法解析），下轮重试"
            fi
        fi
        return
    fi

    # 开机首轮就不通：可以不等三次确认直接拉，但判据不能只看 ping。
    #
    # ping 不通有两种完全不同的情况：
    #   a) 网卡压根没配上地址 —— 网络确实没就绪，该拉
    #   b) 有 IP 有路由，只是这两个包丢了 —— 链路是活的，此时去断网重连
    #      是在拿一个能用的网络冒险，而这个随身 WiFi 的 ICMP 本来就不稳
    # 只有 (a) 才动手。(b) 交给常规的"连续三次失败"流程去慢慢确认，
    # 那条路径有冷却时间保护，代价可控。
    if [ "$boot_phase" -eq 1 ]; then
        boot_phase=0
        if [ -z "$(default_gw)" ] || ! ip addr show wlan0 2>/dev/null | grep -q "inet "; then
            log "开机后网卡尚未配上地址，立即拉起 WiFi（开机阶段不等三次确认）"
            last_reconnect=$(date +%s)
            if wifi_reconnect; then
                cur_cooldown=$RECONNECT_COOLDOWN
            else
                cur_cooldown=$RECONNECT_COOLDOWN_FAIL
            fi
            net_fail=0
            time_synced=0
        else
            log "开机探测：网卡已有地址但网关暂不可达，按常规流程观察，不贸然重连"
        fi
        return
    fi

    net_fail=$((net_fail + 1))
    gw=$(default_gw)
    # "网关为空"和"网关不通"是两回事，日志要分清：前者是根本没有默认路由
    # （网卡没起来、没拿到 DHCP），后者是路由存在但对端不响应。
    # 原来直接打印 $(default_gw)，接口不存在时那里是空的，日志就成了
    # "网络探测失败（第 1 次，网关 ）"——括号里空着，看着像日志本身出了问题
    log "网络探测失败（第 $net_fail/$NET_FAIL_LIMIT 次，${gw:+网关 $gw 不通}${gw:-无默认路由}）"
    [ "$net_fail" -ge "$NET_FAIL_LIMIT" ] || return

    now=$(date +%s)
    if [ $((now - last_reconnect)) -lt "$cur_cooldown" ]; then
        return    # 冷却期内，静默跳过，不刷日志
    fi
    last_reconnect=$now
    if wifi_reconnect; then
        cur_cooldown=$RECONNECT_COOLDOWN
    else
        cur_cooldown=$RECONNECT_COOLDOWN_FAIL
    fi
    net_fail=0
    time_synced=0     # 网络刚恢复，重新校一次时间
}

# ======================= 日志轮转 =======================
#
# /tmp 在这块板子上是 tmpfs——写进去的日志占的是【内存】，不是 flash。
# 几个常驻进程各写一份，其中 UI 那份还带着 MQTT 交互记录，日积月累能把
# 内存吃掉一大块，最后表现成"跑几天之后系统变慢/进程被 OOM 干掉"，
# 而没人会想到是日志撑的。
#
# 不用 logrotate（板子上没有），自己截断：超过阈值就把后半截留下、丢掉前半。
# 保留尾部而不是直接清空，是因为出问题时最近的记录才有价值。
LOG_MAX_KB=512

rotate_logs() {
    for f in "$LOG_DIR"/em_*.log; do
        [ -f "$f" ] || continue
        sz=$(( $(wc -c < "$f") / 1024 ))
        [ "$sz" -gt "$LOG_MAX_KB" ] || continue
        # 两个坑，都必须绕开：
        #
        # 1) 不能直接 `tail -c N "$f" > "$f"`。重定向会先把 f 截断成 0 字节，
        #    tail 再去读就只剩空文件，整份日志瞬间蒸发。所以要用临时文件中转。
        #
        # 2) 中转完【不能用 mv 覆盖回去】。这些日志正被各个子进程通过 >> 持有
        #    着文件描述符，而 mv 换掉的是 inode：进程手里的 fd 仍然指向旧
        #    inode，之后所有输出都写进那个已经没有文件名的旧文件里——新日志
        #    永远是空的，旧 inode 又因为还被打开而不会释放，内存一点没省下来，
        #    反而把日志弄丢了。典型的"轮转之后日志就不动了"。
        #    正确做法是 `cat tmp > f`：它截断的是【同一个 inode】，
        #    以 O_APPEND 打开的写入方仍然接着往新的末尾写，一切正常。
        if tail -c $(( LOG_MAX_KB * 512 )) "$f" > "$f.tmp" 2>/dev/null; then
            cat "$f.tmp" > "$f"
            rm -f "$f.tmp"
        fi
        log "日志 $f 超过 ${LOG_MAX_KB}KB，已就地截断，保留最近一半"
    done
}

# busybox 的 pgrep 不一定支持 -f，用 ps 输出过滤更保险。
#
# 【必须带 -ef】。这块板子上的 ps 是 procps 风格：不带参数时只列出"和调用者
# 同一个控制终端"的进程，而本脚本是 setsid 起的、根本没有控制终端——那种情况
# 下它可能一个进程都看不到，于是 is_running 恒为假，每 10 秒就把五个程序
# 重新拉起一遍，几分钟内进程数就爆了。
# （踩过的坑：清理多余守护进程时用的也是不带 -ef 的 ps，结果一个都没杀掉，
#   每执行一次"清理+重启"就多叠一个实例，最后同时跑了 4 个。）
is_running() {
    ps -ef 2>/dev/null | grep -v grep | grep -q "$1"
}

start_if_dead() {
    tag="$1"       # 用于日志和进程匹配的关键字
    shift
    if ! is_running "$tag"; then
        log "启动 $tag"
        # 末尾的 9>&- 不能省：它把单实例锁的文件描述符对子进程关掉。
        #
        # flock 的锁绑在【打开文件描述】上，由所有共享它的进程共同持有，
        # 而 fd 默认会被 fork/exec 继承。不关的话，守护进程拉起的每一个程序
        # 都攥着这把锁——守护进程自己死了，锁还被那一堆孩子把持着，
        # 新实例永远拿不到锁、永远起不来。
        #
        # 实测踩过：换新版脚本时 kill 掉旧守护进程，新的却报"已经有一个在
        # 运行"，一查发现持有锁的是 gateway_mqtt、video_v4l2 甚至
        # wpa_supplicant——全是它生的孩子。
        # 单实例锁本来是防重复启动的，结果自己变成了"永远无法启动"的原因。
        setsid "$@" >>"$LOG_DIR/em_$tag.log" 2>&1 </dev/null 9>&- &
    fi
}

# ======================= 单实例保护 =======================
#
# 网关和历史服务都加了 flock，唯独这个"管着所有程序"的脚本没加——结果是
# 最容易叠起来的偏偏是它。多个实例同时跑的实际危害不是浪费 CPU，而是
# 网络自愈会互相打架：网关一旦不通，N 个实例会【同时】去 kill wpa_supplicant
# 并重连 WiFi，彼此把对方的重连过程打断，本来能恢复的网络反而彻底起不来。
#
# 用文件描述符 9 持有锁：进程活着期间 fd 一直开着，退出（哪怕是 kill -9）
# 时内核自动释放，不留下需要手工清理的陈旧锁。
LOCK_FILE=/tmp/edgemonitor-daemon.lock
if command -v flock >/dev/null 2>&1; then
    exec 9>"$LOCK_FILE"
    # **等锁，而不是拿不到就退出**（-w 20 而非 -n）。
    #
    # 原来用 -n 的问题出在"重启"这个最常见的动作上：kill 掉旧实例之后
    # 立刻启动新的，而旧实例还没退完、锁还没释放，新实例当场退出——
    # 结果是一个都不剩，整套服务全停。踩过三次，每次都要人工再起一遍。
    #
    # 等待不会削弱单实例保护：真有另一个在正常运行，20 秒也等不到，
    # 照样退出并提示；只是把"重启时的竞态"和"确实已经有一个在跑"
    # 这两种情况区分开了——它们该有不同的处理方式。
    if ! flock -w 20 9; then
        echo "$(date '+%F %T') 等了 20 秒仍拿不到锁，说明确实有另一个守护进程在正常运行，本次启动退出。"
        echo "  想重启的话：ps -ef | grep [e]dgemonitor-daemon   然后 kill 掉旧的"
        exit 1
    fi
else
    echo "$(date '+%F %T') [warn] 系统没有 flock，跳过单实例检查——"
    echo "  启动前请自己确认没有别的实例在跑（ps -ef | grep [e]dgemonitor-daemon）"
fi

log "EdgeMonitor 守护启动，巡检间隔 ${CHECK_INTERVAL}s，MQTT_HOST=${MQTT_HOST:-未配置(将连 localhost)}"

# CAN 接口每次开机都要重新配。加 2>/dev/null 是因为已经配好时会报错，
# 这属于正常情况不用打扰日志
ip link set can0 type can bitrate 500000 2>/dev/null
ip link set can0 up 2>/dev/null

while true; do
    load_env      # 每轮重读，改完配置无需重启本脚本
    network_watchdog   # 探测/自愈网络 + 首次联网后校时（内部自己控制频率）
    # 每 30 轮（约 5 分钟）查一次日志大小。查得太勤没必要，日志长不了那么快
    [ $((tick % 30)) -eq 0 ] && rotate_logs
    # 板载 MQTT broker。**必须排在所有 MQTT 客户端前面**：
    # 客户端自己会重连，所以顺序错了最终也能连上，但开机那几秒日志里
    # 会刷一串"连接被拒绝"，看着像故障。让它先起来更干净。
    #
    # 为什么 broker 在板子上：发布方(gateway_mqtt/gps_mqtt)和订阅方(Qt 界面)
    # 本来就都在这块板子上，原先却要绕一圈到虚拟机的 broker——
    # 虚拟机一关，板子自己的界面就不刷新、控制页也发不出指令。
    # 搬到本地之后，车的自洽性不再依赖任何上位机；
    # Web 大屏那边由虚拟机上的 broker 桥接过来取数（浏览器只能走
    # WebSocket，而板子这个交叉编译版没链 libwebsockets）。
    start_if_dead mosquitto      "$BIN_DIR/mosquitto"      -c "$BIN_DIR/mosquitto.conf"
    start_if_dead gateway_mqtt   "$BIN_DIR/gateway_mqtt"   can0
    start_if_dead video_v4l2     "$BIN_DIR/video_v4l2"     /dev/video2 8081
    start_if_dead edgemonitor_ui "$BIN_DIR/edgemonitor_ui" -platform linuxfb
    # 远程屏幕：抓 framebuffer + 注入触摸 + 接收固件上传（端口 8082）
    start_if_dead screen_share   "$BIN_DIR/screen_share"   8082 "" /dev/input/event1 "$BIN_DIR/"
    # OTA：订阅 monitor/ota/cmd，收到命令后调 uds_tool 升级 STM32。
    # 之前默认注释掉，结果网页点"开始升级"毫无反应——没有任何一端会报错，
    # 因为消息发出去了、只是没人订阅。默认启用更符合预期
    start_if_dead ota_service    "$BIN_DIR/ota_service"    can0
    # RTK/GNSS：OEM700 走 USB，枚举出 4 个 CDC-ACM 口，只有一个吐 NMEA
    # （另外几个是模块日志/Lua REPL、RTCM 二进制观测量）。
    #
    # **不再写死 ttyACM3**：拔了再插之后编号会整体后移（旧节点还没回收，
    # 新的从 ACM4 起），现场表现是"插回去就是不出数据"而设备明明枚举成功了。
    # 不带参数启动时 gps_mqtt 自己挨个口听 2 秒，谁吐 $..GGA 就用谁。
    # 串口没找到也不退出，每 5 秒重找一次，模块拔了再插能自己恢复。
    start_if_dead gps_mqtt       "$BIN_DIR/gps_mqtt"
    # 手机遥控：直接在板子上开 HTTP，收到指令直写 CAN。
    # **不经过 MQTT/broker**——"车开着、人就在旁边、想让它动一下"
    # 这个场景不该依赖机房里的虚拟机。
    # 第二个参数是 token，空 = 内网免验证（要收紧就填一个）。
    # 第二个参数是访问令牌，从 /etc/edgemonitor.env 的 CTRL_TOKEN 来。
    # 令牌不写在这里也不进仓库：这个脚本是要提交的，而 git 历史删不干净。
    #
    # 令牌走 URL 查询串（手机上存成书签即可），页面本身不带令牌、
    # 也不把它注入到 HTML 里——所以"页面能打开"不等于"能开车"。
    # 留空则退回无验证模式（和以前一样），不会因为没配就起不来。
    start_if_dead mobile_ctrl    "$BIN_DIR/mobile_ctrl"    8085 "$CTRL_TOKEN" can0
    sleep "$CHECK_INTERVAL"
done
