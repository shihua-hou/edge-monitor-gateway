#include "vehiclepage.h"
#include "iostheme.h"
#include "ioscard.h"

#include <QLabel>
#include <QPainter>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPropertyAnimation>
#include <QtMath>

/* 轮子参数必须和固件 motor.h 里的一致。
   这是"同一个常量存在两处"的典型场景——固件按它算里程，界面按它算车速。
   两边不一致的话，车速和里程会各说各话，而且都"看起来合理"。
   固件那份是权威，这里改动前先确认 motor.h。 */
static const double kWheelCircM = 0.204;      /* Φ65 轮周长，米 */
static const double kMaxKmh     = 2.5;        /* 表盘满量程：200RPM ≈ 2.45km/h */

/* 3S 锂电放电曲线（静态电压 → 剩余电量）。
   查表 + 线性插值，不用简单的线性映射：锂电的放电曲线中段很平
   （11.9~12.2V 覆盖了 40% 电量），线性映射会让中段掉得飞快、
   两头几乎不动，用户会觉得"电量表乱跳"。 */
struct BattPoint { double v; int pct; };
static const BattPoint kBattCurve[] = {
    {12.60, 100}, {12.45, 90}, {12.33, 80}, {12.19, 70}, {12.06, 60},
    {11.94, 50},  {11.81, 40}, {11.72, 30}, {11.66, 20}, {11.51, 10},
    {11.10, 5},   {9.90,  0}
};
static const int kBattPoints = int(sizeof(kBattCurve) / sizeof(kBattCurve[0]));

/* 满电标称续航（分钟）。没有电流传感器，这个数只能靠实测跑一次得出，
   现在填的是估计值——所以界面上一律写"约"。 */
static const int kFullRangeMin = 90;

static int battPercent(double v)
{
    if (v >= kBattCurve[0].v) return 100;
    for (int i = 1; i < kBattPoints; i++) {
        if (v >= kBattCurve[i].v) {
            const BattPoint &hi = kBattCurve[i - 1], &lo = kBattCurve[i];
            double t = (v - lo.v) / (hi.v - lo.v);
            return int(lo.pct + t * (hi.pct - lo.pct) + 0.5);
        }
    }
    return 0;
}

/* ================= 环形速度表 ================= */
/* iOS 的表盘语言：细环、圆头端点、大数字居中，不画刻度盘那种拟物件。
   指针式仪表是传统车机/Android 的做法，iOS 的健康/活动都是圆环。 */
class SpeedGauge : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(double shown READ shown WRITE setShown)
public:
    explicit SpeedGauge(QWidget *parent = nullptr)
        : QWidget(parent), m_target(0), m_shown(0)
    {
        /* 最小尺寸给小，靠 Expanding 吃剩余空间。
           **QStackedWidget 的最小尺寸取所有页面里最大的那个**，不是当前页——
           所以这里写 260 的话，即使你在助手页，它也一直在占着 260px 的高度，
           把软键盘挤到只剩两行。这条是整套 UI 上最反直觉的一个约束。 */
        setMinimumSize(150, 150);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_anim = new QPropertyAnimation(this, "shown", this);
        m_anim->setDuration(Ios::durNormal());
        m_anim->setEasingCurve(Ios::spring());
    }

    double shown() const { return m_shown; }
    void setShown(double v) { m_shown = v; update(); }

    void setSpeed(double kmh)
    {
        if (qAbs(kmh - m_target) < 0.005) return;
        m_target = kmh;
        /* 数值动画而不是瞬跳：车速本身是连续量，瞬跳会让人以为读数在抖。
           只重绘表盘这一块（260x260），代价可接受。 */
        m_anim->stop();
        m_anim->setStartValue(m_shown);
        m_anim->setEndValue(kmh);
        m_anim->start();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const double side = qMin(width(), height());
        const double pen  = side * 0.075;
        QRectF r((width() - side) / 2 + pen / 2, (height() - side) / 2 + pen / 2,
                 side - pen, side - pen);

        /* 240° 扫角，从左下 210° 起顺时针。Qt 的角度单位是 1/16 度，
           且逆时针为正——这里的负号不是笔误。 */
        const int startAngle = 210 * 16;
        const int span       = -240 * 16;

        QPen bg(Ios::fill(), pen, Qt::SolidLine, Qt::RoundCap);
        p.setPen(bg);
        p.drawArc(r, startAngle, span);

        double ratio = qBound(0.0, m_shown / kMaxKmh, 1.0);
        if (ratio > 0.001) {
            /* 接近满量程时变橙：车速本身不危险，但"已经顶到上限"
               这件事值得让人看见。 */
            QPen fg(ratio > 0.9 ? Ios::orange() : Ios::blue(),
                    pen, Qt::SolidLine, Qt::RoundCap);
            p.setPen(fg);
            p.drawArc(r, startAngle, int(span * ratio));
        }

        p.setPen(Ios::label());
        QFont f = Ios::fontNumber(int(side * 0.26));
        p.setFont(f);
        QRectF numRect = r.adjusted(0, -side * 0.04, 0, -side * 0.04);
        p.drawText(numRect, Qt::AlignCenter, QString::number(m_shown, 'f', 2));

        p.setPen(Ios::labelSecondary());
        p.setFont(Ios::fontFootnote());
        p.drawText(QRectF(r.left(), r.center().y() + side * 0.16, r.width(), 24),
                   Qt::AlignHCenter | Qt::AlignTop, "km/h");
    }

