# EdgeMonitor —— 基于 CAN 总线的多传感器远程监控与 OTA 固件升级系统

面向**园区巡检机器人**的移动设备远程监控与运维平台原型。

多源传感采集 → CAN 总线 → 嵌入式 Linux 边缘网关 → MQTT → Web 数据大屏，
外加本地 Qt 触摸屏、V4L2 视频回传、UDS/A-B 双分区 OTA、电机闭环控制，
形成「感知 → 控制 → 边缘 → 云 → 视觉 → 升级」的完整闭环。

> 这个仓库里除了代码，还有一份 [`deploy/优化记录.md`](deploy/优化记录.md)（40+ 批次）
> 记录了每一个真实故障的**现象 / 定位过程 / 根因 / 验证方法**。
> 如果你只想看一样东西，看那个 —— 代码能说明"做了什么"，那份记录说明"为什么这么做"。

---

## 一、系统架构

```
   ── 巡检机器人（会移动、会掉线）──            ── 服务器 / VM（固定、常在线）──

[超声波][MPU6050][DHT11][光敏]      [GPS]
[TB6612+编码器电机×2]                 │ UART/NMEA
        │                            │
   STM32F103 (FreeRTOS)              │
   采集 + 电机 PI 闭环 + 看门狗        │
        │ CAN 500K                    ▼
        └──────────► i.MX6ULL 边缘网关 ─────┐
                     ├─ 7寸RGB屏 (Qt 现场中控 + 语音助手)
                     ├─ USB摄像头 (自写 V4L2 + MJPEG 推流, token 鉴权)
                     ├─ 远程屏幕 (framebuffer 抓屏 + 触摸注入)
                     └─ MQTT client + LWT 遗嘱 ──WiFi──►  MQTT broker (认证)
                                    ▲                          │
                                    └──── 控制下行 ────────────┤
                                                               ├─ Web 数据大屏
                                                               └─ history_logger
                                                                  (MQTT→SQLite+查询API)
```

### 为什么 broker 和 Web 大屏放在服务器侧，而不是跟网关同机

1. **设备掉线时监控还得在** —— 监控系统跟被监控对象同生共死，等于没有监控。
2. **MQTT LWT 遗嘱机制结构上要求 broker 在设备之外** —— 拔网线/断电时设备自己
   已经发不出任何消息，只能靠 broker 在 keepalive 超时后代发遗嘱。broker 跟设备
   同机的话，它俩一起没了，大屏只会永远停在"最后一帧数据"上，**分不清是设备
   静默还是真掉线**。
3. **多设备汇聚** —— 一个大屏看 N 台机器人，靠 `DEVICE_ID` 区分，而不是记 N 个 IP。
4. **资源** —— 6U 是单核 Cortex-A7，静态文件服务和 broker 没必要占它的算力。

Qt 触摸屏留在机器人上是对的：那是给**现场**巡检人员用的。视频流也仍由机器人直供
（流量大，经服务器中转不划算），设备离线时视频不可用是合理的——那时本来就没有画面。

### 设备自报家门

网关上线时在 `monitor/online/<device_id>` 发布 **retained** 消息：

```json
{"online":1,"dev":"robot01","ip":"192.168.x.x","video":8081}
```

大屏据此自动挂载视频源 —— 设备换 IP、加设备都不用改任何配置。
掉线时 broker 按遗嘱把同一 topic 覆盖成 `{"online":0,...}`，大屏立刻转离线态并摘掉视频源。

---

## 二、快速开始

### 依赖

| 组件 | 工具链 |
|---|---|
| STM32 固件 | Keil MDK5 + ARMCC V5（工程自带 HAL 库和 FreeRTOS） |
| i.MX6ULL 网关 / Qt | Poky SDK `fsl-imx-x11-glibc-x86_64-meta-toolchain-qt5-cortexa7hf-neon 4.1.15-2.1.0` |
| 服务器侧 | Python 3.6+、mosquitto 1.6+ |

### 1. STM32 固件

