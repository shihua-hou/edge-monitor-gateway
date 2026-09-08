# i.MX6ULL 开发指南（结合本项目）

> 覆盖：开发模式选择、系统准备、板子基础操作、三种开发路径、
> 本项目各阶段（CAN/Qt/MQTT/视频/GPS）在 i.MX6ULL 上的具体做法、调试手段。

---

## 0. 先搞清你的板子（影响接线和系统，不阻塞开发）

| 项 | ALPHA 板 | Mini 板 |
|----|---------|---------|
| RGB 屏接口 | 有（可插 7寸 RGB） | 有 |
| 板载 CAN 收发器 | 有（直接 CANH/CANL） | 无（需外接 TJA1050 模块） |
| 建议 | 本文按此 | 差异仅在 CAN 接线 |

**确认方法**：看板子正面丝印 / 问卖家。本文默认 ALPHA 板。

---

## 1. 开发模式总览（最关键决策）

i.MX6ULL 是 **ARM A7 + Linux**，不是裸机，开发模式有两条路：

```
路 A：板载直接开发          路 B：PC 交叉编译（推荐组合）
板子运行完整系统(Ubuntu)      PC 上 arm-linux-gnueabihf-gcc
apt 装东西、板载 gcc 编译     编好二进制 scp 到板子跑
优点：最省心、像 PC 开发      优点：编译快、专业、原厂系统也能用
缺点：占资源、需烧 Ubuntu     缺点：环境配置要花一次功夫
```

**本项目推荐组合**（各取所长）：

| 组件 | 用哪种 | 原因 |
|------|--------|------|
| CAN 网关(C单文件) | **交叉编译** | 无依赖、秒编，scp 即跑 |
| Qt 屏程序 | **交叉编译** | Qt 工程板载编译太慢 |
| MQTT broker | 看系统（见下） | Ubuntu 就 apt，原厂系统就交叉编译 |
| V4L2 视频采集 | 板载或交叉编译 | 自写，体现驱动接口级能力 |
| Web 前端 | **PC 浏览器开发** | 静态文件拷板子即可 |

---

## 2. 系统准备（重要！先决定系统）

正点原子 i.MX6ULL 出厂系统是 **Buildroot 精简版**：
- ❌ 无 apt、可能无 gcc / python
- ✅ 自带 Qt 例程、CAN 驱动、UVC 摄像头驱动

**两种做法：**

### 做法 A：烧录正点原子 Ubuntu 镜像（推荐省心）
1. 资料包 → 镜像目录 → 找 i.MX6ULL Ubuntu（或"开发板系统镜像"）
2. 烧录方式：
   - **SD 卡启动**：`dd if=xxx.img of=/dev/sdX bs=1M`（PC 上 Linux 或 win32diskimager）
   - **EMMC 烧录**：用正点原子 MfgTool（资料包工具），USB OTG 线连电脑，MfgTool 一键烧
3. 之后可 `apt install gcc make mosquitto libmosquitto-dev ...` 随便装

### 做法 B：保留出厂 Buildroot，全部交叉编译
- PC 配好交叉工具链（见第 4 节）
- 所有程序 PC 编译 → scp 板子跑
- 缺库就交叉编译对应库（libmosquitto 等）拷到板子

> **建议**：如果资料包里有 Ubuntu 镜像，直接烧 Ubuntu 最省事。
> 本指南后续命令按 Ubuntu 系统写（buildroot 差异处标注）。

---

## 3. 板子基础操作

### 3.1 串口登录
1. 板子 USB 转串口（CH340）→ PC，装驱动
2. MobaXterm / SecureCRT / PuTTY，选串口，波特率 **115200**
3. 上电，回车 → 登录（root，出厂密码通常为空或 root）

### 3.2 网络连接（传文件、Web 访问用）
**方式 1：网线直连 PC（推荐开发）**
```
板子网口 ── 网线 ── PC 网口
板子: ifconfig 查 IP（正点原子常为 192.168.1.232，看实际）
PC : 配同网段静态 IP，如 192.168.1.100
验证: ping 192.168.1.232
```

