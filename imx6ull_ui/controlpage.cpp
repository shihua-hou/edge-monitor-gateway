#include "controlpage.h"
#include "iostheme.h"
#include "ioscard.h"
#include "iosswitch.h"
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
    /* 按钮统一走 iOS 填充样式：次级填充色 + 白字。
       iOS 里按钮不描边，靠填充色和周围留白区分层次。 */
    setStyleSheet(QString(
        "QPushButton { background:%1; color:%2; border:none;"
        "  border-radius:14px; font-size:16px; }"
        "QPushButton:pressed { background:%3; }")
        .arg(QColor(0x2C,0x2C,0x2E).name())
        .arg(Ios::label().name())
        .arg(QColor(0x3A,0x3A,0x3C).name()));

    QHBoxLayout *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);

    // ---- 执行设备 ----
    IosCard *devPanel = new IosCard(this, 18);
    QVBoxLayout *devLayout = new QVBoxLayout(devPanel);
    devLayout->setContentsMargins(18, 16, 18, 16);
    devLayout->setSpacing(10);
    QLabel *devTitle = new QLabel("执行设备", devPanel);
    devTitle->setFont(Ios::fontHeadline());
    devTitle->setStyleSheet(QString("color:%1;").arg(Ios::label().name()));
    devLayout->addWidget(devTitle);

    /* 不用 emoji 前缀（💡🔔 这类真彩色 emoji 在嵌入式系统字体下大概率显示
       成方块），文字本身已经说清楚是什么按钮，靠开关态的背景色区分够用 */
    /* iOS 分组列表：一行一个设备，左边名字、右边开关，行高 52。 */
    m_ledSw    = new IosSwitch(devPanel);
    m_buzzerSw = new IosSwitch(devPanel);
    struct { const char *name; IosSwitch *sw; } rows[2] = {
        { "LED", m_ledSw }, { "蜂鸣器", m_buzzerSw }
    };
    for (int i = 0; i < 2; i++) {
        QWidget *row = new QWidget(devPanel);
        row->setMinimumHeight(52);
        QHBoxLayout *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        QLabel *n = new QLabel(QString::fromUtf8(rows[i].name), row);
        n->setFont(Ios::fontBody());
        n->setStyleSheet(QString("color:%1;").arg(Ios::label().name()));
        rl->addWidget(n);
        rl->addStretch();
        rl->addWidget(rows[i].sw);
        devLayout->addWidget(row);
        if (i == 0) {
            /* 行之间一条 1px 分隔线——iOS 分组列表的标志。
               不画的话两行会糊成一块。 */
            QFrame *sep = new QFrame(devPanel);
            sep->setFixedHeight(1);
            sep->setStyleSheet(QString("background:%1;")
                               .arg(Ios::separator().name(QColor::HexArgb)));
            devLayout->addWidget(sep);
        }
    }
    connect(m_ledSw,    &IosSwitch::clicked, this, &ControlPage::toggleLed);
    connect(m_buzzerSw, &IosSwitch::clicked, this, &ControlPage::toggleBuzzer);
    devLayout->addStretch();
    root->addWidget(devPanel, 1);

    // ---- 小车控制 ----
    IosCard *carPanel = new IosCard(this, 18);
    QVBoxLayout *carLayout = new QVBoxLayout(carPanel);
    carLayout->setContentsMargins(18, 16, 18, 16);
    carLayout->setSpacing(12);
    QLabel *carTitle = new QLabel("小车运动控制", carPanel);
    carTitle->setFont(Ios::fontHeadline());
    carTitle->setStyleSheet(QString("color:%1;").arg(Ios::label().name()));
    carLayout->addWidget(carTitle);

    QHBoxLayout *speedRow = new QHBoxLayout;
    m_speedSlider = new QSlider(Qt::Horizontal, carPanel);
    m_speedSlider->setRange(0, 100);
    m_speedSlider->setValue(60);
    /* iOS 滑块：细轨道（高 4）+ 大白圆旋钮（直径 26）。
       Qt 默认的 QSlider 手柄是矩形、轨道很粗，不套样式完全不像。
       旋钮用 margin 负值把它顶出轨道，否则会被轨道高度限住。 */
    m_speedSlider->setStyleSheet(QString(
        "QSlider::groove:horizontal { height:4px; border-radius:2px; background:%1; }"
        "QSlider::sub-page:horizontal { height:4px; border-radius:2px; background:%2; }"
        "QSlider::handle:horizontal { width:26px; height:26px; margin:-11px 0;"
        "  border-radius:13px; background:#ffffff; }")
        .arg(Ios::fill().name(QColor::HexArgb))
        .arg(Ios::blue().name()));
    m_speedVal = new QLabel("60%", carPanel);
    m_speedVal->setFont(Ios::fontNumber(17));
    m_speedVal->setStyleSheet(QString("color:%1; min-width:52px;").arg(Ios::label().name()));
    m_speedVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    connect(m_speedSlider, &QSlider::valueChanged, this, [this](int v) {
        m_speedVal->setText(QString::number(v) + "%");
    });
    speedRow->addWidget(m_speedSlider);
    speedRow->addWidget(m_speedVal);
    carLayout->addLayout(speedRow);

    QGridLayout *dpad = new QGridLayout;
    dpad->setSpacing(10);
    QPushButton *fwdBtn   = new QPushButton("前进", carPanel);
    QPushButton *leftBtn  = new QPushButton("左转", carPanel);
    QPushButton *stopBtn  = new QPushButton("停止", carPanel);
    QPushButton *rightBtn = new QPushButton("右转", carPanel);
    QPushButton *backBtn  = new QPushButton("后退", carPanel);
    /* 停止是唯一的破坏性动作，用 systemRed 实心填充。
       iOS 的规矩：危险动作用颜色区分，不靠描边。
       同时去掉了 ▲◀■ 这些几何符号：嵌入式字体对它们的覆盖不可靠（跟当初
       emoji 显示成方块是同一类问题），而且 iOS 本身也不用字符当图标。 */
    stopBtn->setStyleSheet(QString(
        "QPushButton { background:%1; color:white; border:none; border-radius:14px;"
        "  font-size:16px; font-weight:600; }"
        "QPushButton:pressed { background:%2; }")
        .arg(Ios::red().name())
        .arg(Ios::red().darker(120).name()));
    for (QPushButton *b : { fwdBtn, leftBtn, stopBtn, rightBtn, backBtn }) {
        b->setMinimumHeight(72);
        b->setFont(Ios::fontBody());
    }
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
    m_ackLabel->setFont(Ios::fontFootnote());
    m_ackLabel->setStyleSheet(QString("color:%1;").arg(Ios::gray().name()));
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
    m_ackLabel->setStyleSheet(QString("color:%1;").arg(color));
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

/* 设备回传的真实状态。用 blockSignals 包住：
   setChecked 会触发 clicked 信号 -> 又发一次命令 -> 无限往复。
   这是"状态回写"类 UI 最常见的一个坑。 */
void ControlPage::setLedState(bool on)
{
    m_ledOn = on;
    m_ledSw->blockSignals(true);
    m_ledSw->setChecked(on);
    m_ledSw->blockSignals(false);
}
void ControlPage::setBuzzerState(bool on)
{
    m_buzzerOn = on;
    m_buzzerSw->blockSignals(true);
    m_buzzerSw->setChecked(on);
    m_buzzerSw->blockSignals(false);
}
void ControlPage::toggleLed()
{
    m_ledOn = m_ledSw->isChecked();
    emit cmdRequested(Dev::LED, m_ledOn ? Act::ON : Act::OFF, 0, 0);
    noteCommand(QString("LED ") + (m_ledOn ? "开" : "关"), 0x01, m_ledOn);
}
void ControlPage::toggleBuzzer()
{
    m_buzzerOn = m_buzzerSw->isChecked();
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