```
Keil 打开 EdgeMonitor_App/Projects/MDK-ARM/atk_f103.uvprojx
两个 target 都要 Rebuild：FreeRTOS_SlotA 和 FreeRTOS_SlotB
```

> A/B 双分区用的是 **direct-xip**：两个槽的固件链接地址不同（`0x08004000` / `0x08040000`），
> 是**两份不能互换的 bin**，必须都编。

Bootloader 在 `EdgeMonitor_Boot/`，只需烧一次（J-Link/ISP）。

⚠️ **Keil → Debug → Settings → Port 必须选 SW，不能选 JTAG。**
固件里调了 `__HAL_AFIO_REMAP_SWJ_NOJTAG()` 释放 PA15/PB3/PB4 给编码器用，
选 JTAG 会导致烧录后再也连不上（要拉 BOOT0 进 ISP 才能救）。

### 2. i.MX6ULL 网关 / Qt 界面

```sh
. /opt/fsl-imx-x11/4.1.15-2.1.0/environment-setup-cortexa7hf-neon-poky-linux-gnueabi

# 网关
$CC gateway_imx6ull/gateway_mqtt.c -o gateway_mqtt \
    -I mosquitto-1.6.15/lib -L mosquitto-1.6.15/lib -lmosquitto -lpthread

# Qt 中控
cd imx6ull_ui && qmake && make -j4
```

> **必须先 source SDK 环境**，否则 `make` 会用宿主机的 gcc 静默产出 x86 二进制。

### 3. 服务器侧

```sh
# broker
mosquitto -c gateway_imx6ull/mosquitto.conf -d

# 历史落库 + 查询 API
cp deploy/edgemonitor-server.env.example /etc/edgemonitor-server.env   # 改密码
python3 server/history_logger.py

# Web 大屏（静态文件）
cd web && python3 -m http.server 8080
```

systemd / sysvinit 单元见 [`deploy/`](deploy/)。

### 4. 设备侧配置

```sh
cp deploy/edgemonitor.env.example /etc/edgemonitor.env
chmod 600 /etc/edgemonitor.env      # 里面有 broker 密码
vi /etc/edgemonitor.env             # MQTT_HOST 填服务器地址，不是 localhost
cp deploy/sysvinit/edgemonitor-daemon.sh /home/root/ && chmod +x /home/root/edgemonitor-daemon.sh
```

> **`/etc/edgemonitor.env` 是 broker 地址的唯一来源。** 网关、OTA 服务、板子上的
> Qt 界面都从这里取值。改完之后 **已在运行的进程不会重读**（`getenv` 只在
> `main()` 里执行一次），必须 `killall gateway_mqtt ota_service edgemonitor_ui`
> 让守护脚本重新拉起。

---

## 三、硬件与接线

### 清单

- 正点原子战舰 STM32F103ZET6（板载 TJA1050 CAN 收发器）
- 传感器：HC-SR04 超声波、MPU6050、DHT11、光敏电阻
- 执行器：LED、蜂鸣器、**WHEELTEC TB6612 稳压版驱动 + MG513 P28 编码器电机 ×2 + Φ65 轮**
- i.MX6ULL + 7 寸 RGB 屏 + USB 摄像头 + USB WiFi + GPS
- CAN 双绞线 + **共地线** + 120Ω 终端电阻

### STM32 引脚分配

| 功能 | 引脚 | 说明 |
|---|---|---|
| CAN1 | PA11 / PA12 | RX / TX |
| USART1 | PA9 / PA10 | 调试串口 115200 |
| DHT11 | PG11 | 单总线 |
| HC-SR04 | PE6 (TRIG) / PB6 (ECHO) | ECHO = TIM4_CH1 输入捕获 |
| 光敏 | PA1 | ADC1_CH1 |
| 电池电压 | PC5 | ADC1_CH15，经模块 100k/10k 分压 |
| MPU6050 | PB10 / PB11 | 软件 I2C |
| LED / 蜂鸣器 | PE5 / PB8 | |
| **电机 PWM** | PC6 / PC7 | TIM8_CH1/CH2，20kHz |
| **方向** | PC8~PC11 | AIN1/AIN2/BIN1/BIN2 |
| **STBY** | PA8 | 拉低 = 硬件急停 |
| **左轮编码器** | PB4 / PB5 | TIM3 部分重映射 |
| **右轮编码器** | PA15 / PB3 | TIM2 部分重映射 1 |