**方式 2：USB WiFi 连路由器（远程访问用）**
```bash
# Ubuntu 系统
nmcli dev wifi connect "你的WiFi名" password "密码"
# 或
nmtui   # 图形化连 WiFi
ifconfig   # 记下 wlan0 的 IP
```

### 3.3 文件传输（三选一）
```bash
# A. scp（推荐，同网段）
scp gateway_can.c root@192.168.1.232:/root/
# B. U 盘
mount /dev/sda1 /mnt && cp /mnt/file /root/
# C. MobaXterm 直接拖拽（走串口/SSH ZModem）
```

### 3.4 建立工作目录
```bash
mkdir -p /root/monitor/{bin,src,web,qt}
cd /root/monitor
```

---

## 4. 交叉编译环境搭建（PC 上）

### 4.1 工具链
正点原子资料包 → 工具 → 交叉编译器 `arm-linux-gnueabihf-gcc`（gcc-arm-linux-gnueabihf 压缩包）
```bash
# PC 上（Linux）解压到 /usr/local/arm
sudo mkdir -p /usr/local/arm && sudo tar -xjf gcc-arm-xxx.tar.bz2 -C /usr/local/arm
export PATH=/usr/local/arm/gcc-arm-xxx/bin:$PATH   # 可写入 ~/.bashrc
arm-linux-gnueabihf-gcc --version                  # 验证
```
> Windows 用户：可用正点原子提供的 **Ubuntu 虚拟机**（资料包含）或 WSL 跑交叉编译。

### 4.2 编译本项目网关程序
```bash
arm-linux-gnueabihf-gcc gateway_can.c -o gateway_can
file gateway_can   # 应显示 ARM 32-bit，确认编的是 ARM 版
scp gateway_can root@192.168.1.232:/root/monitor/bin/
```

---

## 5. 本项目各阶段在 i.MX6ULL 的做法

### 阶段 2：CAN 网关（已完成代码）
```bash
# 板子上执行
sudo ip link set can0 type can bitrate 500000
sudo ip link set can0 up
./gateway_can can0
```
- 交叉编译后 scp 到板子即可，无依赖
- 调试：`candump can0`、`cansend can0 110#0301020000000000`（模拟下发控制命令）

### 阶段 3：7寸屏 Qt 本地可视化
**开发流程（交叉编译）**：
1. PC 装正点原子 **Qt 交叉编译工具链 + Qt Creator**（资料包"开发板工具/ Qt"目录）
2. 新建 Qt Widgets 工程，Kits 选交叉编译器（arm-linux-gnueabihf）
3. 通过 **SocketCAN 直接读数据**（Qt 里用 QSocketNotifier 监听 can0），或**订阅本地 MQTT**（推荐，见阶段 5）
4. 交叉编译出 ARM 版可执行文件 → scp 到板子
5. 板子上跑（出厂 buildroot 已带 Qt 库）：
   ```bash
   ./monitor_gui -platform linuxfb   # 无桌面系统用 linuxfb 或 eglfs
   ```
**注意**：Qt 程序在 buildroot 下无 X 桌面，用 `-platform linuxfb` 直接刷屏。

### 阶段 5/6：MQTT + Web 仪表盘 + 远程控制
**Ubuntu 系统**：
```bash
apt install mosquitto mosquitto-clients libmosquitto-dev
systemctl start mosquitto
```
**配置 broker 开 WebSocket**（Web 端直连需要）：
```bash
# 编辑 /etc/mosquitto/mosquitto.conf 增加：
listener 1883
listener 8083
protocol websockets
# 重启: systemctl restart mosquitto
```

**网关程序发布/订阅**（升级 gateway_can → 用 libmosquitto）：
```bash
# 编译: arm-linux-gnueabihf-gcc gateway_mqtt.c -o gateway_mqtt -lmosquitto
```

**Web 端（PC 开发，静态文件拷板子）**：
- mqtt.js 通过 `ws://板子IP:8083` 直连 broker
- 页面文件放 `/root/monitor/web/`
- 板子起 HTTP 服务：`python3 -m http.server 8080 --directory /root/monitor/web`
- 访问：`http://板子IP:8080`（手机、PC 同一网络即可）

