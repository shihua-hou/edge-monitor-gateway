#include "mainwindow.h"
#include "mqttclient.h"
#include "weatherclient.h"
#include "voiceassistant.h"
#include "dashboardpage.h"
#include "controlpage.h"
#include "chatpage.h"
#include "settingspage.h"
#include "notificationbanner.h"
#include "naviconbutton.h"
#include "virtualkeyboard.h"

#include <cstdio>

#include <QLabel>
#include <QButtonGroup>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QDateTime>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), m_mqtt(nullptr), m_weather(nullptr), m_voice(nullptr)
{
    setWindowTitle("EdgeMonitor 车载中控终端");
    buildUi();
    connectServices();

    m_cfg = AppConfig::load();
    m_settings->setConfig(m_cfg);
    applySettings(m_cfg);

    /* 软键盘的事件过滤器等界面搭完、配置也灌完之后再装：setConfig() 会往
       设置页的一堆输入框里填值，那个过程中的焦点变化不该弹出键盘 */
    m_keyboard->attachToApplication();
}

void MainWindow::buildUi()
{
    setStyleSheet(
        "QMainWindow { background:#0a0e15; }"
        "QWidget { font-family:'Microsoft YaHei', sans-serif; }"
    );

    QWidget *central = new QWidget(this);
    setCentralWidget(central);
    QHBoxLayout *root = new QHBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- 左侧竖排导航 ----
    QWidget *nav = new QWidget(central);
    nav->setFixedWidth(84);
    nav->setStyleSheet("background:#0c111a; border-right:1px solid #1c2536;");
    QVBoxLayout *navLayout = new QVBoxLayout(nav);
    navLayout->setContentsMargins(8, 16, 8, 16);
    navLayout->setSpacing(10);

    QLabel *brand = new QLabel("EM", nav);
    brand->setAlignment(Qt::AlignCenter);
    brand->setFixedSize(48, 48);
    brand->setStyleSheet("background:qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 #3ddc97, stop:1 #5aa9ff);"
                          "border-radius:14px; font-weight:800; color:#04140d; font-size:16px;");
    navLayout->addWidget(brand, 0, Qt::AlignHCenter);
    navLayout->addSpacing(10);

    QButtonGroup *group = new QButtonGroup(this);
    const NavIconButton::IconType types[4] = {
        NavIconButton::Home, NavIconButton::Control, NavIconButton::Voice, NavIconButton::Settings
    };
    const char *names[4] = {"首页", "控制", "助手", "设置"};
    for (int i = 0; i < 4; i++) {
        /* 图标完全用 QPainter 矢量绘制（见 naviconbutton.cpp），不依赖字体
           字形——原来这里用 Unicode 符号（⌂⚙🎙☰）当文字画，🎙 是真彩色
           emoji，嵌入式系统的字体基本不带 emoji 字体集，显示成方块/空白 */
        NavIconButton *b = new NavIconButton(types[i], names[i], nav);
        group->addButton(b, i);
        navLayout->addWidget(b);
        m_navBtns[i] = b;
    }
    m_navBtns[0]->setChecked(true);
    // buttonClicked(int) 从 Qt 5.15 起标记为 deprecated（推荐用新的 idClicked），
    // 但嵌入式 SDK 常年停留在 5.12/5.9 这类 LTS 版本，idClicked 在那些版本上
    // 根本不存在，编译不过——这里用旧的重载换更广的兼容性，deprecation 警告
    // 无所谓，能在目标板子的 Qt 版本上编译过更重要
    connect(group, QOverload<int>::of(&QButtonGroup::buttonClicked), this, &MainWindow::switchPage);
    navLayout->addStretch();

    m_connDot = new QLabel(nav);
    m_connDot->setFixedSize(10, 10);
    m_connDot->setStyleSheet("background:#5a6577; border-radius:5px;");
    m_connText = new QLabel("离线", nav);
    m_connText->setStyleSheet("color:#5a6577; font-size:10px;");
    m_connText->setAlignment(Qt::AlignCenter);
    navLayout->addWidget(m_connDot, 0, Qt::AlignHCenter);
    navLayout->addWidget(m_connText);
    root->addWidget(nav);

    // ---- 右侧页面栈 ----
    m_stack = new QStackedWidget(central);
    m_dashboard = new DashboardPage(m_stack);
    m_control = new ControlPage(m_stack);
    m_chat = new ChatPage(m_stack);
    m_settings = new SettingsPage(m_stack);
    m_stack->addWidget(m_dashboard);
    m_stack->addWidget(m_control);
    m_stack->addWidget(m_chat);
    m_stack->addWidget(m_settings);

    QWidget *content = new QWidget(central);
    QVBoxLayout *contentLayout = new QVBoxLayout(content);
    /* 上下边距压到 8：屏幕只有 600px 高，这里每省一像素，下面的页面就多
       一像素可用。左右不动，横向 1024px 很宽裕 */
    contentLayout->setContentsMargins(18, 8, 18, 8);
    contentLayout->setSpacing(8);
    m_banner = new NotificationBanner(content);
    contentLayout->addWidget(m_banner);
    contentLayout->addWidget(m_stack);

    /* 软键盘放在页面栈下面、作为布局的一员（不是浮层）：弹出时上面的内容
       被自然压扁，聊天页的输入框会跟着上移，设置页的滚动区会缩短，不会出现
       "正在输入的框被键盘盖住"的情况。默认隐藏，输入框获得焦点时自动弹出 */
    m_keyboard = new VirtualKeyboard(content);
    contentLayout->addWidget(m_keyboard);

    root->addWidget(content, 1);
}