private:
    double m_target, m_shown;
    QPropertyAnimation *m_anim;
};

/* ================= 页面 ================= */
static QLabel *bigVal(QWidget *parent, int px = 30)
{
    QLabel *l = new QLabel("--", parent);
    l->setFont(Ios::fontNumber(px));
    l->setStyleSheet(QString("color:%1;").arg(Ios::label().name()));
    return l;
}
static QLabel *capLab(QWidget *parent, const QString &t)
{
    QLabel *l = new QLabel(t, parent);
    l->setFont(Ios::fontFootnote());
    l->setStyleSheet(QString("color:%1;").arg(Ios::labelSecondary().name(QColor::HexArgb)));
    return l;
}

VehiclePage::VehiclePage(QWidget *parent) : QWidget(parent)
{
    QHBoxLayout *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);

    // ---- 左：速度表 ----
    IosCard *gaugeCard = new IosCard(this, 18);
    QVBoxLayout *gl = new QVBoxLayout(gaugeCard);
    gl->setContentsMargins(18, 16, 18, 16);
    QLabel *gt = new QLabel("车速", gaugeCard);
    gt->setFont(Ios::fontHeadline());
    gt->setStyleSheet(QString("color:%1;").arg(Ios::label().name()));
    m_gauge = new SpeedGauge(gaugeCard);
    m_motorState = capLab(gaugeCard, "待机");
    m_motorState->setAlignment(Qt::AlignHCenter);
    gl->addWidget(gt);
    gl->addWidget(m_gauge, 1);
    gl->addWidget(m_motorState);
    root->addWidget(gaugeCard, 5);

    // ---- 右：里程 / 电池 / 左右轮 ----
    QVBoxLayout *col = new QVBoxLayout;
    col->setSpacing(12);

    IosCard *odoCard = new IosCard(this, 16);
    QVBoxLayout *ol = new QVBoxLayout(odoCard);
    ol->setContentsMargins(18, 14, 18, 14);
    ol->setSpacing(2);
    ol->addWidget(capLab(odoCard, "累计里程 km"));
    m_odom = bigVal(odoCard, 34);
    ol->addWidget(m_odom);
    col->addWidget(odoCard);

    IosCard *battCard = new IosCard(this, 16);
    QVBoxLayout *bl = new QVBoxLayout(battCard);
    bl->setContentsMargins(18, 14, 18, 14);
    bl->setSpacing(2);
    bl->addWidget(capLab(battCard, "电池"));
    QHBoxLayout *brow = new QHBoxLayout;
    m_battPct = bigVal(battCard, 34);
    m_battVolt = capLab(battCard, "-- V");
    brow->addWidget(m_battPct);
    brow->addStretch();
    brow->addWidget(m_battVolt);
    bl->addLayout(brow);
    m_range = capLab(battCard, "续航 --");
    bl->addWidget(m_range);
    col->addWidget(battCard);

    IosCard *whCard = new IosCard(this, 16);
    QVBoxLayout *wl = new QVBoxLayout(whCard);
    wl->setContentsMargins(18, 14, 18, 14);
    wl->setSpacing(2);
    wl->addWidget(capLab(whCard, "左 / 右轮转速 RPM"));
    QHBoxLayout *wrow = new QHBoxLayout;
    m_rpmL = bigVal(whCard, 30);
    m_rpmR = bigVal(whCard, 30);
    wrow->addWidget(m_rpmL);
    wrow->addStretch();
    wrow->addWidget(m_rpmR);
    wl->addLayout(wrow);
    col->addWidget(whCard);

    /* 定位状态卡。原来这儿是个 addStretch，右下角空一大块——
       与其塞占位内容，不如放真正属于这一页的数据。 */
    IosCard *gpsCard = new IosCard(this, 16);
    QVBoxLayout *pl = new QVBoxLayout(gpsCard);
    pl->setContentsMargins(18, 14, 18, 14);
    pl->setSpacing(2);
    pl->addWidget(capLab(gpsCard, "定位状态"));
    m_fixText = bigVal(gpsCard, 30);
    m_fixText->setText("无定位");
    pl->addWidget(m_fixText);
    QHBoxLayout *grow = new QHBoxLayout;
    m_satHdop  = capLab(gpsCard, "卫星 -- / HDOP --");
    m_gpsSpeed = capLab(gpsCard, "-- km/h");
    grow->addWidget(m_satHdop);
    grow->addStretch();
    grow->addWidget(m_gpsSpeed);
    pl->addLayout(grow);
    col->addWidget(gpsCard);

    root->addLayout(col, 4);
}

