#include "controlpage.h"
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QTimer>
#include <QDateTime>

namespace Dev { enum { LED = 1, BUZZER = 2, MOTOR = 3 }; }
namespace Act { enum { ON = 1, OFF = 2, BACK = 3, LEFT = 4, RIGHT = 5, SPEED = 6 }; }

ControlPage::ControlPage(QWidget *parent) : QWidget(parent)
{
    setStyleSheet(
        "#panel { background:#141b28; border:1px solid #232f42; border-radius:12px; padding:14px; }"
        "QPushButton { background:#1e2a3f; color:#e6e6e6; border:1px solid #33455f;"
        "  border-radius:9px; font-size:14px; }"
        "QPushButton:pressed { background:#2a3a55; }"
    );

    QHBoxLayout *root = new QHBoxLayout(this);
    root->setSpacing(14);

    // ---- 执行设备 ----
    QFrame *devPanel = new QFrame(this);
    devPanel->setObjectName("panel");
    QVBoxLayout *devLayout = new QVBoxLayout(devPanel);
    QLabel *devTitle = new QLabel("执行设备", devPanel);
    devTitle->setStyleSheet("color:#8a94a6; font-size:13px;");
    devLayout->addWidget(devTitle);

    /* 不用 emoji 前缀（💡🔔 这类真彩色 emoji 在嵌入式系统字体下大概率显示
       成方块），文字本身已经说清楚是什么按钮，靠开关态的背景色区分够用 */
    m_ledBtn = new QPushButton("LED：关", devPanel);
    m_buzzerBtn = new QPushButton("蜂鸣器：关", devPanel);
    for (QPushButton *b : { m_ledBtn, m_buzzerBtn }) b->setMinimumHeight(64);
    connect(m_ledBtn, &QPushButton::clicked, this, &ControlPage::toggleLed);
    connect(m_buzzerBtn, &QPushButton::clicked, this, &ControlPage::toggleBuzzer);
    devLayout->addWidget(m_ledBtn);
    devLayout->addWidget(m_buzzerBtn);
    devLayout->addStretch();
    root->addWidget(devPanel, 1);

    // ---- 小车控制 ----
    QFrame *carPanel = new QFrame(this);
    carPanel->setObjectName("panel");
    QVBoxLayout *carLayout = new QVBoxLayout(carPanel);
    QLabel *carTitle = new QLabel("小车运动控制", carPanel);
    carTitle->setStyleSheet("color:#8a94a6; font-size:13px;");
    carLayout->addWidget(carTitle);

    QHBoxLayout *speedRow = new QHBoxLayout;
    m_speedSlider = new QSlider(Qt::Horizontal, carPanel);
    m_speedSlider->setRange(0, 100);
    m_speedSlider->setValue(60);
    m_speedVal = new QLabel("60%", carPanel);
    m_speedVal->setStyleSheet("color:#3ddc97; font-size:14px; min-width:44px;");
    connect(m_speedSlider, &QSlider::valueChanged, this, [this](int v) {
        m_speedVal->setText(QString::number(v) + "%");
    });
    speedRow->addWidget(m_speedSlider);
    speedRow->addWidget(m_speedVal);
    carLayout->addLayout(speedRow);

    QGridLayout *dpad = new QGridLayout;
    dpad->setSpacing(8);
    QPushButton *fwdBtn = new QPushButton("▲\n前进", carPanel);
    QPushButton *leftBtn = new QPushButton("◀\n左转", carPanel);
    QPushButton *stopBtn = new QPushButton("■\n停止", carPanel);
    QPushButton *rightBtn = new QPushButton("▶\n右转", carPanel);
    QPushButton *backBtn = new QPushButton("▼\n后退", carPanel);
    stopBtn->setStyleSheet("background:#3a1420; border-color:#d04a5a; color:#ff5470; font-weight:700;");
    for (QPushButton *b : { fwdBtn, leftBtn, stopBtn, rightBtn, backBtn }) b->setMinimumHeight(66);
    dpad->addWidget(fwdBtn, 0, 1);
    dpad->addWidget(leftBtn, 1, 0);
    dpad->addWidget(stopBtn, 1, 1);
    dpad->addWidget(rightBtn, 1, 2);
    dpad->addWidget(backBtn, 2, 1);
    connect(fwdBtn, &QPushButton::clicked, this, [this] { carCmd(Act::ON); });
    connect(backBtn, &QPushButton::clicked, this, [this] { carCmd(Act::BACK); });
    connect(leftBtn, &QPushButton::clicked, this, [this] { carCmd(Act::LEFT); });
    connect(rightBtn, &QPushButton::clicked, this, [this] { carCmd(Act::RIGHT); });
    connect(stopBtn, &QPushButton::clicked, this, [this] { carCmd(Act::OFF); });
    carLayout->addLayout(dpad);
    carLayout->addStretch();
    root->addWidget(carPanel, 1);

    /* 命令回执。放在执行设备那一栏的底部，占一行——屏幕只有 600px 高，
       这里不做 Web 那种完整列表，只显示最近一条：现场操作者关心的是
       "我刚按的那下生效了没有"，不是历史记录 */
    m_ackLabel = new QLabel("", devPanel);
    m_ackLabel->setWordWrap(true);
    m_ackLabel->setStyleSheet("color:#5a6577; font-size:12px;");
    devLayout->addWidget(m_ackLabel);

    /* 超时必须由独立定时器判定，不能只在"收到下一帧状态"时顺带检查：
       设备彻底离线时压根不会再有帧过来，而那正是这个功能最该抓住的场景。
       靠帧驱动的话命令会永远停在"等待回执"，看着像还在路上，其实石沉大海。 */
    m_ackTimer = new QTimer(this);
    m_ackTimer->setSingleShot(true);
    connect(m_ackTimer, &QTimer::timeout, this, [this]() {
        if (m_pendBit == 0) return;
        m_pendBit = 0;
        showAck(m_pendLabel + " 未生效（3 秒内没等到设备回传状态，"
                              "检查设备是否在线、CAN 链路是否正常）", "#ff5470");
    });
}