void MainWindow::switchPage(int index)
{
    m_stack->setCurrentIndex(index);
    QWidget *w = m_stack->currentWidget();
    QGraphicsOpacityEffect *effect = new QGraphicsOpacityEffect(w);
    w->setGraphicsEffect(effect);
    QPropertyAnimation *anim = new QPropertyAnimation(effect, "opacity", w);
    anim->setDuration(200);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    connect(anim, &QPropertyAnimation::finished, this, [w]() { w->setGraphicsEffect(nullptr); });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void MainWindow::connectServices()
{
    m_weather = new WeatherClient(this);
    connect(m_weather, &WeatherClient::weatherUpdated, this,
            [this](const QString &t, const QString &text, const QString &, const QString &city) {
        m_dashboard->updateWeather(t, text, city);
    });
    connect(m_weather, &WeatherClient::weatherError, this, [this](const QString &msg) {
        m_dashboard->setWeatherError(msg);
    });
    QTimer *weatherTimer = new QTimer(this);
    connect(weatherTimer, &QTimer::timeout, m_weather, &WeatherClient::refresh);
    weatherTimer->start(30 * 60 * 1000);   // 半小时刷新一次，天气数据没必要更频繁

    m_voice = new VoiceAssistant(this);
    connect(m_voice, &VoiceAssistant::partialTranscript, m_chat, &ChatPage::updatePartialTranscript);
    connect(m_voice, &VoiceAssistant::finalTranscript, this, [this](const QString &text) {
        if (text.trimmed().isEmpty()) { m_chat->setListening(false); return; }
        m_chat->appendUserBubble(text);
        m_chat->setThinking(true);
        m_voice->sendChat(text);
    });
    connect(m_voice, &VoiceAssistant::chatFinished, this, [this](const QString &fullText) {
        m_chat->appendAssistantBubble(fullText);
        m_chat->setListening(false);
        if (m_chat->ttsEnabled() && !fullText.isEmpty()) m_voice->speak(fullText);
    });
    connect(m_voice, &VoiceAssistant::assistantError, this, [this](const QString &msg) {
        /* 必须同时打到 stderr。横幅几秒就消失、聊天气泡要切到语音助手页才看得见，
           而现场排查基本都是 ssh 进来 tail 日志——之前"没声音也不知道为什么"
           就是因为错误原话只存在于屏幕上，日志里一个字都没有 */
        fprintf(stderr, "[voice][error] %s\n", msg.toUtf8().constData());
        m_chat->appendAssistantBubble("[错误] " + msg);
        m_chat->setListening(false);
        m_banner->show("语音助手", msg, "warn");
    });

    connect(m_chat, &ChatPage::micPressed, this, [this]() {
        m_chat->setListening(true);
        m_voice->startListening();
    });
    connect(m_chat, &ChatPage::micReleased, this, [this]() { m_voice->stopListening(); });
    connect(m_chat, &ChatPage::sendTextRequested, this, [this](const QString &text) {
        m_chat->setThinking(true);
        m_voice->sendChat(text);
    });

    connect(m_control, &ControlPage::cmdRequested, this, &MainWindow::publishCmd);
    connect(m_settings, &SettingsPage::saved, this, &MainWindow::applySettings);
}

/* 只把语音密钥推给 VoiceAssistant，不碰 MQTT 连接。
   单独拆出来是必须的：密钥是通过 MQTT 消息下发的，如果在消息回调里调用
   完整的 applySettings()，它会 deleteLater 掉当前这条连接再重建——而那正是
   回调的信号源。重连之后又会收到同一条 retained 配置，于是无限重连。 */
void MainWindow::applyVoiceConfig()
{
    VoiceAssistant::Config vc;
    vc.appId = m_cfg.xfAppId; vc.apiKey = m_cfg.xfApiKey; vc.apiSecret = m_cfg.xfApiSecret;
    vc.sparkHost = m_cfg.xfSparkHost; vc.sparkPath = m_cfg.xfSparkPath; vc.sparkDomain = m_cfg.xfSparkDomain;
    m_voice->configure(vc);
}

void MainWindow::applySettings(const AppConfig &cfg)
{
    /* deviceId 不在设置页里，cfg 里带的是结构体默认值。直接整体赋值会把
       启动时从环境变量读到的设备号冲掉，之后订阅的就是 robot01 的 topic，
       本机设备号如果不是 robot01，界面会安静地收不到任何数据。
       所以保留原值，只接受设置页真正管理的那些字段。 */
    const QString keepDeviceId = m_cfg.deviceId;
    m_cfg = cfg;
    m_cfg.deviceId = keepDeviceId;

    applyVoiceConfig();

    m_weather->configure(cfg.qwHost, cfg.qwKey, cfg.qwLocation);
    m_weather->refresh();

    if (m_mqtt) { m_mqtt->deleteLater(); m_mqtt = nullptr; }
    if (!cfg.mqttHost.isEmpty()) {
        m_mqtt = new MqttClient("edgemonitor_ui", this);
        connect(m_mqtt, &MqttClient::connected, this, &MainWindow::onMqttConnected);
        connect(m_mqtt, &MqttClient::disconnected, this, &MainWindow::onMqttDisconnected);
        connect(m_mqtt, &MqttClient::messageReceived, this, &MainWindow::onMqttMessage);
        connect(m_mqtt, &MqttClient::connectionFailed, this, [this](const QString &reason) {
            m_banner->show("MQTT 连接失败", reason, "warn");
        });
        m_mqtt->start(cfg.mqttHost, cfg.mqttPort, cfg.mqttUser, cfg.mqttPass);
    }
}

void MainWindow::onMqttConnected()
{
    m_connDot->setStyleSheet("background:#3ddc97; border-radius:5px;");
    m_connText->setText("在线");
    /* v3：topic 带设备号，只订阅本机这台的数据。一个 broker 上可能接了
       多台机器人，订阅 monitor/data/# 那种全局 topic 会把别的车的数据
       也画到本机屏幕上 */
    m_mqtt->subscribe(QString("monitor/%1/data/#").arg(m_cfg.deviceId));
    /* 语音密钥由 Web 端下发（retained），这样不用在触摸屏上戳几十位的
       密钥，也不用把密钥写进源码。retained 意味着无论设备什么时候上线
       都能收到最后一次下发的配置 */
    m_mqtt->subscribe(QString("monitor/%1/config/#").arg(m_cfg.deviceId));
}
void MainWindow::onMqttDisconnected()
{
    m_connDot->setStyleSheet("background:#ff5470; border-radius:5px;");
    m_connText->setText("离线");
}

void MainWindow::onMqttMessage(const QString &topic, const QByteArray &payload)
{
    /* 语音密钥下发。空 payload 表示 Web 端点了"清除"——把保留消息顶掉，
       这里也要跟着清空本地配置，否则设备上还留着旧密钥 */
    if (topic.endsWith("/config/voice")) {
        if (payload.trimmed().isEmpty()) {
            m_cfg.xfAppId.clear(); m_cfg.xfApiKey.clear(); m_cfg.xfApiSecret.clear();
            m_cfg.save();
            m_settings->setConfig(m_cfg);
            applyVoiceConfig();   /* 不能用 applySettings：会重建当前这条 MQTT 连接 */
            m_banner->show("语音助手", "密钥已被远程清除", "warn");
            return;
        }
        QJsonObject c = QJsonDocument::fromJson(payload).object();
        if (c.isEmpty()) return;

        /* 只在密钥真的变了的时候才做自检播报。这是 retained 消息，
           设备每次重连都会重新收到同一份配置，不判断的话每次开机、
           每次断网重连都要念一遍，很吵 */
        const bool changed = (m_cfg.xfAppId    != c.value("appId").toString() ||
                              m_cfg.xfApiKey   != c.value("apiKey").toString() ||
                              m_cfg.xfApiSecret!= c.value("apiSecret").toString());

        m_cfg.xfAppId       = c.value("appId").toString();
        m_cfg.xfApiKey      = c.value("apiKey").toString();
        m_cfg.xfApiSecret   = c.value("apiSecret").toString();
        if (!c.value("sparkHost").toString().isEmpty())   m_cfg.xfSparkHost   = c.value("sparkHost").toString();
        if (!c.value("sparkPath").toString().isEmpty())   m_cfg.xfSparkPath   = c.value("sparkPath").toString();
        if (!c.value("sparkDomain").toString().isEmpty()) m_cfg.xfSparkDomain = c.value("sparkDomain").toString();
        m_cfg.save();                 /* 落到 QSettings，断网也能用 */
        m_settings->setConfig(m_cfg); /* 设置页的输入框同步显示 */
        applyVoiceConfig();           /* 只更新语音，不重建 MQTT（见函数注释） */

        if (changed && m_voice->isConfigured()) {
            /* 自检：收到新密钥就实际调一次 TTS。
               "配置下发成功"只能证明消息送到了，证明不了密钥是对的——
               鉴权对不对只有真去连一次讯飞才知道。听到这句话就说明
               密钥有效且 TTS 整条链路（鉴权→合成→aplay 播放）都通了；
               密钥错的话会走 assistantError，横幅上会显示服务端的原话。 */
            m_banner->show("语音助手", "已收到密钥，正在测试讯飞连接…", "info");
            m_voice->speak("语音助手配置成功");
        } else {
            m_banner->show("语音助手", "已收到远程下发的密钥", "info");
        }
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject()) return;
    QJsonObject o = doc.object();

    if (topic.endsWith("/status")) {
        double temp = o.value("t").toDouble();
        double humi = o.value("h").toDouble();
        int dist = o.value("d").toInt();
        int s = o.value("s").toInt();
        m_dashboard->updateStatus(temp, humi, o.value("l").toInt(), dist, s);
        m_control->setLedState((s & 1) != 0);
        m_control->setBuzzerState((s & 2) != 0);
        /* 兑现等待中的命令回执。放在 setXxxState 之后：那两个只是把按钮文字
           改成设备的真实状态，而这一句判断的是"我刚下发的那条命令生效了没"，
           两回事 */
        m_control->onStateBits(s);

        /* 已失联的传感器不参与阈值告警。陈旧值是"最后一次有效读数"，拿它
           判阈值毫无意义：传感器恰好在 41℃ 时坏掉，之后会永远重复报
           "温度过高"，而真实温度早就降下来了。一条永不消失的假告警比没有
           告警更糟——它会让人对整个告警系统失去信任。 */
        const bool staleTh   = (m_sensorHealth & 0x01) != 0;
        const bool staleDist = (m_sensorHealth & 0x02) != 0;

        if (!staleTh && temp > kTempHighAlert)
            raiseAlert("temp", "danger", "温度过高", QString("当前 %1℃，超过阈值 %2℃").arg(temp, 0, 'f', 1).arg(kTempHighAlert, 0, 'f', 0));
        if (!staleDist && dist < kDistLowAlert)
            raiseAlert("dist", "warn", "碰撞预警", QString("前方距离仅 %1cm，低于阈值 %2cm").arg(dist).arg(int(kDistLowAlert)));
        if (!staleTh && humi > kHumiHighAlert)
            raiseAlert("humi", "warn", "湿度异常", QString("当前湿度 %1%，超过阈值 %2%").arg(humi, 0, 'f', 1).arg(kHumiHighAlert, 0, 'f', 0));
    } else if (topic.endsWith("/imu")) {
        /* hf 是传感器健康位，搭 IMU 帧的顺风车上来（那一帧还有空字节），
           描述的是全部四路传感器，不只是姿态。见 gateway_mqtt.c 的说明 */
        m_sensorHealth = o.value("hf").toInt();
        m_dashboard->updateSensorHealth(m_sensorHealth);
        m_dashboard->updateImu(o.value("p").toDouble(), o.value("r").toDouble(), o.value("y").toDouble());
    }
}

void MainWindow::raiseAlert(const QString &key, const QString &level, const QString &title, const QString &msg)
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 last = m_alertCooldown.value(key, 0);
    if (now - last < kAlertCooldownMs) return;   // 持续超阈值时不要每帧都弹一条，扰乱操作者
    m_alertCooldown[key] = now;

    m_banner->show(title, msg, level);
    // 危险级别的顺手用语音播一下——巡检现场操作者可能没盯着屏幕看，
    // 声音提示比横幅更不容易被错过。用 danger 级别限定，不然每条 warn
    // 都念一遍会很吵
    if (level == "danger" && m_voice && m_voice->isConfigured()) {
        m_voice->speak(title + "，" + msg);
    }
}

void MainWindow::publishCmd(int dev, int act, int p1, int p2)
{
    if (!m_mqtt) return;
    QJsonObject o;
    o["d"] = dev; o["a"] = act; o["p1"] = p1; o["p2"] = p2;
    /* 命令发到本机设备的 topic。老协议是全局 monitor/cmd，多台机器人在线时
       这块屏幕上按一次"前进"，会让所有车一起动 */
    m_mqtt->publish(QString("monitor/%1/cmd").arg(m_cfg.deviceId),
                    QJsonDocument(o).toJson(QJsonDocument::Compact));
}
