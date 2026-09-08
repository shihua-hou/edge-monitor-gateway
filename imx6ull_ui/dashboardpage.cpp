#include "dashboardpage.h"
#include "attitudewidget.h"

#include <QLabel>
#include <QFrame>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDateTime>
#include <QTimer>

/* 数值标签的两套【完整】样式。
   必须是完整的两份，不能只在需要时 setStyleSheet("color:...")——Qt 的
   setStyleSheet 是整份替换而不是叠加：只设 color 会把构造时设好的
   font-size 和 font-weight 一起抹掉，数字瞬间缩成默认字号。
   （第一版正是这么错的：用 setStyleSheet("") 想"恢复默认样式"，
     实际效果是把这个标签的所有样式全清空了。） */
static const char *kValStyleOk    = "color:#3ddc97; font-size:24px; font-weight:700;";
static const char *kValStyleStale = "color:#5a6577; font-size:24px; font-weight:700;";

static QFrame *card(QWidget *parent, const QString &label, QLabel **valueOut)
{
    QFrame *c = new QFrame(parent);
    c->setObjectName("card");
    QVBoxLayout *l = new QVBoxLayout(c);
    l->setSpacing(4);
    QLabel *lab = new QLabel(label, c);
    lab->setStyleSheet("color:#8a94a6; font-size:12px;");
    QLabel *val = new QLabel("--", c);
    val->setStyleSheet(kValStyleOk);
    l->addWidget(lab);
    l->addWidget(val);
    *valueOut = val;
    return c;
}

DashboardPage::DashboardPage(QWidget *parent) : QWidget(parent)
{
    setStyleSheet(
        "#card, #panel { background:#141b28; border:1px solid #232f42; border-radius:12px; padding:12px; }"
    );

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setSpacing(14);

    // ---- 顶部：时钟 + 天气 ----
    QFrame *hero = new QFrame(this);
    hero->setObjectName("panel");
    QHBoxLayout *heroLayout = new QHBoxLayout(hero);

    QVBoxLayout *clockCol = new QVBoxLayout;
    m_clock = new QLabel("--:--:--", hero);
    m_clock->setStyleSheet("font-size:38px; font-weight:800; color:#e6e6e6;");
    m_dateLabel = new QLabel("----", hero);
    m_dateLabel->setStyleSheet("font-size:13px; color:#8a94a6;");
    clockCol->addWidget(m_clock);
    clockCol->addWidget(m_dateLabel);
    heroLayout->addLayout(clockCol);
    heroLayout->addStretch();

    QVBoxLayout *weatherCol = new QVBoxLayout;
    weatherCol->setAlignment(Qt::AlignRight);
    m_weatherTemp = new QLabel("--℃", hero);
    m_weatherTemp->setStyleSheet("font-size:30px; font-weight:700; color:#5aa9ff;");
    m_weatherTemp->setAlignment(Qt::AlignRight);
    m_weatherText = new QLabel("天气未配置", hero);
    m_weatherText->setStyleSheet("font-size:13px; color:#8a94a6;");
    m_weatherText->setAlignment(Qt::AlignRight);
    weatherCol->addWidget(m_weatherTemp);
    weatherCol->addWidget(m_weatherText);
    heroLayout->addLayout(weatherCol);
    root->addWidget(hero);

    // ---- 中部：传感器卡片 + 姿态仪 ----
    QHBoxLayout *mid = new QHBoxLayout;
    mid->setSpacing(14);

    QGridLayout *grid = new QGridLayout;
    grid->setSpacing(10);
    grid->addWidget(card(this, "温度 ℃", &m_vTemp), 0, 0);
    grid->addWidget(card(this, "湿度 %", &m_vHumi), 0, 1);
    grid->addWidget(card(this, "光照 %", &m_vLight), 1, 0);
    grid->addWidget(card(this, "障碍距离 cm", &m_vDist), 1, 1);
    QWidget *gridWrap = new QWidget(this);
    gridWrap->setLayout(grid);
    mid->addWidget(gridWrap, 3);

    QFrame *attPanel = new QFrame(this);
    attPanel->setObjectName("panel");
    QVBoxLayout *attLayout = new QVBoxLayout(attPanel);
    QLabel *attTitle = new QLabel("姿态水平仪", attPanel);
    attTitle->setStyleSheet("color:#8a94a6; font-size:12px;");
    m_attitude = new AttitudeWidget(attPanel);
    attLayout->addWidget(attTitle);
    attLayout->addWidget(m_attitude, 1);
    mid->addWidget(attPanel, 2);

    root->addLayout(mid, 1);

    QTimer *clockTimer = new QTimer(this);
    connect(clockTimer, &QTimer::timeout, this, &DashboardPage::tickClock);
    clockTimer->start(1000);
    tickClock();
}

