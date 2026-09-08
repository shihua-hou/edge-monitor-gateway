# 虚拟机网络：从桥接改成 NAT + 端口转发

> 目的：根治「板子连不上虚拟机」这个反复出现的故障。
> 现状的临时方案（VM 定时 ping 板子保活 ARP）是治标，本文是治本。

## 一、先说清楚坏在哪

板子的 ARP 表长这样：

```
192.168.100.240  ->  2c:9c:58:c3:f1:03    ← 宿主机 PC 的 MAC
192.168.100.7    ->  2c:9c:58:c3:f1:03    ← 同一个
```

两个不同的 IP，指向同一个 MAC。这不是配置错了，是 **VMware 把虚拟网卡桥接
到宿主机的 WiFi 网卡**时必然的结果：

- 以太网可以让一个物理口后面挂多个 MAC，交换机自己会学。
- **WiFi 不行**。一个 station 关联到 AP 时只登记一个 MAC，802.11 的数据帧里
  没有给"第二个 MAC"留位置（要做到得靠 WDS / 4-address 模式，随身 WiFi
  这类 CPE 基本都不支持）。

所以 VMware 只能做 **MAC 转换**：虚拟机的包出去时换成宿主机的 MAC，回来的包
再按 IP 分发给虚拟机。代价是：

> **这个 IP→MAC 映射只有在虚拟机主动发过包之后才存在，而且 30 秒就老化**
> （Linux 的 `base_reachable_time_ms` 默认 30000）。

而业务方向正好相反：**MQTT 是板子主动连虚拟机**。映射一掉，板子 ARP 不到
虚拟机，就永远连不上——但虚拟机 ping 板子却是通的，呈现出诡异的**单向可达**。

实际事故表现：网关日志正常、数据库有数据、Web 大屏能开，就是板子屏幕全空。
排查时很容易先怀疑 MQTT 认证、topic、防火墙，全都不是。

## 二、NAT 方案为什么能根治

把虚拟机网卡改成 **NAT**，虚拟机退到宿主机后面的私有网段（如 192.168.157.x），
然后在宿主机上做端口转发：

```
板子 192.168.100.3
   │  连 192.168.100.7:1883        ← PC 的地址，PC 是真实 WiFi station
   ▼
宿主机 PC 192.168.100.7 (WiFi)
   │  netsh portproxy 转发
   ▼
虚拟机 192.168.157.134:1883
```

关键点：**板子只跟 PC 这一个真实 station 通信**。PC 的 MAC 是 AP 正经登记过的，
ARP 永远解析得到，不存在"要先主动发包才可达"的问题。

代价是多一跳转发（局域网内可忽略），以及宿主机必须开机——但虚拟机本来就跑在
宿主机上，这不是新增约束。

## 三、操作步骤

### 1. 确认虚拟机有 NAT 网卡

本项目的虚拟机**已经同时挂了两块网卡**，不需要改 VMware 设置：

```sh
ip -4 -o addr show | grep -v ' lo '
# ens33  192.168.100.240/24   桥接（就是有问题的那块）
# ens37  192.168.157.134/24   NAT ← 用这块
```

如果你的虚拟机只有桥接一块，就去 虚拟机 → 设置 → 网络适配器 → 添加一块
**NAT 模式**的网卡（不必删掉桥接那块，留着不影响）。

记下 NAT 地址（下面记作 `<VM_IP>`）。宿主机能直接访问这个网段（走 VMnet8
虚拟网卡），先自己验一下：

```powershell
Test-NetConnection <VM_IP> -Port 1883
```

> **NAT 网段的地址也是 DHCP 发的**，长期跑要在虚拟机里配静态：
> ```sh
> CON=$(nmcli -t -f NAME,DEVICE con show --active | grep ':ens37$' | cut -d: -f1)
> sudo nmcli con mod "$CON" ipv4.method manual \
>      ipv4.addresses <VM_IP>/24 ipv4.gateway 192.168.157.2 \
>      ipv4.dns "192.168.157.2 223.5.5.5"
> sudo nmcli con up "$CON"
> ```
> 网关和 DNS 用 VMware 的 NAT 网关（通常是网段的 .2）。
> 这一步不做的话，NAT 地址变了转发规则就指空，等于换个姿势重犯今天的错。

