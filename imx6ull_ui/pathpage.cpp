#include "pathpage.h"
#include "iostheme.h"
#include "ioscard.h"

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QSizePolicy>
#include <QPainterPath>
#include <QDateTime>
#include <QTimer>
#include <QDebug>
#include <QtMath>

/* ===== 车辆常量（与 firmware_stm32/motor.h 保持一致） ===== */
static const double kMaxRpm     = 200.0;   /* pct=100 对应的转速 */
static const double kWheelCircM = 0.204;   /* 轮周长 */
/* 轮距（左右轮中心距）。从 WHEELTEC 四轮两驱底盘图纸量得：
     外缘 145mm、内侧间隙 58mm -> 胎厚 (145-58)/2 = 43.5mm
     轮心距 = 58 + 43.5 = 101.5mm */
static const double kTrackM     = 0.1015;

/* ===== 回放参数 ===== */
/* 回放指令重发周期。固件 ACT_TANK 有 500ms 超时，必须比它短得多，
   否则一段动作只发一次，车走 500ms 就被固件当成"上位机挂了"而停下。 */
static const int    kReplayMs   = 100;

enum { kDevMotor = 3,
       kActOn = 1, kActOff = 2, kActBack = 3, kActLeft = 4, kActRight = 5,
       kActSpeed = 6, kActTank = 9, kTankBias = 100 };

/* ================= 轨迹画布 ================= */
/* 本地 ENU 平面图：横轴东、纵轴北，单位米，原点是开始推算的位置。
 *
 * 为什么不叠在高德地图上：
 *   ① 中国境内公开地图服务按法规必须用 GCJ-02（火星坐标），而 WGS-84→GCJ-02
 *      的开源转换是逆向出来的近似，误差 1~5 米。RTK 给的是厘米级，
 *      转一道就被打掉两个数量级——显示环节反而成了精度瓶颈。
 *   ② 高德 JS API 必须联网。巡检车在园区里跑，WiFi 未必稳。
 *   本地 ENU 图不做任何坐标转换、纯本地渲染，精度一分不损，断网照常用。
 *   "车在园区哪儿"这种上下文需求交给大屏上的高德，两者各司其职。
 */
class TrackView : public QWidget
{
    Q_OBJECT
public:
    explicit TrackView(QWidget *parent = nullptr) : QWidget(parent)
    {
        /* 同 vehiclepage：QStackedWidget 按所有页面的最大值算最小尺寸，
           这里给大了会拖累其它页面（尤其是软键盘的可用高度）。 */
        setMinimumHeight(130);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_live.reserve(4096);
    }

