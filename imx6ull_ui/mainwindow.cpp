#include "mainwindow.h"
#include "mqttclient.h"
#include "weatherclient.h"
#include "voiceassistant.h"
#include "dashboardpage.h"
#include "vehiclepage.h"
#include "mappage.h"
#include "pathpage.h"
#include "controlpage.h"
#include "chatpage.h"
#include "settingspage.h"
#include "notificationbanner.h"
#include "naviconbutton.h"
#include "iostheme.h"
#include <QPropertyAnimation>
#include <QTimer>
#include <QSizePolicy>
#include <QDebug>
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
    if (m_keyboard) m_keyboard->attachToApplication();
}

void MainWindow::buildUi()
{
    /* 全局底色走主题。字体不在这里写死：
       Ios::font*() 会先在本机字体里找鸿蒙/思源，找不到再退，
       而 QSS 里写死 'Microsoft YaHei' 在板子上根本不存在。 */
    setStyleSheet(QString("QMainWindow { background:%1; }").arg(Ios::bg().name()));

    QWidget *central = new QWidget(this);
    setCentralWidget(central);
    QHBoxLayout *root = new QHBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- 左侧竖排导航 ----
    /* iPadOS 式侧边栏：比原来的 84px 图标栏宽得多（188px），
       因为这是 1024px 横屏，横向富余而纵向只有 600px。
       底色比主背景略亮，右侧一条 1px 分隔线（不用阴影，
       软件渲染下阴影太贵）。 */
    QWidget *nav = new QWidget(central);
    nav->setFixedWidth(188);
    nav->setStyleSheet(QString("background:%1; border-right:1px solid %2;")
                       .arg(QColor(0x14,0x14,0x18).name())
                       .arg(QColor(0x2C,0x2C,0x2E).name()));
    QVBoxLayout *navLayout = new QVBoxLayout(nav);
    navLayout->setContentsMargins(14, 18, 14, 14);
    navLayout->setSpacing(4);

    /* 大标题：iOS 的层级感很大一部分来自"标题大得够夸张"。
       原来那个渐变圆角方块 logo 是安卓/网页的做法，iOS 侧边栏
       顶部用的是文字大标题。 */
    QLabel *brand = new QLabel("EdgeMonitor", nav);
    brand->setFont(Ios::fontTitle());
    brand->setStyleSheet(QString("color:%1;").arg(Ios::label().name()));
    navLayout->addWidget(brand);
    QLabel *sub = new QLabel("巡检车中控", nav);
    sub->setFont(Ios::fontFootnote());
    sub->setStyleSheet(QString("color:%1;").arg(Ios::gray().name()));
    navLayout->addWidget(sub);
    navLayout->addSpacing(16);

    QButtonGroup *group = new QButtonGroup(this);
    const NavIconButton::IconType types[kNavCount] = {
        NavIconButton::Home, NavIconButton::Vehicle, NavIconButton::Map,
        NavIconButton::Route, NavIconButton::Control, NavIconButton::Voice,
        NavIconButton::Settings
    };
    const char *names[kNavCount] = {"首页", "车辆", "地图", "路线", "控制", "助手", "设置"};
    for (int i = 0; i < kNavCount; i++) {
        /* 图标完全用 QPainter 矢量绘制（见 naviconbutton.cpp），不依赖字体
           字形——原来这里用 Unicode 符号（⌂⚙🎙☰）当文字画，🎙 是真彩色
           emoji，嵌入式系统的字体基本不带 emoji 字体集，显示成方块/空白 */
        NavIconButton *b = new NavIconButton(types[i], names[i], nav);
        group->addButton(b, i);
        navLayout->addWidget(b);
        m_navBtns[i] = b;
    }
    m_navBtns[0]->setChecked(true);

    /* 导航选中滑块。放在按钮下层（lower），只靠 geometry 动画移位。
       只重绘 188x46 这么大一块，在软件渲染的单核 A7 上完全跑得动——
       而整页滑动转场那种动画（1024x600x2B 逐帧）就不行了，
       这是这块硬件上"动画该动哪里"的分界线。 */
    m_navPill = new QWidget(nav);
    m_navPill->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_navPill->setStyleSheet(QString("background:%1; border-radius:10px;")
                             .arg(QColor(10, 132, 255, 64).name(QColor::HexArgb)));
    m_navPill->lower();
    m_navPillAnim = new QPropertyAnimation(m_navPill, "geometry", this);
    m_navPillAnim->setDuration(Ios::durNormal());
    m_navPillAnim->setEasingCurve(Ios::spring());
    // buttonClicked(int) 从 Qt 5.15 起标记为 deprecated（推荐用新的 idClicked），
    // 但嵌入式 SDK 常年停留在 5.12/5.9 这类 LTS 版本，idClicked 在那些版本上
    // 根本不存在，编译不过——这里用旧的重载换更广的兼容性，deprecation 警告
    // 无所谓，能在目标板子的 Qt 版本上编译过更重要
    connect(group, QOverload<int>::of(&QButtonGroup::buttonClicked), this, &MainWindow::switchPage);
    navLayout->addStretch();

    /* 连接状态放到同一行：圆点 + 文字左对齐，和上面的导航行对齐。
       原来圆点在上、文字在下居中，在 188px 宽的侧边栏里会显得很飘。 */
    {
        QWidget *statusRow = new QWidget(nav);
        QHBoxLayout *sl = new QHBoxLayout(statusRow);
        sl->setContentsMargins(12, 0, 12, 0);
        sl->setSpacing(8);
        m_connDot = new QLabel(statusRow);
        m_connDot->setFixedSize(8, 8);
        m_connDot->setStyleSheet(QString("background:%1; border-radius:4px;").arg(Ios::gray().name()));
        m_connText = new QLabel("离线", statusRow);
        m_connText->setFont(Ios::fontFootnote());
        m_connText->setStyleSheet(QString("color:%1;").arg(Ios::gray().name()));
        sl->addWidget(m_connDot);
        sl->addWidget(m_connText);
        sl->addStretch();
        navLayout->addWidget(statusRow);
    }
    root->addWidget(nav);

    // ---- 右侧页面栈 ----
    m_stack = new QStackedWidget(central);
    m_dashboard = new DashboardPage(m_stack);
    m_vehicle = new VehiclePage(m_stack);
    m_map = new MapPage(m_stack);
    m_path = new PathPage(m_stack);
    m_control = new ControlPage(m_stack);
    m_chat = new ChatPage(m_stack);
    m_settings = new SettingsPage(m_stack);
    /* 顺序必须和上面 names[] 的顺序一致——switchPage 直接拿按钮下标
       当页面下标用。两边不一致的话点"车辆"会跳到别的页。 */
    m_stack->addWidget(m_dashboard);
    m_stack->addWidget(m_vehicle);
    m_stack->addWidget(m_map);
    m_stack->addWidget(m_path);
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
    /* 页面栈的垂直策略设成 Ignored：**它不再向布局索取最小高度**。
     *
     * QStackedWidget 的 minimumSizeHint 取的是**所有页面里最大的那个**，
     * 不是当前页。实测：路线页 492、车辆页 376、首页 337……于是无论你在哪一页，
     * 栈都按 492 要高度。再加上软键盘自己的 204px，合计 696，
     * 而窗口能给的只有 516——差出来的部分 Qt 只能靠裁切收场，
     * 挨裁的是排在最后的键盘，现象就是"输入法只显示两行"。
     *
     * 这里的取舍很明确：**键盘的 204px 不能让**（裁掉的键按不到，
     * 功能直接没了），该让的是页面内容——手机上弹键盘时也是内容被压扁。
     * Ignored 让栈的最小高度变成 0，键盘先拿够，剩下的都给页面。
     *
     * 代价：键盘弹出时，路线页/车辆页这种内容高的页面会被压。
     * 但这两页都没有输入框，键盘不会在那里弹出，实际碰不到。 */
    m_stack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    contentLayout->addWidget(m_stack);

    /* 软键盘放在页面栈下面、作为布局的一员（不是浮层）：弹出时上面的内容
       被自然压扁，聊天页的输入框会跟着上移，设置页的滚动区会缩短，不会出现
       "正在输入的框被键盘盖住"的情况。默认隐藏，输入框获得焦点时自动弹出 */
    /* 自绘键盘 vs 系统输入法（Qt VirtualKeyboard）
     *
     * 自绘的这个只有英文和符号三层，**打不了中文**——而地图搜索恰恰要输
     * "加油站""南航"这种词，没有中文这一页就废了一半。
     *
     * 板子的 rootfs 里带了 Qt VirtualKeyboard（含拼音插件），试过了——
     * 它是 QtQuick 的独立窗口，在 600px 高的屏上几乎占满，正文全被挡住；
     * 语言列表一长串而这台设备只要中/英；而且选了简体中文候选词出不来。
     * 所以改成在自绘键盘上加中英切换和候选条（见 virtualkeyboard.cpp、
     * pinyinime.cpp），键盘还是那 5 行，只在打拼音时多出一条候选。
     *
     * 两个键盘不能同时在：都盯着 QLineEdit 的焦点事件，会一起弹出来。
     * 所以这里按环境变量二选一——系统输入法可用时，自绘的完全不创建，
     * 连那 204px 的布局高度也一并省掉。 */
    const bool useSystemIme =
        (qgetenv("QT_IM_MODULE") == QByteArray("qtvirtualkeyboard"));
    if (useSystemIme) {
        m_keyboard = nullptr;
        qWarning("[ime] 使用系统输入法 Qt VirtualKeyboard（含拼音）");
    } else {
        m_keyboard = new VirtualKeyboard(content);
        contentLayout->addWidget(m_keyboard);
        qWarning("[ime] 使用内置自绘键盘（英文/符号/拼音，词库 /home/root/pinyin.dict）");
    }

    root->addWidget(content, 1);

}

