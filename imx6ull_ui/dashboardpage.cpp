#include "dashboardpage.h"
#include "attitudewidget.h"
#include "iostheme.h"
#include "ioscard.h"

#include <QLabel>
#include <QFrame>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDateTime>
#include <QTimer>
#include <QPainter>
#include <QVector>
#include <QPolygonF>
#include <QSizePolicy>

/* 数值标签的两套【完整】样式。
   必须是完整的两份，不能只在需要时 setStyleSheet("color:...")——Qt 的
   setStyleSheet 是整份替换而不是叠加：只设 color 会把构造时设好的
   font-size 和 font-weight 一起抹掉，数字瞬间缩成默认字号。
   （第一版正是这么错的：用 setStyleSheet("") 想"恢复默认样式"，
     实际效果是把这个标签的所有样式全清空了。） */
static QString valStyleOk()    { return QString("color:%1;").arg(Ios::label().name()); }
static QString valStyleStale() { return QString("color:%1;").arg(Ios::gray().name()); }


/* Sparkline - 卡片里的微型趋势线。
 *
 * 为什么值得加：一个瞬时数字回答不了"它在往哪走"。障碍距离 30cm
 * 是正在接近还是正在远离，是两件完全不同的事，而单看数字分不出来。
 * 车载场景下这种"趋势"往往比精确值更有用。
 *
 * 为什么不用图表库：这块板子是软件渲染的单核 A7，引一个图表库进来
 * 既是几百 KB 的体积，也是每帧多几倍的绘制开销。而这里要画的东西
 * 一共就是一条折线加一个渐变填充，QPainter 三十行就够。
 */