### 阶段 7：USB 摄像头视频流（自写 V4L2 采集 + MJPEG 推流）
**为什么自写**：MJPG-streamer 是现成工具，面试讲不出"自己实现"的深度；自写 V4L2 能展开讲一整套"驱动接口级"技术（V4L2 接口、mmap、格式协商、HTTP 推流协议）。

**自写方案要点**：
```
1. V4L2 设置 MJPEG 格式（UVC 摄像头硬件编码 JPEG，无需自研压缩）
   → VIDIOC_S_FMT, V4L2_PIX_FMT_MJPEG
2. mmap 映射帧缓冲 → VIDIOC_REQBUFS + VIDIOC_QUERYBUF + mmap
3. 采集线程循环取帧 → VIDIOC_DQBUF/QBUF
4. 主线程 select 管理多客户端，打包 HTTP multipart/x-mixed-replace 推流
```

**编译与运行**：
```bash
arm-linux-gnueabihf-gcc video_v4l2.c -o video_v4l2 -lpthread
scp video_v4l2 root@板子IP:/root/monitor/bin/
./video_v4l2 /dev/video0 8081
```
**Web 端**：`<img src="http://板子IP:8081/?action=stream">`
**排查**：
- `ls /dev/video*`、`dmesg | grep uvc`（确认 UVC 驱动识别摄像头）
- `v4l2-ctl --list-formats -d /dev/video0`（确认摄像头支持 MJPEG）
- 若摄像头只支持 YUYV：需要加 libjpeg 软件压缩（`apt install libjpeg-dev`，`-ljpeg`）

### 阶段 8：GPS 定位 + 地图
- 接 UART（如 `/dev/ttymxc2`，9600 波特率，NMEA 0183）
- **方案 1（简单）**：`apt install gpsd gpsd-clients`，配串口，`gpsmon` 看定位
- **方案 2（体现功底，推荐）**：网关程序自己解析 `$GPGGA/$GPRMC` 帧，提取经纬度
- Web 端 **Leaflet** 地图打点（离线瓦片或高德/百度），通过 MQTT 收经纬度实时移动标记

---

## 6. 调试手段速查

| 场景 | 命令/工具 |
|------|----------|
| 看内核/驱动 | `dmesg \| grep -i can/uvc/wlan` |
| 看 CAN 数据 | `candump can0` |
| 模拟 CAN 下发 | `cansend can0 110#0301020000000000` |
| 看 MQTT 流量 | `mosquitto_sub -t '#' -v` |
| 看进程/CPU | `top`、`ps aux` |
| 网络抓包 | `tcpdump -i can0`（CAN 也可抓） |
| 板载 GDB 调试 | `gdb ./程序`（buildroot 可能无，用交叉 gdb） |
| 串口打印 | 程序里 printf → /dev/ttymxc0 |

---

## 7. 推荐开发工作流（本项目）

```
① PC 上写代码（VS Code）
② 单文件 C（网关）  → 交叉编译 → scp → 板子跑
③ Qt 大工程        → PC 交叉编译 → scp → 板子 -platform linuxfb 跑
④ Web 前端         → PC 浏览器开发调试 → 静态文件 scp → 板子 http server
⑤ 全链路联调       → MQTT 数据流验证 + Web 截图
⑥ 录演示视频       → 手机/相机录"屏上数据 + Web 控制 + 小车动作"
```

---

## 8. 常见坑

| 坑 | 解决 |
|----|------|
| scp 连不上 | 确认同网段、`ifconfig` 查 IP、关 PC 防火墙 |
| 编译出的程序跑不了（No such file） | 编错架构了，`file` 查是否 ARM；或缺动态库，用 `arm-linux-gnueabihf-ldd` 查 |
| Qt 黑屏/花屏 | 用 `-platform linuxfb`，确认分辨率匹配 |
| WiFi 连不上 | 确认 USB WiFi 芯片驱动加载（`dmesg` 看 wlan） |
| 摄像头没图像 | `ls /dev/video*`、`dmesg | grep uvc`，换 UVC 免驱摄像头 |
| MQTT 连接被拒 | broker 没开对应端口/协议，检查 mosquitto.conf |
| 板子空间不足 | `df -h`，删编译中间文件，日志别存板子 |