### TB6612 模块接线（全部集中在战舰板 P2 排针）

| 模块 | 丝印 | → STM32 |
|---|---|---|
| H1-7 / H1-1 | PWMA / PWMB | PC6 / PC7 |
| H1-5 / H1-6 | AIN1 / AIN2 | PC8 / PC9 |
| H1-3 / H1-2 | BIN1 / BIN2 | PC10 / PC11 |
| H1-4 | STBY | PA8 |
| H2-4 / H2-3 | E1A / E1B | PB4 / PB5 |
| H2-2 / H2-1 | E2A / E2B | PA15 / PB3 |
| H2-6 | GND | GND（**必接**） |
| H2-5 | ADC | PC5 |

⚠️ **H1 那排 7 个脚全是信号、一根 GND 都没有**，共地必须从 H2-6 单独引。
不共地的话逻辑电平没有参考，行为完全不可预期。

⚠️ **12V 只进模块的 J8，绝不能碰 STM32 的任何电源脚。**
也**不要**从模块的 5V 给主控供电 —— 模块过温保护会切断板载 5V/3.3V，
电机堵转发热正是最需要遥测的时刻，主控却跟着掉电了。

---

## 四、协议速查

### CAN（500 kbps，详见 [`docs/CAN通信协议.md`](docs/CAN通信协议.md)）

| ID | 方向 | 内容 |
|---|---|---|
| `0x100` | STM32 → 网关 | 温度(2B 大端) / 光照 / 距离 / 状态字 / 湿度 |
| `0x101` | STM32 → 网关 | 俯仰 / 横滚 / 偏航 + **传感器健康位** + **电池电压** |
| `0x102` | STM32 → 网关 | 左右轮转速(**有符号**) / 里程 |
| `0x110` | 网关 → STM32 | 控制命令（LED / 蜂鸣器 / 电机 / **急停**） |
| `0x111` | 网关 → STM32 | 参数配置（上报周期 / 告警阈值） |
| `0x130` / `0x120` | 双向 | App 诊断请求 / UDS 响应 |

状态字 bit：`bit0` LED、`bit1` 蜂鸣器、`bit2` 电机在转（**由编码器实测推导**）、`bit3` 急停。

传感器健康位：`bit0` 温湿度、`bit1` 超声波、`bit2` 光敏、`bit3` IMU、`bit4` 电机驱动。

### MQTT Topic

| Topic | 方向 | Payload |
|---|---|---|
| `monitor/<dev>/data/status` | 上行 | `{"t":24.6,"h":63,"l":73,"d":15,"s":0}` |
| `monitor/<dev>/data/imu` | 上行 | `{"p":0.5,"r":-0.2,"y":11.5,"hf":0,"bat":12.1}` |
| `monitor/<dev>/data/motor` | 上行 | `{"rl":39,"rr":40,"ol":109,"or":109}` |
| `monitor/<dev>/data/gps` | 上行 | `{"lat":...,"lon":...}` |
| `monitor/online/<dev>` | 上行(retained+LWT) | `{"online":1,"ip":"...","video":8081}` |
| `monitor/<dev>/cmd` | 下行 | `{"dev":3,"act":1,"p1":20}` |
| `monitor/<dev>/ota/cmd` / `ota/status` | 双向 | 固件升级 |
| `monitor/<dev>/config/voice` | 下行(retained) | 语音密钥下发 |

---

## 五、核心功能

### 电机闭环（20ms 周期增量式 PI）

