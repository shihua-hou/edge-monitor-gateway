#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QMap>
#include "appconfig.h"

class QLabel;
class QPushButton;
class QStackedWidget;
class NavIconButton;
class MqttClient;
class WeatherClient;
class VoiceAssistant;
class DashboardPage;
class ControlPage;
class ChatPage;
class SettingsPage;
class NotificationBanner;
class VirtualKeyboard;

/* MainWindow - EdgeMonitor 车载中控风格终端外壳。
 *
 * 四个页面（首页/设备控制/语音助手/系统设置）通过 QStackedWidget 切换，
 * 左侧竖排图标导航，切页面带一个淡入淡出过渡——纯粹是视觉打磨，功能上
 * 跟直接 setCurrentIndex 没区别。
 *
 * 这个类是"胶水层"：拥有 MqttClient/WeatherClient/VoiceAssistant 三个后台
 * 服务对象，把它们的信号接到对应页面的更新方法上，页面本身不知道这些
 * 服务怎么实现的——页面只管交互和展示。 */
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void onMqttConnected();
    void onMqttDisconnected();
    void onMqttMessage(const QString &topic, const QByteArray &payload);
    void switchPage(int index);
    void applySettings(const AppConfig &cfg);
    /* 只刷新语音密钥，不动 MQTT 连接。密钥是经 MQTT 下发的，
       在消息回调里走完整的 applySettings 会重建当前连接导致死循环 */
    void applyVoiceConfig();

protected:
    /* 滑块的首次定位必须等到窗口真的显示、布局算完之后。
       踩过的坑：main.cpp 里窗口是开机动画结束后（1.1s）才 showFullScreen 的，
       而构造函数里的 QTimer::singleShot(0) 早就跑完了——那时按钮的
       geometry 还是默认值，滑块会停在左上角。 */
    void showEvent(class QShowEvent *e) override;
    void resizeEvent(class QResizeEvent *e) override;
    /* 见 mainwindow.cpp：把"窗口比屏幕大"这种沉默故障变成一行日志 */
    void checkFitsScreen();

private:
    void buildUi();
    void connectServices();
    void publishCmd(int dev, int act, int p1, int p2);
    void raiseAlert(const QString &key, const QString &level, const QString &title, const QString &msg);
    /* 把导航选中滑块移到第 index 项。animate=false 用于首次定位 */
    void movePill(int index, bool animate);

    /* 导航选中滑块（弹簧动画）。放在按钮下层，只动 geometry。 */
    QWidget *m_navPill = nullptr;
    class QPropertyAnimation *m_navPillAnim = nullptr;

    QStackedWidget *m_stack;
    DashboardPage *m_dashboard;
    class VehiclePage *m_vehicle;
    class MapPage *m_map;
    class PathPage *m_path;
    ControlPage *m_control;
    ChatPage *m_chat;
    SettingsPage *m_settings;
    NotificationBanner *m_banner;
    VirtualKeyboard *m_keyboard;   // 触摸软键盘，见 virtualkeyboard.h 的说明

    // 告警阈值，跟 Web 端 v4 的逻辑一致；这里没有做成可配置项——中控屏是
    // 给现场操作者用的，阈值调整这种运营参数留在 Web 大屏那边统一管，
    // 避免两处各改一份互相不同步
    static constexpr double kTempHighAlert = 40.0;
    static constexpr double kDistLowAlert = 10.0;
    static constexpr double kHumiHighAlert = 90.0;
    static constexpr qint64 kAlertCooldownMs = 60000;
    /* 最近一次收到的传感器健康位（MQTT hf 字段）。缓存在这里是因为它随
       IMU 帧到达，而阈值判断发生在处理 status 帧的时候，两帧不同步 */
    int m_sensorHealth = 0;
    QMap<QString, qint64> m_alertCooldown;   // key -> 上次触发的 epoch ms，避免持续超阈值时刷屏

    QLabel *m_connDot;
    QLabel *m_connText;
    /* 导航项个数。**只在这里定一处**——原来 types[6]/names[6]/m_navBtns[6]
       三个 6 分散在两个文件里，加一页要改三处，漏一处就是数组越界，
       而越界写的是相邻成员，表现会是别的控件莫名其妙地坏掉。 */
    static const int kNavCount = 7;
    NavIconButton *m_navBtns[kNavCount];

    MqttClient *m_mqtt;
    WeatherClient *m_weather;
    VoiceAssistant *m_voice;
    AppConfig m_cfg;
};

#endif // MAINWINDOW_H