    void setPose(double x, double y, double yaw)
    {
        m_x = x; m_y = y; m_yaw = yaw;
        /* 只有移动超过 2cm 才记一个点：不设阈值的话静止时也在
           不停往数组里堆同一个坐标，几分钟就上万个点，
           画线和自动缩放都会变慢，而轨迹一点没多。 */
        if (m_live.isEmpty() ||
            QLineF(m_live.last(), QPointF(x, y)).length() > 0.02) {
            m_live.append(QPointF(x, y));
            if (m_live.size() > 20000) m_live.remove(0, 5000);   /* 上限保护 */
        }
        update();
    }
    void clearLive() { m_live.clear(); m_span = 4.0; update(); }
    /* 回放时把实时线换个颜色：跟录制时那条摆在一起才看得出差多少 */
    void setLiveColor(const QColor &c) { m_liveCol = c; update(); }
    void setRecorded(const QVector<QPointF> &t) { m_rec = t; update(); }
    const QVector<QPointF> &live() const { return m_live; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath clip = Ios::squircle(r, 12);
        p.setClipPath(clip);
        p.fillPath(clip, QColor(0x14, 0x14, 0x18));

        /* 自动缩放：把所有点 + 当前位置框进来，留 15% 边距。
           固定比例尺的话，走远之后车就跑出画面了；而每帧重算范围
           会让画面不停微微跳动——所以范围只在超出当前视野时才扩，
           不会缩回去。 */
        double minx = m_x, maxx = m_x, miny = m_y, maxy = m_y;
        const QVector<QPointF> *sets[2] = { &m_live, &m_rec };
        for (int s = 0; s < 2; s++)
            for (int i = 0; i < sets[s]->size(); i++) {
                const QPointF &q = sets[s]->at(i);
                minx = qMin(minx, q.x()); maxx = qMax(maxx, q.x());
                miny = qMin(miny, q.y()); maxy = qMax(maxy, q.y());
            }
        double spanX = qMax(1.0, maxx - minx), spanY = qMax(1.0, maxy - miny);
        double span  = qMax(spanX, spanY) * 1.3;
        m_span = qMax(m_span, span);           /* 只扩不缩 */
        double cx = (minx + maxx) / 2, cy = (miny + maxy) / 2;

        const double side = qMin(r.width(), r.height());
        const double scale = side / m_span;    /* 像素/米 */

        /* 米制网格。格距随比例尺跳档，保证屏幕上格子大小始终合适——
           固定 1 米一格的话，跑出 50 米就成一片密线了。 */
        double step = 1.0;
        while (step * scale < 28) step *= 2;
        while (step * scale > 90) step /= 2;

        p.setPen(QPen(QColor(255, 255, 255, 16), 1));
        for (double gx = qFloor((cx - m_span / 2) / step) * step; gx < cx + m_span / 2; gx += step) {
            double px = r.center().x() + (gx - cx) * scale;
            p.drawLine(QPointF(px, r.top()), QPointF(px, r.bottom()));
        }
        for (double gy = qFloor((cy - m_span / 2) / step) * step; gy < cy + m_span / 2; gy += step) {
            double py = r.center().y() - (gy - cy) * scale;
            p.drawLine(QPointF(r.left(), py), QPointF(r.right(), py));
        }

        /* 比例尺标注：没有它，图上一段线到底是 1 米还是 50 米完全看不出来 */
        p.setPen(Ios::labelTertiary());
        p.setFont(Ios::fontCaption());
        p.drawText(QRectF(r.left() + 8, r.bottom() - 22, 160, 18),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QString("网格 %1 m").arg(step, 0, 'g', 2));

        /* 已录制的轨迹画在下层（灰），实时轨迹在上层（蓝） */
        drawPolyline(p, m_rec,  r, cx, cy, scale, QColor(0x8E, 0x8E, 0x93), 2.0);
        drawPolyline(p, m_live, r, cx, cy, scale, m_liveCol, 2.5);

        /* 起点 */
        if (!m_live.isEmpty()) {
            QPointF s = toPx(m_live.first(), r, cx, cy, scale);
            p.setPen(Qt::NoPen);
            p.setBrush(Ios::green());
            p.drawEllipse(s, 4, 4);
        }

        /* 当前位置 + 朝向 */
        QPointF c = toPx(QPointF(m_x, m_y), r, cx, cy, scale);
        p.setPen(Qt::NoPen);
        p.setBrush(m_liveCol);
        p.drawEllipse(c, 6, 6);
        p.setPen(QPen(Qt::white, 2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(c, QPointF(c.x() + qCos(m_yaw) * 16, c.y() - qSin(m_yaw) * 16));
    }

private:
    static QPointF toPx(const QPointF &m, const QRectF &r,
                        double cx, double cy, double scale)
    {
        /* 纵轴取反：数学上 y 向北为正，而屏幕坐标 y 向下为正 */
        return QPointF(r.center().x() + (m.x() - cx) * scale,
                       r.center().y() - (m.y() - cy) * scale);
    }
    static void drawPolyline(QPainter &p, const QVector<QPointF> &pts, const QRectF &r,
                             double cx, double cy, double scale,
                             const QColor &col, double w)
    {
        if (pts.size() < 2) return;
        QPainterPath path;
        path.moveTo(toPx(pts.first(), r, cx, cy, scale));
        for (int i = 1; i < pts.size(); i++)
            path.lineTo(toPx(pts.at(i), r, cx, cy, scale));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(col, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
    }

    QVector<QPointF> m_live, m_rec;
    double m_x = 0, m_y = 0, m_yaw = 0;
    double m_span = 4.0;      /* 当前视野边长(米)，只扩不缩；清空时复位 */
    QColor m_liveCol = Ios::blue();
};

/* ================= 页面 ================= */
PathPage::PathPage(QWidget *parent) : QWidget(parent)
{
    QHBoxLayout *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);

    IosCard *mapCard = new IosCard(this, 18);
    QVBoxLayout *ml = new QVBoxLayout(mapCard);
    ml->setContentsMargins(16, 14, 16, 16);
    ml->setSpacing(10);
    QLabel *mt = new QLabel("巡检轨迹", mapCard);
    mt->setFont(Ios::fontHeadline());
    mt->setStyleSheet(QString("color:%1;").arg(Ios::label().name()));
    m_view = new TrackView(mapCard);
    ml->addWidget(mt);
    ml->addWidget(m_view, 1);
    root->addWidget(mapCard, 5);

    QVBoxLayout *col = new QVBoxLayout;
    col->setSpacing(12);

    IosCard *statCard = new IosCard(this, 16);
    QVBoxLayout *sl = new QVBoxLayout(statCard);
    sl->setContentsMargins(18, 14, 18, 14);
    sl->setSpacing(6);
    QLabel *st = new QLabel("推算状态", statCard);
    st->setFont(Ios::fontFootnote());
    st->setStyleSheet(QString("color:%1;").arg(Ios::labelSecondary().name(QColor::HexArgb)));
    m_stats = new QLabel("--", statCard);
    m_stats->setFont(Ios::fontBody());
    m_stats->setStyleSheet(QString("color:%1;").arg(Ios::label().name()));
    m_stats->setWordWrap(true);
    sl->addWidget(st);
    sl->addWidget(m_stats);
    col->addWidget(statCard);

    IosCard *opCard = new IosCard(this, 16);
    QVBoxLayout *opl = new QVBoxLayout(opCard);
    opl->setContentsMargins(18, 14, 18, 14);
    opl->setSpacing(10);
    m_recBtn = new QPushButton("开始录制", opCard);
    m_playBtn = new QPushButton("回放路线", opCard);
    m_clearBtn = new QPushButton("清除轨迹", opCard);
    for (QPushButton *b : { m_recBtn, m_playBtn, m_clearBtn }) {
        b->setMinimumHeight(52);
        b->setFont(Ios::fontBody());
    }
    m_recBtn->setStyleSheet(QString(
        "QPushButton{background:%1;color:white;border:none;border-radius:14px;font-weight:600;}"
        "QPushButton:pressed{background:%2;}")
        .arg(Ios::blue().name()).arg(Ios::blue().darker(120).name()));
    m_clearBtn->setStyleSheet(QString(
        "QPushButton{background:%1;color:%2;border:none;border-radius:14px;}"
        "QPushButton:pressed{background:%3;}")
        .arg(QColor(0x2C,0x2C,0x2E).name()).arg(Ios::label().name())
        .arg(QColor(0x3A,0x3A,0x3C).name()));
    m_playBtn->setStyleSheet(QString(
        "QPushButton{background:%1;color:white;border:none;border-radius:14px;font-weight:600;}"
        "QPushButton:disabled{background:%2;color:%3;}"
        "QPushButton:pressed{background:%4;}")
        .arg(Ios::green().name())
        .arg(QColor(0x2C,0x2C,0x2E).name())
        .arg(Ios::gray().name())
        .arg(Ios::green().darker(120).name()));
    m_playBtn->setEnabled(false);
    opl->addWidget(m_recBtn);
    opl->addWidget(m_playBtn);
    opl->addWidget(m_clearBtn);
    col->addWidget(opCard);

    /* 诚实说明放在界面上，而不是只写在文档里。
       用推算出来的轨迹做判断的人，需要当场知道它的误差量级。 */
    IosCard *noteCard = new IosCard(this, 16);
    QVBoxLayout *nl = new QVBoxLayout(noteCard);
    nl->setContentsMargins(18, 14, 18, 14);
    m_hint = new QLabel(
        "录制的是<b>指令流</b>：你每一次前进/转向的原始指令和时刻，"
        "回放时按原时刻重发一遍。<br>"
        "回放是开环的——不看反馈、不修偏差。电量、地面、载重变了，"
        "走出来就是另一条线。灰色是按指令流算出的标称路线，"
        "蓝色是实时推算位置（陀螺会漂，仅供参考）。",
        noteCard);
    m_hint->setTextFormat(Qt::RichText);
    m_hint->setWordWrap(true);
    m_hint->setFont(Ios::fontFootnote());
    m_hint->setStyleSheet(QString("color:%1;").arg(Ios::labelSecondary().name(QColor::HexArgb)));
    nl->addWidget(m_hint);
    col->addWidget(noteCard);

    col->addStretch();
    root->addLayout(col, 4);

    connect(m_recBtn, &QPushButton::clicked, this, [this]() {
        m_recording = !m_recording;
        if (m_recording) {
            qDebug("[path] 开始录制");
            if (m_replaying) stopReplay("开始录制");
            m_segs.clear();
            resetPose();
            m_view->setRecorded(QVector<QPointF>());
            m_recBtn->setText("停止录制");
            m_recBtn->setStyleSheet(QString(
                "QPushButton{background:%1;color:white;border:none;border-radius:14px;font-weight:600;}"
                "QPushButton:pressed{background:%2;}")
                .arg(Ios::red().name()).arg(Ios::red().darker(120).name()));
        } else {
            m_recBtn->setText("开始录制");
            m_recBtn->setStyleSheet(QString(
                "QPushButton{background:%1;color:white;border:none;border-radius:14px;font-weight:600;}"
                "QPushButton:pressed{background:%2;}")
                .arg(Ios::blue().name()).arg(Ios::blue().darker(120).name()));
            qDebug("[path] 停止录制，%d 条指令", m_segs.size());
            /* 末尾往往是一条"停止"——回放时最后再补一条即可，录进去只会干等 */
            while (!m_segs.isEmpty() && m_segs.last().act == kActOff)
                m_segs.removeLast();
            rebuildNominal();
        }
        m_playBtn->setEnabled(!m_recording && !m_segs.isEmpty());
        updateStats();
    });

    connect(m_playBtn, &QPushButton::clicked, this, [this]() {
        if (m_replaying) stopReplay("手动停止");
        else             startReplay();
    });

    connect(m_clearBtn, &QPushButton::clicked, this, [this]() {
        qDebug("[path] 清除轨迹");
        m_dist = 0;
        resetPose();
        m_havePrev = false;
        m_startMs = QDateTime::currentMSecsSinceEpoch();
        m_segs.clear();
        m_view->setRecorded(QVector<QPointF>());
        if (m_replaying) stopReplay("轨迹已清除");
        m_playBtn->setEnabled(false);
        updateStats();
    });

    m_replayTimer = new QTimer(this);
    m_replayTimer->setInterval(kReplayMs);
    connect(m_replayTimer, &QTimer::timeout, this, &PathPage::replayTick);

    m_startMs = QDateTime::currentMSecsSinceEpoch();
    updateStats();
}

void PathPage::updateYaw(double yawDeg)
{
    m_yawRaw = qDegreesToRadians(yawDeg);
    m_yaw = m_yawRaw - m_yawRef;
}

void PathPage::updateMotion(int rpmL, int rpmR, int odomL, int odomR,
                            int dispL, int dispR, bool haveDisp)
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();

    /* ── 以下是实时推算，只用于画蓝色参考线和统计显示 ── */

    /* 累计行驶里程：直接用固件的绝对值里程，这是它本来的用途 */
    int odo = (odomL + odomR) / 2;          /* cm */
    if (m_havePrev) {
        int d = odo - m_prevOdo;
        /* 里程只增不减，变小说明下位机重启了，跳过这一拍 */
        if (d > 0 && d <= 500) m_dist += d / 100.0;
    }
    m_prevOdo = odo;

    if (!m_havePrev) { m_havePrev = true; m_prevMs = now; updateStats(); return; }

    /* 实际帧间隔。0x102 名义 5Hz，但下位机主循环会被超声波/DHT11 的
       阻塞读拖慢，实测周期并不稳定，所以用到达时刻算而不是写死 200ms。
       上限 1s：丢帧或页面刚切回来时，用一个陈旧的 dt 会凭空积出一大段位移。 */
    double dt = (now - m_prevMs) / 1000.0;
    m_prevMs = now;
    if (dt <= 0.0 || dt > 1.0) dt = 0.0;

    /* 位移优先用固件累加的**有符号位移**（0x103，协议 v2.6）。
       它在下位机里按每个编码器计数累加，不漏任何一拍；
       而这里能看到的转速是 5Hz 采样出来的，两次采样之间的加减速
       完全看不见——匀速直线时两者差不多，频繁起停和转弯时差得明显。

       没有 0x103（老固件）时退回转速积分。两条路都**不能用里程**：
       固件里程是绝对值累加的，原地转向时左右都在变大，平均下来
       ds>0，每转一次弯轨迹就凭空往前窜一段。 */
    double ds;
    if (haveDisp) {
        if (!m_haveDisp) {          /* 第一帧只取基准，不产生位移 */
            m_haveDisp = true;
            m_prevDispL = dispL; m_prevDispR = dispR;
            updateStats();
            return;
        }
        double dL = (dispL - m_prevDispL) / 1000.0;   /* mm -> m */
        double dR = (dispR - m_prevDispR) / 1000.0;
        m_prevDispL = dispL; m_prevDispR = dispR;
        /* 单帧位移超过 1 米说明下位机重启了（位移从 0 重新开始），
           跳过这一拍，否则会算出一大段假位移把轨迹甩出画布。 */
        if (dL > 1.0 || dL < -1.0 || dR > 1.0 || dR < -1.0) { updateStats(); return; }
        ds = (dL + dR) / 2.0;
    } else {
        double vL = rpmL / 60.0 * kWheelCircM;  /* m/s */
        double vR = rpmR / 60.0 * kWheelCircM;
        ds = (vL + vR) / 2.0 * dt;              /* 米，可正可负（倒车） */
    }

    if (ds != 0.0) {
        m_x += ds * qCos(m_yaw);
        m_y += ds * qSin(m_yaw);
        m_view->setPose(m_x, m_y, m_yaw);
    }
    updateStats();
}

/* 把位姿归零，并把"当前朝向"当作新的零度基准。
   录制和回放各自从原点、朝向 0° 起画，两条线才叠得到一起比对；
   不归零的话回放画出来的是接着上次终点、歪着一个角度的另一条线，
   看着像"完全不一样的路线"，其实只是起点和朝向不同。 */
void PathPage::resetPose()
{
    m_x = m_y = 0;
    m_yawRef = m_yawRaw;
    m_yaw = 0;
    m_view->clearLive();
    m_view->setPose(0, 0, 0);
}

/* ============ 指令流录制 ============ */
void PathPage::onCmdEcho(qint64 ts, int dev, int act, int p1, int p2)
{
    if (dev != kDevMotor || !m_recording) return;
    /* 回放时自己发的指令也会被网关回显回来。不挡掉的话
       "边回放边录制"会把回放指令再录一遍，越录越长。 */
    if (m_replaying) return;

    if (m_segs.isEmpty()) {
        /* 第一条指令就是计时零点。录制按钮按下到真正开车之间的等待
           不该算进去——回放时那段会变成干等。 */
        m_recT0 = ts;
        if (act == kActOff) return;        /* 开头的"停止"没有意义 */
    }
    CmdSeg g; g.ts = ts - m_recT0; g.act = act; g.p1 = p1; g.p2 = p2;
    m_segs.append(g);
    updateStats();
}

/* 把一条指令翻译成左右轮速度百分比 + 它自带的时长(0=持续到下一条)。
   只用于**画图**，回放不走这条路——回放是原样重发指令。 */
static void segToWheels(int act, int p1, int p2, double *pctL, double *pctR, int *selfMs)
{
    *pctL = *pctR = 0; *selfMs = 0;
    switch (act) {
    case kActOn: case kActSpeed: *pctL = *pctR =  p1; break;
    case kActBack:               *pctL = *pctR = -p1; break;
    case kActTank:               *pctL = p1 - kTankBias; *pctR = p2 - kTankBias; break;
    /* 左右转是固件自计时的：p1=速度 p2=毫秒，转完自己停 */
    case kActLeft:  *pctL = -p1; *pctR =  p1; *selfMs = p2; break;
    case kActRight: *pctL =  p1; *pctR = -p1; *selfMs = p2; break;
    default: break;                        /* OFF / ESTOP 等：0 */
    }
}

/* 把指令流按差速运动学积分成一条标称路线。
   注意这条线画的是"照着这串指令走会走成什么样"，
   不是"当时真的走成了什么样"——两者的差别就是这个方案的误差。 */
void PathPage::rebuildNominal()
{
    QVector<QPointF> pts;
    double x = 0, y = 0, th = 0;
    pts.append(QPointF(0, 0));

    for (int i = 0; i < m_segs.size(); i++) {
        const CmdSeg &g = m_segs.at(i);
        double pl, pr; int selfMs;
        segToWheels(g.act, g.p1, g.p2, &pl, &pr, &selfMs);

        /* 这条指令生效多久：自计时的按它自己的时长，
           其余的持续到下一条指令为止。最后一条按 1 秒收尾。 */
        qint64 span = (i + 1 < m_segs.size()) ? (m_segs.at(i + 1).ts - g.ts) : 1000;
        qint64 dur  = selfMs ? qMin<qint64>(selfMs, span) : span;
        if (dur <= 0) continue;

        double vL = pl / 100.0 * kMaxRpm / 60.0 * kWheelCircM;   /* m/s */
        double vR = pr / 100.0 * kMaxRpm / 60.0 * kWheelCircM;
        double v  = (vL + vR) / 2.0;
        double w  = (vR - vL) / kTrackM;                         /* rad/s */

        /* 分成 20ms 的小步长积分：一整段直接算的话，
           转弯段会被当成直线，弧线变成折线。 */
        int steps = qMax(1, int(dur / 20));
        double dt = dur / 1000.0 / steps;
        for (int k = 0; k < steps; k++) {
            th += w * dt;
            x  += v * qCos(th) * dt;
            y  += v * qSin(th) * dt;
        }
        /* 自计时指令走完后，剩下的时间车是停着的，不用积分 */
        pts.append(QPointF(x, y));
    }
    m_view->setRecorded(pts);
}

void PathPage::startReplay()
{
    if (m_segs.isEmpty()) return;
    qDebug("[path] 开始回放，%d 条指令", m_segs.size());
    /* 回放画一条新的线：从原点、朝向 0° 起，和灰色标称路线同起点，
       走完一眼就能看出偏了多少。 */
    resetPose();
    m_view->setLiveColor(Ios::green());
    m_replaying = true;
    m_replayT0 = QDateTime::currentMSecsSinceEpoch();
    m_playIdx = 0;
    m_sentIdx = -1;
    m_replayTimer->start();
    m_playBtn->setText("停止回放");
    m_recBtn->setEnabled(false);
    updateStats();
}

void PathPage::stopReplay(const QString &why)
{
    if (!m_replaying) return;
    qDebug("[path] 停止回放：%s", qPrintable(why));
    m_replaying = false;
    m_replayTimer->stop();
    m_view->setLiveColor(Ios::blue());
    /* 一定要显式发停止。靠固件的 500ms 超时兜底也能停，
       但那半秒车还在往前冲——回放结束时车正好压在终点上，
       多冲半秒可能就撞上去了。 */
    emit cmdRequested(kDevMotor, kActOff, 0, 0);
    m_playBtn->setText("回放路线");
    m_recBtn->setEnabled(true);
    m_hint->setText(QString("回放结束：%1").arg(why));
    updateStats();
}

/* 每 100ms 一拍。做两件事：
     ① 时候到了就把下一条指令原样发下去；
     ② 对 ACT_TANK 做保活重发。
   ①用**绝对时间轴**判定，而不是"发完一条等固定间隔发下一条"——
   后者会把每一拍的定时器误差累加进去，几十条之后整条路线就被拉长了。 */
void PathPage::replayTick()
{
    qint64 el = QDateTime::currentMSecsSinceEpoch() - m_replayT0;

    while (m_playIdx < m_segs.size() && m_segs.at(m_playIdx).ts <= el) {
        const CmdSeg &g = m_segs.at(m_playIdx);
        emit cmdRequested(kDevMotor, g.act, g.p1, g.p2);
        m_sentIdx = m_playIdx;
        m_playIdx++;
    }

    /* 保活：ACT_TANK 在固件里有 500ms 超时，一条只发一次的话
       车走半秒就被当成"上位机挂了"而停下。
       ACT_ON/BACK/SPEED 设的是持续目标转速，没有超时，不用重发；
       ACT_LEFT/RIGHT 是固件自计时的，重发反而会把转向时间续上一截。 */
    if (m_sentIdx >= 0 && m_segs.at(m_sentIdx).act == kActTank) {
        const CmdSeg &g = m_segs.at(m_sentIdx);
        emit cmdRequested(kDevMotor, g.act, g.p1, g.p2);
    }

    if (m_playIdx >= m_segs.size()) {
        /* 最后一条指令还要让它跑完它自己的时长再收尾 */
        const CmdSeg &last = m_segs.last();
        double pl, pr; int selfMs;
        segToWheels(last.act, last.p1, last.p2, &pl, &pr, &selfMs);
        qint64 tail = selfMs ? selfMs : 1000;
        if (el >= last.ts + tail)
            stopReplay(QString("共 %1 条指令，%2 秒")
                       .arg(m_segs.size()).arg((last.ts + tail) / 1000.0, 0, 'f', 1));
    }
    updateStats();
}

void PathPage::updateStats()
{
    qint64 total = m_segs.isEmpty() ? 0 : m_segs.last().ts;

    QString act;
    if (m_recording)
        act = QString("录制中 · %1 条 / %2 s").arg(m_segs.size()).arg(total / 1000.0, 0, 'f', 1);
    else if (m_replaying)
        act = QString("回放中 · 第 %1 / %2 条").arg(qMin(m_playIdx + 1, m_segs.size())).arg(m_segs.size());
    else if (m_segs.isEmpty())
        act = "未录制";
    else
        act = QString("已录 %1 条 / %2 s").arg(m_segs.size()).arg(total / 1000.0, 0, 'f', 1);

    QString cur = "—";
    int i = m_replaying ? m_sentIdx : (m_segs.isEmpty() ? -1 : m_segs.size() - 1);
    if (i >= 0 && i < m_segs.size()) {
        const CmdSeg &g = m_segs.at(i);
        const char *n = "?";
        switch (g.act) {
        case kActOn:    n = "前进"; break;
        case kActOff:   n = "停止"; break;
        case kActBack:  n = "后退"; break;
        case kActLeft:  n = "左转"; break;
        case kActRight: n = "右转"; break;
        case kActSpeed: n = "调速"; break;
        case kActTank:  n = "差速"; break;
        default: break;
        }
        if (g.act == kActTank)
            cur = QString("%1  L %2%  R %3%").arg(n).arg(g.p1 - kTankBias).arg(g.p2 - kTankBias);
        else if (g.act == kActLeft || g.act == kActRight)
            cur = QString("%1  %2%  %3 ms").arg(n).arg(g.p1).arg(g.p2);
        else
            cur = QString("%1  %2%").arg(n).arg(g.p1);
    }

    m_stats->setText(QString(
        "指令流  %1\n"
        "当前     %2\n"
        "\n"
        "推算位置  X %3 m   Y %4 m\n"
        "航向  %5°\n"
        "累计行驶  %6 m")
        .arg(act).arg(cur)
        .arg(m_x, 0, 'f', 2).arg(m_y, 0, 'f', 2)
        .arg(qRadiansToDegrees(m_yaw), 0, 'f', 1)
        .arg(m_dist, 0, 'f', 2));
}

#include "pathpage.moc"