目标量是**转速**而不是 PWM 占空比：同样的占空比，空载和爬坡的实际转速差很远，
直线走着走着就偏了；以转速为目标，上坡时 PI 会自动把占空比顶上去。

实测（目标 40 RPM）：

| 命令 | 左 RPM | 右 RPM |
|---|---|---|
| 前进 20% | +39.0 ± 1.0 | +39.5 ± 1.5 |
| 后退 20% | −38.1 | −38.2 |
| 左转 | −36.5 | +37.4 |
| 右转 | +34.6 | −34.1 |

跑 3.5 米后左右累计里程都是 355 cm，**误差 0**。

**飞车保护**：任一侧转速超上限 1.5 倍并持续 100ms → 拉低 STBY 硬停 + 上报故障位，
**不自动恢复**（需下发 `ACT_RESUME`）。判据用"转速失控"而不是"输出饱和"——
爬坡载重时输出饱和是正常的，只有反馈出错时转速才会失控地涨。

### A/B 双分区 OTA（UDS over CAN）

- direct-xip：两个槽的固件链接地址不同，`SCB->VTOR` 运行期设置，**绝不写死**
- 目标槽由**设备自己**计算（= 非活动槽），升级永远不碰正在运行的固件
- 安全访问：`0x27` 动态种子（复位计数器 ^ 混淆）+ **XTEA** 加密密钥
- 启动确认：App 连续成功上报 25 次（约 5s）才"确认"，否则 Boot 试满 3 次自动回滚
- Boot 里**不喂看门狗** —— 擦除 App 区要 5~10 秒，远超 4 秒超时，喂了 OTA 直接废掉

板子装车后下载口不便，后续固件全部走 OTA 更新，已连续 6 次零失败。

### 其余

- **自写 V4L2 + MJPEG 推流**，URL token 鉴权（`SHA-256(user:pass)` 前 16 位）
- **远程屏幕**：抓 framebuffer + 注入 `/dev/input` 触摸事件，浏览器里直接操作板子
- **讯飞语音助手**：听写 / 合成 / 星火大模型，密钥由 Web 端经 retained 消息下发
- **历史数据**：MQTT → SQLite（WAL 模式），HTTP 查询 API，网页回看曲线
- **多设备**：topic 按 `DEVICE_ID` 隔离，一个大屏管多台

---

## 六、可靠性设计

这套系统的运行前提是**没人在现场**。所以设计重点不只是"功能能用"，
而是**故障发生时能不能被发现、能不能自己恢复**。下面每一条都来自真机联调中实际踩到的问题。

### 1. 不让故障伪装成正常

监控系统最危险的失败不是"报错"，而是**"看起来一切正常"**。

| 原本的表现 | 为什么危险 | 现在怎么做 |
|---|---|---|
| DHT11 读失败 → 保留上次值 | 大屏上是个稳定的数字 | CAN 帧带**传感器健康位**，界面标"已失联" |
| 超声波超时 → 返回 0 | 看着像"贴着障碍物"，是合法读数 | 0 判为失败，不写入数值 |
| IMU 读失败 → 三角清 0 | 0° 恰是最不可疑的姿态 | 保持上次角度 + 置故障位 |
| 陈旧值继续参与告警 | 传感器在 41℃ 时坏掉 → 永远重复报"温度过高" | 失联传感器不参与告警判定 |
| 电机状态由"最后一条命令"推导 | 定时转向到期 / 急停 / 电机堵死，三种情况都会说谎 | **由编码器实测推导** |
| 电池 ADC 线没接 → 报 0.0V | 一条永不消失的假告警 | 低于 3V 判为"未接入"，不告警 |
| 命令发出去就把按钮变色 | 只证明消息发出去了 | **等状态位回传才算确认**，并显示往返时延 |

> **假告警看多了，真告警也就没人信了 —— 那比没有告警更糟。**

### 2. 可恢复的故障要自己恢复