### 2. 宿主机：加端口转发

**以管理员身份**打开 PowerShell：

```powershell
$VM = "192.168.157.134"   # 换成实际的 VM_IP
foreach ($p in 1883,8083,8080,8090) {
  netsh interface portproxy add v4tov4 listenport=$p listenaddress=0.0.0.0 connectport=$p connectaddress=$VM
}
netsh interface portproxy show v4tov4
```

四个端口分别是：MQTT / MQTT-over-WebSocket / Web 大屏 / 历史数据 API。

> `listenaddress=0.0.0.0` 是必须的。写 127.0.0.1 只会监听回环，板子过不来——
> 这是这一步最常见的错。

### 3. 宿主机：放行防火墙

Windows 防火墙默认会拦掉这些入站连接，而且**拦得很安静**：板子那边表现为
连接超时，宿主机上一点提示都没有。

```powershell
New-NetFirewallRule -DisplayName "EdgeMonitor 内网入站" -Direction Inbound `
  -Protocol TCP -LocalPort 1883,8083,8080,8090 -Action Allow -Profile Private
```

> `-Profile Private` 而不是 Any：只在"专用网络"生效。如果你的 WiFi 在
> Windows 里被识别成"公用网络"，规则不会生效——去 设置 → 网络 → 属性
> 把它改成"专用网络"，或者把 Profile 改成 Any（安全性差一些）。

### 4. 板子和 Web 端改地址

板子（**唯一来源**，见 `deploy/sysvinit/README.md`）：

```sh
sed -i 's/^MQTT_HOST=.*/MQTT_HOST=192.168.100.7/' /etc/edgemonitor.env
killall gateway_mqtt ota_service edgemonitor_ui    # 已在跑的进程不会重读，必须重启
```

Web 大屏：登录框留空即可——它默认用 `location.hostname`，而页面本身就是从
转发端口打开的，天然指向 PC。

### 5. 撤掉临时方案

NAT 生效后，ARP 保活的 cron 就没用了：

```sh
crontab -l | grep -v arp_keepalive | crontab -
rm -f /home/user/edgemonitor/arp_keepalive.sh
```

## 四、验证

```sh
# 板子上
ping -c2 192.168.100.7
# 数据库有新行（在 VM 上）
python3 -c "
import sqlite3,time
c=sqlite3.connect('file:/home/user/edgemonitor/edgemonitor.db?mode=ro',uri=True)
now=time.time()*1000
for ts,t,d in c.execute('SELECT ts,temp,dist FROM sensor_history ORDER BY rowid DESC LIMIT 3'):
    print('%.1fs前 temp=%s dist=%s' % ((now-ts)/1000.0, t, d))"
```

**一定要看时间戳**。只看数值的话，26 小时前的旧数据和实时数据长得一模一样——
这个项目就因为这个误判过一次，基于"数据通了"的错误前提往下推，白查了一轮
STM32 的输入捕获。

## 五、回滚

```powershell
foreach ($p in 1883,8083,8080,8090) {
  netsh interface portproxy delete v4tov4 listenport=$p listenaddress=0.0.0.0
}
Remove-NetFirewallRule -DisplayName "EdgeMonitor 内网入站"
```

再把 VMware 网卡改回桥接、虚拟机地址改回原样即可。

## 六、面试怎么讲这一段

这是个**从现象倒推到二层协议**的例子，比"我配了个 MQTT"有料得多：

1. 现象：网关连上了、数据入库了，板子屏幕却是空的。
2. 缩小范围：板子 ping 不通虚拟机，但虚拟机 ping 得通板子——**单向可达**，
   这就排除了防火墙和应用层。
3. 看 ARP 表：两个 IP 同一个 MAC → 这不是 IP 层的问题，是二层。
4. 追到根因：WiFi station 只能有一个 MAC，桥接必须做 MAC 转换，映射靠虚拟机
   主动发包建立、30 秒老化；而业务方向正好相反。
5. 方案分层：先上保活止血（治标），再改 NAT 消除这个约束（治本）。

顺带能带出一个观点：**故障要能被如实上报，前提是它别伪装成别的样子**。
这次它伪装成了"MQTT 连不上"，同一个项目里超声波那次伪装成了"模块卡死"
（实际是浮空引脚被耦合），排查方向一开始就是错的。
