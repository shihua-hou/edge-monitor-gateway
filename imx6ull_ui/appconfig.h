#ifndef APPCONFIG_H
#define APPCONFIG_H

#include <QString>

/* AppConfig - 整个中控终端的持久化配置，一次性放一个结构体里，
 * 免得 MQTT/讯飞/天气三套配置分散在三个类里各自定义一遍 QSettings key。 */
struct AppConfig {
    /* 设备号。MQTT topic 是 monitor/<deviceId>/data/xxx，本机的 gateway_mqtt
     * 用的是环境变量 DEVICE_ID —— 这个界面要订阅的正是本机 gateway 发出来的
     * 数据，两者必须一致，所以这里也只认同一个环境变量，不做成 QSettings 里
     * 可改的项：分成两处配置，一旦填得不一样，界面就会安静地收不到任何数据，
     * 而且从现象上完全看不出是配置不一致导致的。
     * 要改设备号就改 /etc/edgemonitor.env，网关和界面一起生效。 */
    QString deviceId = "robot01";

    // MQTT
    QString mqttHost;
    int mqttPort = 1883;
    QString mqttUser;
    QString mqttPass;
    /* true = broker 地址来自环境变量（即 /etc/edgemonitor.env）。
       设置页据此把这几个框置灰，免得用户在界面上改了却不生效、
       或者改出一份和网关不一致的配置。 */
    bool mqttFromEnv = false;

    // 讯飞开放平台（语音听写/合成 + 星火大模型）
    QString xfAppId;
    QString xfApiKey;
    QString xfApiSecret;
    QString xfSparkHost = "spark-api.xf-yun.com";
    QString xfSparkPath = "/v2/chat/completions";   // 星火 X2/V2，跟实测验证过的版本一致
    QString xfSparkDomain = "generalv2";

    // 和风天气
    QString qwHost;      // 形如 xxxxx.re.qweatherapi.com，登录和风天气控制台"我的项目"里看
    QString qwKey;
    /* "auto" = 开机用 IP 定位查一次经纬度。这台是会被搬走的巡检终端，
       写死城市 ID 在它换了地方之后就是错的，而且错得很安静。
       要钉死某个城市就填 LocationID（和风文档里查）或 "经度,纬度"。 */
    QString qwLocation = "auto";

    static AppConfig load();
    void save() const;
};

#endif // APPCONFIG_H