| 故障 | 表现 | 自愈方式 |
|---|---|---|
| WiFi 关联僵死 | AP 显示"已连接"但不通 | 守护脚本探测网关，连续失败自动重连（带冷却） |
| CAN bus-off | 进程活着、MQTT 连着，就是没数据 | 自动 `down/up` 重新初始化控制器，指数退避 |
| 任务跑飞 | 屏幕卡住不动 | IWDG **签到式**喂狗：两个周期任务都打卡才喂一次 |
| 升级后固件起不来 | 变砖 | A/B 双分区 + 启动确认 + 自动回滚 |
| 时钟漂移 | 云服务鉴权 401 | 联网后用 HTTP 响应头的 `Date` 校时 |
| 日志撑满 tmpfs | 跑几天系统变慢 | 守护脚本按大小就地截断 |

### 3. 三个反复出现的 bug 模式

调试过程中同一类问题出现了多次，值得单独列出来：

**① 注释描述的是意图，代码路径才是事实**

| 位置 | 注释承诺 | 代码实际 |
|---|---|---|
| `mqttclient.cpp` | "loop_start 会自动重连" | `connect_async` 失败就 `return`，`loop_start` 永远没被调用 |
| `weatherclient.cpp` | "失败也照常查天气" | 错误分支直接 `return`，`fetchWeather()` 走不到 |

写下"失败时也会 xxx"时，必须确认那条路径真的可达 ——
最省事的验证是**把失败故意造出来跑一遍**。

**② 重试间隔必须和故障的时间尺度匹配**

开机竞态的故障窗口是秒级（WiFi 还没关联好），而天气刷新间隔是 30 分钟。
故障持续几秒、重试间隔半小时，这个比例本身就是 bug。

**③ 配置有多个来源 = 迟早会不一致**

broker 地址曾经散在三处（`/etc/edgemonitor.env`、Qt 的 QSettings、浏览器 localStorage）。
虚拟机换一次 IP 之后只改了第一处，结果是"网关连上了、数据也入库了，
可是板子屏幕上什么都不显示"—— 而这个现象从表面上根本看不出是配置不一致。
现在收敛成单一来源，QSettings 只在环境变量缺失时兜底且不落盘。

### 4. 可观测性

排查的转折点往往不是"想到了什么"，而是"能看到什么"。日志是有目的地加的：

- `[xfyun] sign date=...` —— 把"时间戳"这个变量从 401 里摘出去。三个完全不同的
  根因（时钟偏移 / URL 编码 bug / 密钥抄错）都报 401，没有这行只能靠猜。
- 反过来，mosquitto 的 DEBUG 日志被**关掉**了 —— 每秒 5 条 `received PUBLISH`
  会把真正的错误埋掉。**日志的作用是让问题浮出来，不是把问题埋进去。**

---

## 七、安全设计

| 项 | 实现 |
|---|---|
| UDS `0x27` 安全访问 | 动态种子（复位计数器 ^ 混淆）+ **XTEA** 加密密钥 |
| MQTT broker | 禁匿名 + 账号密码 + 可选 ACL（默认不启用，需手动开） |
| Web 登录 | 凭据即 broker 凭据，未登录不可见数据/不可控；网关对下发命令做取值范围校验 |
| 视频鉴权 | URL token = `SHA-256(用户名:密码)` 前 16 位，不是密码明文 |
| 传输加密 | 预留 8084 TLS listener，https 下自动切 wss；本地台架默认明文 |
| 凭据不入库 | 所有密钥（讯飞 / 和风 / MQTT）都经**运行期配置**下发到设备，源码里没有任何硬编码凭据 |

> XTEA 的密钥是编译期常量，随源码开源 —— 这一点在 [方案书 4.4](docs/项目方案书.md)
> 里有诚实说明：它挡的是"随便接根 CAN 线就能刷固件"，挡不住能拿到固件的人。
> 真正的方案是非对称签名 + 安全启动，需要硬件支持。

---

## 八、目录说明