void MainWindow::showEvent(QShowEvent *e)
{
    QMainWindow::showEvent(e);
    /* 窗口显示了，但布局未必已经算完（尤其是 showFullScreen 会再改一次尺寸），
       所以再延后到下一轮事件循环。 */
    QTimer::singleShot(0, this, [this]() {
        movePill(m_stack->currentIndex(), false);
        checkFitsScreen();
    });
}

/* 窗口尺寸守卫。
 *
 * 踩过的坑：某个子控件的 minimumHeight 给大了，整窗的最小尺寸就超过了
 * 600px 的屏高。Qt **不会**把窗口缩到 minimumSizeHint 以下，于是
 * showFullScreen() 之后窗口反而比屏幕大，下半部分被裁在屏幕外——
 * 现象是"界面显示不全"，而日志里一个字都没有，只能靠肉眼发现。
 * 更隐蔽的是软键盘：它是布局成员而不是浮层，弹出时才会把总高推过界，
 * 所以问题只在"点了某个输入框之后"才出现。
 *
 * 这个检查不修复任何东西，只是把沉默的故障变成一行能搜到的日志。
 * 出现这行就去查是谁的 minimumHeight 太大——不是去调窗口尺寸。 */
void MainWindow::checkFitsScreen()
{
    const QSize need = minimumSizeHint();
    const QSize have = size();

    /* 高度预算表。这几个数加起来必须装进屏幕，否则 Qt 只能靠裁切收场。
       为什么值得常驻打印：这些最小高度分散在七八个文件里，
       改任何一个都可能把别人挤掉，而挤掉之后**没有任何报错**——
       上一轮就是软键盘被压到只剩两行，肉眼看半天也想不到是
       车辆页那个 260px 的仪表盘引起的（QStackedWidget 的最小尺寸
       取所有页面的最大值，不是当前页）。 */
    static bool logged = false;
    if (!logged) {
        logged = true;
        for (int i = 0; i < m_stack->count(); i++) {
            QWidget *w = m_stack->widget(i);
            qWarning("[layout]   page %d %s min=%d", i,
                     w->metaObject()->className(),
                     w->minimumSizeHint().height());
        }
        /* 注意：页面栈的垂直策略是 Ignored，**它的 minimumSizeHint 不参与
           布局计算**（上面逐页打印的那些数字只是给人看"谁最占地方"）。
           真正要装进窗口的是：max(导航, 键盘 + 内容实际能压到多少)。
           所以这里把栈的那个数标成"仅参考"，免得下次有人拿它去算账。 */
        qWarning("[layout] 高度预算：导航 %d，键盘 %d，窗口 %d"
                 "（页面栈 %d 仅参考，已设 Ignored 不占最小高度）",
                 m_navBtns[0]->parentWidget()->minimumSizeHint().height(),
                 m_keyboard ? m_keyboard->sizeHint().height() : 0,
                 have.height(),
                 m_stack->minimumSizeHint().height());
    }

    if (need.height() > have.height() || need.width() > have.width()) {
        qWarning("[layout] 窗口最小尺寸 %dx%d 超过当前窗口 %dx%d，"
                 "界面会被裁切——检查各页面的 setMinimumHeight",
                 need.width(), need.height(), have.width(), have.height());
    }
}

