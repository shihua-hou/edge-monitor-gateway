#ifndef SETTINGSPAGE_H
#define SETTINGSPAGE_H

#include <QWidget>
#include "appconfig.h"

class QLineEdit;
class QVBoxLayout;
class QLabel;

/* SettingsPage - 系统设置页：MQTT 连接 / 讯飞开放平台 / 和风天气三组配置，
 * 全部走本机 QSettings 持久化。跟 Web 端每次登录都要重新输入不一样——这台
 * 终端固定装在设备上，是本机操作台，信任边界是"谁能碰到这台设备"，不是
 * "谁知道账号密码"，所以配置好一次开机自动生效，不用每次都重新填。 */
class SettingsPage : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsPage(QWidget *parent = nullptr);

    void setConfig(const AppConfig &cfg);
    AppConfig config() const;

signals:
    void saved(const AppConfig &cfg);

private:
    QLineEdit *addField(QVBoxLayout *form, const QString &label, bool password = false);

    QLineEdit *m_mqttHost, *m_mqttPort, *m_mqttUser, *m_mqttPass;
    /* broker 地址由 /etc/edgemonitor.env 提供时，上面几个框置灰并显示这行说明。
       这个标志必须在 setConfig/config 之间原样带回去，否则 config() 造出来的
       AppConfig 里 mqttFromEnv 是 false，保存时又会把 env 的值落成影子配置。 */
    QLabel *m_mqttEnvHint;
    bool m_mqttFromEnv;
    QLineEdit *m_xfAppId, *m_xfApiKey, *m_xfApiSecret, *m_xfSparkHost, *m_xfSparkPath, *m_xfSparkDomain;
    QLineEdit *m_qwHost, *m_qwKey, *m_qwLocation;
};

#endif // SETTINGSPAGE_H