void VehiclePage::updateGps(int fix, const QString &fixText, int sat, double hdop, double speedKmh)
{
    m_fixText->setText(fixText.isEmpty() ? "无定位" : fixText);
    /* 颜色直接说明能力等级：固定解绿、浮动解橙、单点/差分白、无解灰。
       RTK 固定和浮动差一个数量级（厘米 vs 分米），
       笼统显示成"已定位"等于把 RTK 最重要的信息扇掉了。 */
    QColor c = Ios::gray();
    if (fix == 4)      c = Ios::green();
    else if (fix == 5) c = Ios::orange();
    else if (fix > 0)  c = Ios::label();
    m_fixText->setStyleSheet(QString("color:%1;").arg(c.name()));

    /* HDOP 9999 是接收机在"无解"时填的哨兵值，不是真实精度因子。
       原样显示会让人以为"精度差到 9999"，实际是"根本没算出来"。 */
    const QString hd = (hdop <= 0 || hdop > 90) ? QString("--") : QString::number(hdop, 'f', 1);
    m_satHdop->setText(QString("卫星 %1 / HDOP %2").arg(sat).arg(hd));
    m_gpsSpeed->setText(fix > 0 ? QString::number(speedKmh, 'f', 2) + " km/h" : "-- km/h");
}

void VehiclePage::updateMotor(int rpmL, int rpmR, int odomL, int odomR)
{
    /* 车速取两轮平均的绝对值：原地转向时左右符号相反，平均后接近 0，
       这正确——原地转向的"前进速度"确实是 0。 */
    double avgRpm = (rpmL + rpmR) / 2.0;
    double kmh = qAbs(avgRpm) * kWheelCircM / 60.0 * 3.6;
    m_gauge->setSpeed(kmh);

    m_rpmL->setText(QString::number(rpmL));
    m_rpmR->setText(QString::number(rpmR));
    /* 一侧正一侧负 = 在原地转向，单独标出来：
       这时候车速显示接近 0，但车其实在动，不说明会让人以为卡住了。 */
    const bool spinning = (rpmL > 0 && rpmR < 0) || (rpmL < 0 && rpmR > 0);
    if (spinning)                 m_motorState->setText("原地转向中");
    else if (avgRpm > 1)          m_motorState->setText("前进");
    else if (avgRpm < -1)         m_motorState->setText("后退");
    else                          m_motorState->setText("待机");

    double km = (odomL + odomR) / 2.0 / 100.0 / 1000.0;
    m_odom->setText(QString::number(km, 'f', 3));
}

void VehiclePage::updateBattery(double volt)
{
    /* 0 = ADC 线没接，不是"没电了"。这两种必须显示成不同的样子，
       否则每台没接线的设备都在报低电量，假告警看多了真告警也没人信。 */
    if (volt <= 0.05) {
        m_battPct->setText("--");
        m_battPct->setStyleSheet(QString("color:%1;").arg(Ios::gray().name()));
        m_battVolt->setText("未接入");
        m_range->setText("续航 -- （电压采集未接线）");
        return;
    }

    int pct = battPercent(volt);
    m_battPct->setText(QString::number(pct) + "%");
    m_battPct->setStyleSheet(QString("color:%1;")
        .arg(pct <= 15 ? Ios::red().name()
           : pct <= 30 ? Ios::orange().name()
                       : Ios::label().name()));
    m_battVolt->setText(QString::number(volt, 'f', 2) + " V");
    /* 写"约"，并且把电压原值一起摆出来——没有电流传感器，
       这个数反映不了负载差异，用户得能看到它是怎么来的。 */
    m_range->setText(QString("续航约 %1 分钟（按电压估算，未计负载）")
                     .arg(pct * kFullRangeMin / 100));
}

void VehiclePage::updateHealth(int hf)
{
    /* bit4 = SENS_FAULT_MOTOR：初始化失败，或**飞车保护已触发**。
       后者必须看得见——不显示的话界面上只有"转速 0"，
       和正常待机一模一样。 */
    if (hf & 0x10) {
        m_motorState->setText("电机异常 / 飞车保护已触发");
        m_motorState->setStyleSheet(QString("color:%1;").arg(Ios::red().name()));
    } else {
        m_motorState->setStyleSheet(QString("color:%1;")
            .arg(Ios::labelSecondary().name(QColor::HexArgb)));
    }
}

#include "vehiclepage.moc"