```
firmware_stm32/       STM32 固件源码（归档参考副本，与 Keil 工程双向同步）
  ├─ sensor/          DHT11 / HC-SR04 / 光敏ADC+电池 / MPU6050 软件I2C
  ├─ motor.c/.h       TB6612 双路驱动 + 正交编码器 PI 闭环 + 飞车保护
  └─ bootloader/      Boot 区（UDS + XTEA + Flash + CRC + A/B 回滚）
EdgeMonitor_App/      STM32 App 的 Keil 工程（SlotA / SlotB 两个 target）
EdgeMonitor_Boot/     Bootloader 的 Keil 工程
gateway_imx6ull/      网关：CAN↔MQTT、V4L2 视频、远程屏幕、OTA 服务、uds_tool
imx6ull_ui/           7 寸屏 Qt 中控（首页/控制/语音助手/设置 + 自写软键盘）
web/                  Web 数据大屏（登录/大屏/控制/历史/告警/远程屏幕/OTA/设置）
server/               history_logger.py：MQTT→SQLite 落库 + HTTP 查询接口
docs/                 协议文档、方案书、Bootloader/UDS 设计、开发环境搭建
deploy/               部署脚本、systemd/sysvinit 单元、优化记录
scripts/              开发辅助工具（见下）
```

### `scripts/` 里的工具

| 工具 | 用途 |
|---|---|
| `sync_firmware.py` | STM32 两份源码副本的同步与差异比对 |
| `brace_check.py` | 无交叉编译器时的静态快查：括号平衡 + **字符串断行检测** |
| `safe_edit.py` | 原子写入（写临时文件 → 校验 → `os.replace`） |
| `xtea_selftest.py` | XTEA 实现自测（板子和上位机两侧算法一致性） |

> `safe_edit.py` 的存在有个具体理由：`open(path, 'w')` **在打开的那一刻就把文件
> 截断成 0 字节**，之后才写入。写入过程中一旦抛异常，文件就永久停在空的状态 ——
> 而这个失败模式和"脚本没跑成功"看起来一模一样。这个仓库曾因此丢过一份文档。

---

## 九、已知限制

诚实列出来，避免看代码的人误以为这些已经做好了：

| 项 | 状态 |
|---|---|
| 编码器标定 | `ENC_CNT_PER_REV = 13×4×28` 中减速比已由供应商确认，**整体标定未实测验证**。转速和里程可能整体差一个固定比例 |
| GPS | 代码就绪，硬件未接入 |
| TLS | mosquitto 预留了 8084 listener，未实际启用 |
| ACL | 配置样例已给，默认不启用 |
| 急停 | 固件支持 `ACT_ESTOP`，Web 大屏尚无按钮 |
| 电池电压 | 仅 Web 端显示，Qt 界面未显示 |
| `adc_light.c` | 已扩成 ADC1 的独占持有者（光敏 + 电池两路），文件名不再贴切，待重命名 |
| 视频 / 远程屏幕 | 走明文 HTTP，仅适合内网 |

---

## 十、开发记录

[`deploy/优化记录.md`](deploy/优化记录.md) 按批次记录了每一处改动的
**问题 / 根因 / 改动 / 验证方法**，包括：

- 超声波"传感器坏了"其实是 PB6 浮空 —— 浮空脚被 TRIG 边沿容性耦合，
  把"断线"伪装成了"模块卡死"
- 桥接虚拟机在 WiFi 上的 ARP 陷阱 —— 从"单向可达"这个现象倒推到
  802.11 一个 station 只能有一个 MAC
- 板子 CA 证书 7 年没更新 —— Qt 报 `SSL handshake failed`，实际握手是成功的，
  死在证书验证；`openssl s_client` 的 `verify return code` 那一行才是实话
- 左轮飞车 —— 模块把电机极性做成镜像却没把编码器跟着镜像，
  负反馈变成正反馈；用**不通电的手转测试**分辨出"电机反转"和"编码器反相"

---

## 许可

本仓库为个人学习/求职作品。第三方材料（正点原子官方例程、SDK、文档）
不包含在本仓库内，请自行从官方渠道获取。
