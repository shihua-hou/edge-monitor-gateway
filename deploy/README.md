# 部署说明

系统分两侧部署：**机器人侧**（会移动、会掉线）和 **服务器侧**（固定、常在线）。

```
巡检机器人 (i.MX6ULL)                    服务器 / VM
├─ gateway_mqtt   CAN↔MQTT 网关          ├─ mosquitto     MQTT broker
├─ video_v4l2     摄像头 MJPEG 推流      └─ nginx/http    Web 大屏静态文件
├─ gps_mqtt       GPS 解析
├─ ota_service    远程升级
└─ edgemonitor_ui Qt 触摸屏（现场用）
```

**broker 和 Web 为什么在服务器侧**，见根目录 [README](../README.md#系统架构)。
一句话：监控系统不能跟被监控对象同生共死，而且 MQTT 的 LWT 遗嘱机制要求
broker 在设备之外才有意义。

---

## 一、服务器侧（VM / 任意常开机器）

### 1. MQTT broker

```bash
sudo apt install -y mosquitto mosquitto-clients
sudo cp gateway_imx6ull/mosquitto.conf /etc/mosquitto/conf.d/edgemonitor.conf
sudo mosquitto_passwd -c /etc/mosquitto/passwd em     # 按提示设密码
sudo systemctl restart mosquitto
```

`mosquitto.conf` 里已经开了 1883(TCP) 和 8083(WebSocket) 两个 listener，
分别给网关和浏览器用；8084 是预留的 TLS listener，台架联调默认不启用。

**注意**：broker 要监听 `0.0.0.0` 而不是 `localhost`，否则机器人连不进来。

### 2. 历史数据服务

订阅 MQTT 落 SQLite，并提供历史查询接口给大屏。跟网关完全解耦——
网关一行代码都不用改，加设备也不用改它。

```bash
sudo apt install -y python3-pip && pip3 install paho-mqtt
sudo mkdir -p /opt/edgemonitor
sudo cp server/history_logger.py /opt/edgemonitor/
sudo cp deploy/edgemonitor-server.env.example /etc/edgemonitor-server.env
sudo chmod 600 /etc/edgemonitor-server.env
sudo vi /etc/edgemonitor-server.env          # 填 broker 密码
sudo cp deploy/systemd/edgemonitor-history.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now edgemonitor-history
```

验证：

```bash
curl 'http://localhost:8090/api/history?metric=temp&hours=1'
curl 'http://localhost:8090/api/events?limit=10'
```

**降采样说明**：传感器 200ms 上报一次，全存的话一天 43 万条纯属浪费，
曲线上也看不出差别。按 5 秒间隔采样存储，但两类情况例外，一条不漏：
- **状态字跳变**（LED/蜂鸣器/电机开关）立即落库，否则"什么时候开的灯"会被采样吃掉
- **事件**（设备上下线、越界告警）本身就稀疏且重要，全存

### 3. Web 大屏

静态文件，扔给任意 http server 即可：

```bash
sudo cp web/index.html web/app.js /var/www/html/
```

没装 nginx 的话，临时用 Python 起一个也行：

```bash
cd web && python3 -m http.server 8080
```

浏览器打开后，登录页的 **broker 地址填服务器自己的 IP**（不是机器人的）。

---

## 二、机器人侧（i.MX6ULL）

### 1. 放二进制

交叉编译产物放到 `/root/monitor/bin/`：

```bash
mkdir -p /root/monitor/bin /root/monitor/fw
```

### 2. 配环境变量

```bash
cp deploy/edgemonitor.env.example /etc/edgemonitor.env
chmod 600 /etc/edgemonitor.env       # 里面有 broker 密码
vi /etc/edgemonitor.env              # 把 MQTT_HOST 改成服务器 IP、填密码、设 DEVICE_ID
```

**最容易错的一步**：`MQTT_HOST` 要填**服务器的地址**。填成 localhost 就退回旧架构了
（网关会去连本机根本不存在的 broker）。

### 3. 装 systemd 服务

```bash
cp deploy/systemd/*.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now edgemonitor-gateway edgemonitor-video edgemonitor-gps edgemonitor-ota
```

这样进程崩了会自动拉起（`Restart=always`），开机也会自启。
之前进程一挂就彻底没人管，现象是"数据突然不更新了"，还得人爬上去看才知道进程没了。

### 4. 看日志

```bash
journalctl -u edgemonitor-gateway -f
```

正常启动应该能看到：

```
CAN can0 ready
device=robot01 ip=192.168.100.3, online topic=monitor/online/robot01
[mqtt] connected, subscribed: monitor/cmd, online -> monitor/online/robot01
```

---

## 三、验证清单

| 验证项 | 方法 | 预期 |
|---|---|---|
| 设备上线通告 | 服务器上 `mosquitto_sub -h localhost -u em -P 密码 -t 'monitor/online/#' -v` | 收到 `{"online":1,"dev":"robot01","ip":...}` |
| **掉线检测** | 拔掉机器人网线，等 keepalive 超时（约 60~90s） | 同一 topic 收到 `{"online":0,...}`，大屏状态转红并升告警 |
| 重连恢复订阅 | 网线插回，然后从 Web 点一个控制按钮 | 机器人能收到命令（验证 on_connect 里的重新订阅生效） |
| 视频自动挂载 | 大屏登录后不做任何配置 | 视频源自动指向设备通告的 IP，不再用 broker 地址 |
| 进程自恢复 | `kill -9` 掉 gateway_mqtt | 3 秒后自动重启，大屏短暂离线后恢复 |

**掉线检测那条是这次架构调整的核心收益**——旧架构（broker 跟网关同机）下这个测试
根本做不了：拔网线时 broker 跟设备一起没了，没人替它发遗嘱。