void MainWindow::resizeEvent(QResizeEvent *e)
{
    QMainWindow::resizeEvent(e);
    checkFitsScreen();
    /* 全屏切换/旋转后按钮位置会变，滑块得跟上，否则会错位。 */
    if (m_navPill && m_navPill->isVisible())
        movePill(m_stack->currentIndex(), false);
}

/* 把滑块移到第 index 个导航项。
   首次（窗口刚显示、布局还没算完）直接定位不动画，
   否则会看到它从 (0,0) "飞"到第一项。 */
void MainWindow::movePill(int index, bool animate)
{
    if (!m_navPill || index < 0 || index > 5) return;
    QRect target = m_navBtns[index]->geometry();
    if (!animate || !m_navPill->isVisible()) {
        m_navPill->setGeometry(target);
        m_navPill->show();
        m_navPill->lower();
        return;
    }
    m_navPillAnim->stop();
    m_navPillAnim->setStartValue(m_navPill->geometry());
    m_navPillAnim->setEndValue(target);
    m_navPillAnim->start();
}

void MainWindow::switchPage(int index)
{
    movePill(index, true);
    m_stack->setCurrentIndex(index);
    /* 每页的最小尺寸不一样，QStackedWidget 的 sizeHint 跟着当前页走——
       所以"窗口装不下"可能只在某一页出现。切页时查一次，
       日志里就能直接看出是哪一页撑的，而不是靠肉眼比对截图。 */
    checkFitsScreen();
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
    /* 路径回放的差速指令走同一条发布通道，不另开一份 MQTT */
    connect(m_path, &PathPage::cmdRequested, this, &MainWindow::publishCmd);
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
    m_connDot->setStyleSheet(QString("background:%1; border-radius:4px;").arg(Ios::green().name()));
    m_connText->setStyleSheet(QString("color:%1;").arg(Ios::green().name()));
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
    m_connDot->setStyleSheet(QString("background:%1; border-radius:4px;").arg(Ios::red().name()));
    m_connText->setStyleSheet(QString("color:%1;").arg(Ios::gray().name()));
    m_connText->setText("离线");
}