class Sparkline : public QWidget
{
    Q_OBJECT
public:
    /* minSpan：纵轴的**最小跨度**，按指标的物理量纲给。
       没有它就是纯自动缩放，而自动缩放对"几乎不变"的数据是灾难：
       湿度在 77.7~78.1 之间抖 0.4，会被拉满整个纵轴画成方波——
       屏幕上看着像剧烈震荡，实际上非常稳定。
       图表骗人最常见的方式就是这个，而且画的人往往没意识到。 */
    explicit Sparkline(const QColor &color, double minSpan, QWidget *parent = nullptr)
        : QWidget(parent), m_col(color), m_minSpan(minSpan)
    {
        /* **最小高度必须给得很小**，靠 Expanding 去吃版面剩下的空间，
           而不是靠 minimumHeight 去索取空间。
           这是个踩过的坑：一开始写的是 setMinimumHeight(34)，四张卡片
           一共给整窗的最小尺寸加了 80px，直接超过 600px 的屏高——
           Qt 不会把窗口缩到 minimumSizeHint 以下，于是 showFullScreen()
           之后窗口反而比屏幕大，底下一行卡片和侧边栏的状态行被裁在屏幕外。
           现象是"界面显示不全"，很难想到是某个子控件的最小高度引起的。 */
        setMinimumHeight(10);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    void push(double v)
    {
        m_data.append(v);
        /* 只留最近 90 个点。按 200ms 一帧算是 18 秒，正好是"刚刚"的跨度；
           留太多的话早期的大幅波动会把纵轴撑开，近期的细微变化反而被压平。 */
        if (m_data.size() > 90) m_data.remove(0, m_data.size() - 90);
        update();
    }

    void setStale(bool s) { if (m_stale != s) { m_stale = s; update(); } }

    /* 期望高度给 34：版面宽裕时按这个画，紧张时可以一路压到 10。 */
    QSize sizeHint() const override { return QSize(80, 34); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (m_data.size() < 2) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        double lo = m_data[0], hi = m_data[0];
        for (int i = 1; i < m_data.size(); i++) {
            lo = qMin(lo, m_data[i]);
            hi = qMax(hi, m_data[i]);
        }
        /* 数值完全不变时 hi==lo，按比例缩放会除零。
           给一个最小跨度，让平线落在中间而不是贴边——
           贴边的平线看起来像"卡在最大值"，是会误导人的。 */
        if (hi - lo < m_minSpan) {
            double mid = (hi + lo) / 2.0;
            hi = mid + m_minSpan / 2.0;
            lo = mid - m_minSpan / 2.0;
        }

        const QRectF r = QRectF(rect()).adjusted(1, 3, -1, -3);
        QPolygonF line;
        for (int i = 0; i < m_data.size(); i++) {
            double x = r.left() + r.width() * i / double(m_data.size() - 1);
            double y = r.bottom() - r.height() * (m_data[i] - lo) / (hi - lo);
            line << QPointF(x, y);
        }

        QColor c = m_stale ? Ios::gray() : m_col;

        QPolygonF area = line;
        area << QPointF(r.right(), r.bottom()) << QPointF(r.left(), r.bottom());
        QLinearGradient g(0, r.top(), 0, r.bottom());
        QColor c1 = c; c1.setAlpha(70);
        QColor c2 = c; c2.setAlpha(0);
        g.setColorAt(0, c1);
        g.setColorAt(1, c2);
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawPolygon(area);

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(line);

        /* 最新点画一个小圆：线的末端就是"现在"，标出来读图时不用找。 */
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(line.last(), 2.6, 2.6);
    }

private:
    QVector<double> m_data;
    QColor m_col;
    double m_minSpan;
    bool   m_stale = false;
};

/* iOS 信息卡：小标题在上（次级色、小字），大数字在下。
   这个上小下大的节奏是 iOS 小组件/健康 App 的标准做法：
   扫一眼先看到数，需要时才去读标题。车载场景尤其适用——
   司机没时间逐字读。 */
static IosCard *card(QWidget *parent, const QString &label, QLabel **valueOut,
                    const QColor &trendColor, double minSpan, Sparkline **sparkOut)
{
    IosCard *c = new IosCard(parent, 16);
    QVBoxLayout *l = new QVBoxLayout(c);
    l->setContentsMargins(16, 14, 16, 14);
    l->setSpacing(2);

    QLabel *lab = new QLabel(label, c);
    lab->setFont(Ios::fontFootnote());
    lab->setStyleSheet(QString("color:%1;").arg(Ios::labelSecondary().name(QColor::HexArgb)));

    QLabel *val = new QLabel("--", c);
    val->setFont(Ios::fontNumber(40));
    val->setStyleSheet(valStyleOk());

    /* 标签 + 数值作为一组垂直居中，不是"标签顶到头、数值沉到底"。
       后者在卡片变高时中间会空出一大块，看起来像排版坏了。
       iOS 的信息卡一向是内容成组、组内紧凑、组外留白。 */
    Sparkline *sp = new Sparkline(trendColor, minSpan, c);

    l->addStretch();
    l->addWidget(lab);
    l->addWidget(val);
    l->addSpacing(4);
    l->addWidget(sp, 1);
    *valueOut = val;
    *sparkOut = sp;
    return c;
}

DashboardPage::DashboardPage(QWidget *parent) : QWidget(parent)
{
    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);

    // ---- 顶部：时钟 + 天气 ----
    IosCard *hero = new IosCard(this, 18);
    QHBoxLayout *heroLayout = new QHBoxLayout(hero);
    heroLayout->setContentsMargins(20, 14, 20, 14);

    QVBoxLayout *clockCol = new QVBoxLayout;
    m_clock = new QLabel("--:--:--", hero);
    m_clock->setFont(Ios::fontNumber(44));
    m_clock->setStyleSheet(QString("color:%1;").arg(Ios::label().name()));
    m_dateLabel = new QLabel("----", hero);
    m_dateLabel->setFont(Ios::fontFootnote());
    m_dateLabel->setStyleSheet(QString("color:%1;").arg(Ios::labelSecondary().name(QColor::HexArgb)));
    clockCol->addWidget(m_clock);
    clockCol->addWidget(m_dateLabel);
    heroLayout->addLayout(clockCol);
    heroLayout->addStretch();

    QVBoxLayout *weatherCol = new QVBoxLayout;
    weatherCol->setAlignment(Qt::AlignRight);
    m_weatherTemp = new QLabel("--℃", hero);
    m_weatherTemp->setFont(Ios::fontNumber(34));
    m_weatherTemp->setStyleSheet(QString("color:%1;").arg(Ios::label().name()));
    m_weatherTemp->setAlignment(Qt::AlignRight);
    m_weatherText = new QLabel("天气未配置", hero);
    m_weatherText->setFont(Ios::fontFootnote());
    m_weatherText->setStyleSheet(QString("color:%1;").arg(Ios::labelSecondary().name(QColor::HexArgb)));
    m_weatherText->setAlignment(Qt::AlignRight);
    weatherCol->addWidget(m_weatherTemp);
    weatherCol->addWidget(m_weatherText);
    heroLayout->addLayout(weatherCol);
    root->addWidget(hero);

