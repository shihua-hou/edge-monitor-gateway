# 板子侧部署（sysvinit / busybox）

实测这块 i.MX6ULL（正点原子 ALPHA，Yocto 系统）**没有 systemd**：

```
$ systemctl --version
（命令不存在）
$ ls /etc/rc5.d/
S01networking  S01xserver-nodm  S02dbus-1  S05connman  S10dropbear
```

是 sysvinit + busybox，所以 `deploy/systemd/` 那套 unit 在这台板子上用不了，
改用 `edgemonitor-daemon.sh` 守护脚本。若换成带 systemd 的板子，优先用 unit
（systemd 的进程管理比脚本轮询可靠）。

## 为什么需要守护脚本

现场踩过一次：WiFi 断线 → SSH 会话终止 → 在 SSH 里前台启动的 `edgemonitor_ui`
收到 SIGHUP 退出，而且再没人拉起来。屏幕停在最后一帧，看着像界面卡死，
实际进程早就没了（`ps` 里根本找不到）。

同一时刻 `video_v4l2` 却活得好好的——因为它当初是 `nohup`/`setsid` 起的，
`ps` 里 TTY 显示 `?`（已脱离终端）。这个对比正好说明问题出在哪。

脚本做两件事：`setsid` 脱离终端 + 每 10 秒巡检拉起。

## 部署步骤

### 1. 配置

```sh
cp deploy/edgemonitor.env.example /etc/edgemonitor.env
chmod 600 /etc/edgemonitor.env
vi /etc/edgemonitor.env       # MQTT_HOST 填服务器 IP，不是 localhost
```

**最容易错的一项**：`MQTT_HOST` 要填**服务器/VM 的地址**。
按新架构 broker 已经不在板子上了，留着 localhost 会一直连不上。

> **这个文件是 broker 地址的唯一来源。**
> `gateway_mqtt`、`ota_service` 和板子上的 Qt 界面都从这里取值（守护脚本
> source 后 export）。Qt 界面的设置页会把这几个框置灰并标注来源，
> QSettings 只在手工启动（没有这些环境变量）时兵底。
>
> 为什么要这么守：地址曾经散在三处（本文件 / Qt 的 QSettings / Web 的
> localStorage）。VM 换了一次 IP 之后只改了本文件，结果是"网关连上了、
> 数据也入库了，可是板子屏幕上什么都不显示"——而这个现象从表面上
> 根本看不出是配置不一致。Web 大屏则默认用页面自己的 `location.hostname`（页面
> 就是 broker 那台机器发出来的），并在连不上时自动回退。

改完此文件后要注意：**已经在跑的进程不会重新读**。`gateway_mqtt` 在 `main()`
里 `getenv` 一次就存下来了，守护脚本的"每轮重读"只对**新拉起的**进程生效。
改完执行：

```sh
killall gateway_mqtt ota_service edgemonitor_ui   # 守护脚本会在一个巡检周期内重新拉起
```

### 2. 放脚本

```sh
cp deploy/sysvinit/edgemonitor-daemon.sh /home/root/
chmod +x /home/root/edgemonitor-daemon.sh
```

### 3. 挂到开机启动

编辑 `/etc/rc.local`，在 `exit 0` **之前**加一行：

```sh
setsid /home/root/edgemonitor-daemon.sh >/tmp/em_daemon.log 2>&1 &
```

⚠️ **同时要处理 `/opt/QDesktop`**：出厂 rc.local 里有这么一行

```sh
/opt/QDesktop >/dev/null 2>&1 &
```

它是正点原子自带的 Qt 桌面，**会抢占 framebuffer 和触摸屏**，和我们的
`edgemonitor_ui` 冲突。要让本项目的界面开机直接显示，把这行注释掉。

### 4. 立即生效（不重启）

```sh
setsid /home/root/edgemonitor-daemon.sh >/tmp/em_daemon.log 2>&1 &
```

### 5. 验证

```sh
sleep 12; ps | grep -v grep | grep -E "gateway_mqtt|video_v4l2|edgemonitor_ui"
cat /tmp/em_daemon.log
```

杀掉任意一个进程，10 秒内应该自动重启：

```sh
killall edgemonitor_ui; sleep 12; ps | grep -v grep | grep edgemonitor_ui
```

## 遗留问题：WiFi 断了不会自动重连

现场实测：跑了约 36 小时后 AP 把板子踢下线
（`dmesg`: `RTL871X: OnDeAuth(wlan0) reason=2`），之后 `wpa_supplicant`
进程本身也没了，网络再没恢复过——只能靠串口进去手工救。

手工恢复命令（注意 **`-D wext`**，RTL8188EU 是 Realtek 老驱动，
不支持 nl80211，不加这个参数会报
`Driver does not support authentication/association or connect commands`）：

```sh
wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf -D wext
udhcpc -i wlan0
```

另外 `iw dev wlan0 link` 对这个驱动**永远显示 Not connected**（`iw` 只认
nl80211），查状态要用 `iwconfig wlan0`。

根治还没做，`/etc/rc5.d/S05connman` 里的 connman 也在管 WiFi，
两者可能互相打架，需要单独排查。