void MainWindow::onMqttMessage(const QString &topic, const QByteArray &payload)
{
    /* 语音密钥下发。
     *
     * ── 为什么"空 payload"不再表示清除 ──
     * 原来的约定是：Web 端点"清除"就发一条空的 retained 消息，既顶掉
     * broker 上的保留消息，也让设备清空本地配置——一条消息干两件事。
     * 问题是这两件事的生命周期不一样：**想把密钥从 broker 上撤下来，
     * 并不等于想把设备上的密钥也删掉**。
     * 而"发空 retained"恰恰是 MQTT 里删除保留消息的唯一办法，于是
     * 任何一次纯粹的 broker 清理都会顺手把设备的密钥抹掉。
     *
     * 现在拆开：
     *   空 payload      → 只是 broker 在撤保留消息，设备**不动**
     *   {"clear":1}     → 明确的清除指令，设备清空本地配置
     * 这样密钥就可以不再用 retained 下发（它本来也不需要——下面那行
     * m_cfg.save() 已经把它落到 QSettings 了，断电重启照样在）。 */
    if (topic.endsWith("/config/voice")) {
        QJsonObject probe = QJsonDocument::fromJson(payload).object();
        if (payload.trimmed().isEmpty()) {
            /* 保留消息被撤销。本地配置原样保留，只记一笔。 */
            return;
        }
        if (probe.value("clear").toInt() == 1) {
            m_cfg.xfAppId.clear(); m_cfg.xfApiKey.clear(); m_cfg.xfApiSecret.clear();
            m_cfg.save();
            m_settings->setConfig(m_cfg);
            applyVoiceConfig();   /* 不能用 applySettings：会重建当前这条 MQTT 连接 */
            m_banner->show("语音助手", "密钥已被远程清除", "warn");
            return;
        }
        const QJsonObject &c = probe;
        if (c.isEmpty()) return;

        /* 只在密钥真的变了的时候才做自检播报。
           改成非 retained 之后重复下发少了，但"同一份配置发两次"
           仍然可能（比如手滑点了两下下发），这个判断继续留着。 */
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
        m_vehicle->updateBattery(o.value("bat").toDouble());
        m_vehicle->updateHealth(m_sensorHealth);
        m_path->updateYaw(o.value("y").toDouble());
    } else if (topic.endsWith("/motor")) {
        /* 0x102：转速是**有符号**的（协议 v2.4），负值代表倒转。
           拿 toInt() 直接用就行，网关那边已经按 int16_t 解过了。 */
        m_vehicle->updateMotor(o.value("rl").toInt(), o.value("rr").toInt(),
                               o.value("ol").toInt(), o.value("or").toInt());
        /* 航迹推算：先有航向（imu 帧）再有位移（motor 帧）。
           两帧到达顺序不保证，但 200ms 周期下差一拍的误差
           远小于陀螺漂移本身，不值得为此做时间同步。 */
        /* dl/dr 只有新固件（协议 v2.6）才有；contains 判断比 toInt()==0
           可靠——位移本来就可能正好是 0。 */
        const bool haveDisp = o.contains("dl") && o.contains("dr");
        m_path->updateMotion(o.value("rl").toInt(), o.value("rr").toInt(),
                             o.value("ol").toInt(), o.value("or").toInt(),
                             o.value("dl").toInt(), o.value("dr").toInt(), haveDisp);
    } else if (topic.endsWith("/cmdecho")) {
        /* 网关从 CAN 上抓到的原始控制指令。路线录制录的就是它——
           手机控制页直接写 CAN，不经过 MQTT，只有在总线上才录得全。 */
        m_path->onCmdEcho((qint64)o.value("ts").toDouble(),
                          o.value("dev").toInt(), o.value("act").toInt(),
                          o.value("p1").toInt(), o.value("p2").toInt());
    } else if (topic.endsWith("/gps")) {
        m_vehicle->updateGps(o.value("fix").toInt(), o.value("fixText").toString(),
                             o.value("sat").toInt(), o.value("hdop").toDouble(),
                             o.value("speed").toDouble());
        /* 地图页收到的是**原始 WGS-84**，转 GCJ-02 在它内部做。
           在这里转的话，车辆页显示的坐标也会跟着变成火星坐标——
           而那一页显示的应该是 GNSS 的原值。 */
        m_map->updateVehicle(o.value("lat").toDouble(), o.value("lon").toDouble(),
                             o.value("fix").toInt());
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
