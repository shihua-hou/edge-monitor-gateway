#include "appconfig.h"
#include <QSettings>
#include <QByteArray>

AppConfig AppConfig::load()
{
    QSettings s("EdgeMonitor", "edgemonitor_ui");
    AppConfig c;

    /* 设备号只从环境变量取，跟本机 gateway_mqtt 同源（见 appconfig.h 的说明）。
       守护脚本 edgemonitor-daemon.sh 会 source /etc/edgemonitor.env 并 export，
       所以由它拉起的界面能读到；手工启动时没有这个变量就退回默认 robot01 */
    QByteArray env = qgetenv("DEVICE_ID");
    if (!env.isEmpty()) c.deviceId = QString::fromUtf8(env).trimmed();

    /* MQTT 连接参数：环境变量优先，QSettings 只兜底。

       理由和上面 deviceId 那段一模一样，而且这条是被真事故逼出来的：
       broker 的地址原来存在三个地方——/etc/edgemonitor.env（网关和 OTA 用）、
       这里的 QSettings（界面用）、Web 大屏的 localStorage。虚拟机的 IP 变了
       之后只改了第一处，结果是"网关连上了、数据也入库了，可是板子屏幕上
       什么都不显示"——界面还抱着 QSettings 里的旧地址，而这个现象从表面上
       完全看不出是配置不一致。

       为什么是"环境变量优先"而不是"QSettings 优先"：
       QSettings 是设置页写进去的，一旦写过就会永久遮蔽环境变量，那就等于
       又回到了两处配置。反过来，环境变量由守护脚本每轮重读 /etc/edgemonitor.env
       后 export，改一处就能全体生效。QSettings 保留兜底，是为了手工启动
       （不经过守护脚本、没有这些环境变量）时还能从界面上配。 */
    const QString envHost = QString::fromUtf8(qgetenv("MQTT_HOST")).trimmed();
    /* 界面**优先用自己的那组账号**（MQTT_UI_USER/PASS），取不到才退回
       全局的 MQTT_USER/PASS。
       理由是权限方向不同：网关那几个进程只往上发遥测、只接收指令，
       而界面正好相反——只读遥测、要能发指令。broker 的 ACL 按这个
       方向分了账号（见 deploy/board/acl-board），共用一个账号的话，
       那个账号就同时具备"发数据"和"发指令"两种能力，ACL 等于白分。

       为什么不用 `env MQTT_USER=ui ...` 在启动命令里覆盖：那样密码会出现在
       ps 的输出里，任何能跑 ps 的人都看得到——修一个安全问题时顺手
       引入另一个，不划算。 */
    QString envUser = QString::fromUtf8(qgetenv("MQTT_UI_USER")).trimmed();
    QString envPass = QString::fromUtf8(qgetenv("MQTT_UI_PASS"));
    if (envUser.isEmpty()) {
        envUser = QString::fromUtf8(qgetenv("MQTT_USER")).trimmed();
        envPass = QString::fromUtf8(qgetenv("MQTT_PASS"));
    }
    c.mqttFromEnv = !envHost.isEmpty();

    c.mqttHost = c.mqttFromEnv ? envHost : s.value("mqtt/host").toString();
    c.mqttPort = s.value("mqtt/port", 1883).toInt();   /* 端口没进 env，沿用 QSettings */
    c.mqttUser = !envUser.isEmpty() ? envUser : s.value("mqtt/user").toString();
    c.mqttPass = !envPass.isEmpty() ? envPass : s.value("mqtt/pass").toString();

    c.xfAppId = s.value("xfyun/appId").toString();
    c.xfApiKey = s.value("xfyun/apiKey").toString();
    c.xfApiSecret = s.value("xfyun/apiSecret").toString();
    c.xfSparkHost = s.value("xfyun/sparkHost", c.xfSparkHost).toString();
    c.xfSparkPath = s.value("xfyun/sparkPath", c.xfSparkPath).toString();
    c.xfSparkDomain = s.value("xfyun/sparkDomain", c.xfSparkDomain).toString();

    c.qwHost = s.value("qweather/host").toString();
    c.qwKey = s.value("qweather/key").toString();
    c.qwLocation = s.value("qweather/location", c.qwLocation).toString();
    return c;
}

void AppConfig::save() const
{
    QSettings s("EdgeMonitor", "edgemonitor_ui");
    /* 由环境变量提供时不落盘：写进去只会变成一份将来可能过期的影子配置，
       等哪天环境变量没了，它就会以一个谁也想不起来的旧值悄悄生效。
       端口不在 env 里，始终保存。 */
    if (!mqttFromEnv) {
        s.setValue("mqtt/host", mqttHost);
        s.setValue("mqtt/user", mqttUser);
        s.setValue("mqtt/pass", mqttPass);
    }
    s.setValue("mqtt/port", mqttPort);

    s.setValue("xfyun/appId", xfAppId);
    s.setValue("xfyun/apiKey", xfApiKey);
    s.setValue("xfyun/apiSecret", xfApiSecret);
    s.setValue("xfyun/sparkHost", xfSparkHost);
    s.setValue("xfyun/sparkPath", xfSparkPath);
    s.setValue("xfyun/sparkDomain", xfSparkDomain);

    s.setValue("qweather/host", qwHost);
    s.setValue("qweather/key", qwKey);
    s.setValue("qweather/location", qwLocation);
}