void DashboardPage::tickClock()
{
    QDateTime now = QDateTime::currentDateTime();
    m_clock->setText(now.toString("HH:mm:ss"));
    static const char *weekdays[] = {"", "周一","周二","周三","周四","周五","周六","周日"};
    m_dateLabel->setText(now.toString("yyyy年MM月dd日 ") + weekdays[now.date().dayOfWeek()]);
}

/* 传感器故障位，跟固件 app_task.h 的 SENS_FAULT_* 一一对应。
   这里重复定义一份而不是包含固件头文件：那边是 STM32 的裸机代码，
   带着一堆 HAL 依赖，为了四个常量把它拉进 Qt 工程不划算。
   代价是改协议时两处要一起改——所以两边注释里都写明了对应关系。 */
static const int kFaultTh    = 0x01;   /* 温湿度 */
static const int kFaultDist  = 0x02;   /* 超声波 */
static const int kFaultLight = 0x04;   /* 光敏 */
static const int kFaultImu   = 0x08;   /* 姿态 */

void DashboardPage::applyValue(QLabel *lab, const QString &text, int faultBit)
{
    const bool stale = (m_health & faultBit) != 0;
    /* 数值后面直接跟一个"失联"字样。只把颜色调暗是不够的——人看到一个
       数字，默认就当它是当前值，必须用文字点破 */
    lab->setText(stale ? (text + " 失联") : text);
    lab->setStyleSheet(stale ? kValStyleStale : kValStyleOk);
}

void DashboardPage::updateStatus(double temp, double humi, int light, int dist, int)
{
    applyValue(m_vTemp,  QString::number(temp, 'f', 1), kFaultTh);
    applyValue(m_vHumi,  QString::number(humi, 'f', 1), kFaultTh);
    applyValue(m_vLight, QString::number(light),        kFaultLight);
    applyValue(m_vDist,  QString::number(dist),         kFaultDist);
}

void DashboardPage::updateSensorHealth(int healthBits)
{
    if (healthBits == m_health) return;
    m_health = healthBits;
    /* 只更新样式，不改数值：这一帧只带来了健康位，数值还是上一帧的。
       等下一帧 status 到达时 updateStatus 会连数值带样式一起刷新，
       但那要等 200ms，先把标记打上，反应更快 */
    applyValue(m_vTemp,  m_vTemp->text().section(' ', 0, 0),  kFaultTh);
    applyValue(m_vHumi,  m_vHumi->text().section(' ', 0, 0),  kFaultTh);
    applyValue(m_vLight, m_vLight->text().section(' ', 0, 0), kFaultLight);
    applyValue(m_vDist,  m_vDist->text().section(' ', 0, 0),  kFaultDist);
    m_attitude->setStale((m_health & kFaultImu) != 0);
}

void DashboardPage::updateImu(double pitch, double roll, double yaw)
{
    m_attitude->setAttitude(pitch, roll, yaw);
}

void DashboardPage::updateWeather(const QString &tempC, const QString &text, const QString &city)
{
    m_weatherTemp->setText(tempC + "℃");
    m_weatherText->setText(city.isEmpty() ? text : (city + " · " + text));
}

void DashboardPage::setWeatherError(const QString &msg)
{
    m_weatherText->setText(msg);
}