    // ---- 中部：传感器卡片 + 姿态仪 ----
    QHBoxLayout *mid = new QHBoxLayout;
    mid->setSpacing(14);

    QGridLayout *grid = new QGridLayout;
    grid->setSpacing(12);
    /* 趋势线各用一个颜色，和指标的语义对上：
       温度暖色、湿度蓝、光照橙、距离绿（绿只是基准色，
       真到近距离时数值本身会转黄转红，见 updateStatus）。 */
    /* 第四个参数是纵轴最小跨度，按这个量的"多大算有变化"来定：
       室温 2℃、湿度 5%、光照 20%、障碍距离 30cm。
       小于这个幅度的抖动会被画成接近平直的线——它本来就该是平的。 */
    grid->addWidget(card(this, "温度 ℃",     &m_vTemp,  Ios::red(),     2.0,  &m_sTemp),  0, 0);
    grid->addWidget(card(this, "湿度 %",     &m_vHumi,  Ios::blue(),    5.0,  &m_sHumi),  0, 1);
    grid->addWidget(card(this, "光照 %",     &m_vLight, Ios::orange(), 20.0,  &m_sLight), 1, 0);
    grid->addWidget(card(this, "障碍距离 cm", &m_vDist,  Ios::green(),  30.0,  &m_sDist),  1, 1);
    QWidget *gridWrap = new QWidget(this);
    gridWrap->setLayout(grid);
    mid->addWidget(gridWrap, 3);

    IosCard *attPanel = new IosCard(this, 16);
    QVBoxLayout *attLayout = new QVBoxLayout(attPanel);
    attLayout->setContentsMargins(16, 12, 16, 16);
    attLayout->setSpacing(10);
    QLabel *attTitle = new QLabel("姿态水平仪", attPanel);
    attTitle->setFont(Ios::fontFootnote());
    attTitle->setStyleSheet(QString("color:%1;").arg(Ios::labelSecondary().name(QColor::HexArgb)));
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
    lab->setStyleSheet(stale ? valStyleStale() : valStyleOk());
}

void DashboardPage::updateStatus(double temp, double humi, int light, int dist, int)
{
    applyValue(m_vTemp,  QString::number(temp, 'f', 1), kFaultTh);
    applyValue(m_vHumi,  QString::number(humi, 'f', 1), kFaultTh);
    applyValue(m_vLight, QString::number(light),        kFaultLight);
    applyValue(m_vDist,  QString::number(dist),         kFaultDist);

    /* 趋势线只在数据有效时推。传感器失联时继续推最后一个值，
       线会画成一条平直线——那看起来像"稳定"，恰恰和真相相反。 */
    if (!(m_health & kFaultTh))    { m_sTemp->push(temp);  m_sHumi->push(humi); }
    if (!(m_health & kFaultLight))   m_sLight->push(light);
    if (!(m_health & kFaultDist))    m_sDist->push(dist);

    /* 障碍距离按远近变色：这是唯一一个"数值小=危险"的指标，
       其它几个都只是环境读数。不区分的话，快撞上了和天气凉快
       在界面上是同一种存在感。 */
    QColor dc = Ios::green();
    if (dist > 0 && dist < 20)      dc = Ios::red();
    else if (dist > 0 && dist < 50) dc = Ios::orange();
    m_vDist->setStyleSheet((m_health & kFaultDist)
                           ? valStyleStale()
                           : QString("color:%1;").arg(dc.name()));
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
    m_sTemp->setStale((m_health & kFaultTh) != 0);
    m_sHumi->setStale((m_health & kFaultTh) != 0);
    m_sLight->setStale((m_health & kFaultLight) != 0);
    m_sDist->setStale((m_health & kFaultDist) != 0);
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

#include "dashboardpage.moc"