void ControlPage::showAck(const QString &text, const char *color)
{
    if (!m_ackLabel) return;
    m_ackLabel->setText(text);
    m_ackLabel->setStyleSheet(QString("color:%1; font-size:12px;").arg(color));
}

void ControlPage::noteCommand(const QString &label, int bit, bool want)
{
    m_pendLabel = label;
    m_pendBit   = bit;
    m_pendWant  = want;
    m_pendAt    = QDateTime::currentMSecsSinceEpoch();
    if (bit == 0) {
        /* 这个动作在状态字里没有对应位（转向、调速），只能确认"发出去了"。
           假装确认比不确认更糟——用户需要知道哪些是真确认过的 */
        showAck(label + " 已发送（该动作无状态回读）", "#8a94a6");
        m_ackTimer->stop();
        return;
    }
    showAck(label + " 已下发，等待设备回执…", "#8a94a6");
    m_ackTimer->start(3000);
}

void ControlPage::onStateBits(int stateBits)
{
    if (m_pendBit == 0) return;
    if (((stateBits & m_pendBit) != 0) != m_pendWant) return;   /* 还没变成期望的状态 */

    qint64 ms = QDateTime::currentMSecsSinceEpoch() - m_pendAt;
    m_pendBit = 0;
    m_ackTimer->stop();
    /* 把往返时延一起显示出来。这个数字本身也是诊断信息：平时一百多毫秒，
       突然涨到两秒就说明链路上有东西堵了 */
    showAck(m_pendLabel + QString(" 已确认（往返 %1 ms）").arg(ms), "#3ddc97");
}

void ControlPage::setLedState(bool on)
{
    m_ledOn = on;
    m_ledBtn->setText(on ? "LED：开" : "LED：关");
}
void ControlPage::setBuzzerState(bool on)
{
    m_buzzerOn = on;
    m_buzzerBtn->setText(on ? "蜂鸣器：开" : "蜂鸣器：关");
}
void ControlPage::toggleLed()
{
    m_ledOn = !m_ledOn;
    emit cmdRequested(Dev::LED, m_ledOn ? Act::ON : Act::OFF, 0, 0);
    noteCommand(QString("LED ") + (m_ledOn ? "开" : "关"), 0x01, m_ledOn);
}
void ControlPage::toggleBuzzer()
{
    m_buzzerOn = !m_buzzerOn;
    emit cmdRequested(Dev::BUZZER, m_buzzerOn ? Act::ON : Act::OFF, 0, 0);
    noteCommand(QString("蜂鸣器 ") + (m_buzzerOn ? "开" : "关"), 0x02, m_buzzerOn);
}
void ControlPage::carCmd(int act)
{
    int speed = m_speedSlider->value();
    int p2 = (act == Act::LEFT || act == Act::RIGHT) ? 1000 : 0;
    emit cmdRequested(Dev::MOTOR, act, speed, p2);

    /* 状态字的 bit2 只表示"电机在转/没转"，转向和调速反映不到位上，
       所以只有前进/后退/停止能做闭环确认 */
    static const char *names[] = { "", "前进", "停止", "后退", "左转", "右转", "调速" };
    const QString label = QString("小车 ") +
        ((act >= 1 && act <= 6) ? names[act] : "动作");
    if (act == Act::ON || act == Act::BACK) noteCommand(label, 0x04, true);
    else if (act == Act::OFF)               noteCommand(label, 0x04, false);
    else                                    noteCommand(label, 0, false);
}
