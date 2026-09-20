#include "notificationbanner.h"
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPropertyAnimation>
#include "iostheme.h"
#include <QTimer>
#include <QPainter>
#include <QPainterPath>

/* StatusIcon - 通知横幅左侧的级别图标，矢量绘制，不用 ℹ/⚠/🔴 这些 Unicode
 * 符号（尤其 🔴 是真彩色 emoji，嵌入式系统字体基本不带），三种级别分别画
 * 圆圈+i（info）、三角+感叹号（warn）、实心圆（danger），只在本文件内用。 */
class StatusIcon : public QWidget
{
public:
    explicit StatusIcon(QWidget *parent = nullptr) : QWidget(parent) { setFixedSize(20, 20); }
    void setLevel(const QString &level) { m_level = level; update(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QColor c = m_level == "danger" ? QColor("#ff5470")
                  : m_level == "warn"   ? QColor("#ffb454")
                                        : QColor("#5aa9ff");
        QRectF r = rect().adjusted(1, 1, -1, -1);

        if (m_level == "danger") {
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawEllipse(r);
            return;
        }

        QPen pen(c);
        pen.setWidthF(1.6);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);

        if (m_level == "warn") {
            QPainterPath tri;
            tri.moveTo(r.center().x(), r.top());
            tri.lineTo(r.right(), r.bottom());
            tri.lineTo(r.left(), r.bottom());
            tri.closeSubpath();
            p.drawPath(tri);
            p.drawLine(QPointF(r.center().x(), r.top() + r.height() * 0.4),
                       QPointF(r.center().x(), r.top() + r.height() * 0.68));
            QPen dotPen(c); dotPen.setWidthF(2.2); dotPen.setCapStyle(Qt::RoundCap);
            p.setPen(dotPen);
            p.drawPoint(QPointF(r.center().x(), r.bottom() - r.height() * 0.12));
        } else {
            p.drawEllipse(r);
            QPen dotPen(c); dotPen.setWidthF(2.2); dotPen.setCapStyle(Qt::RoundCap);
            p.setPen(dotPen);
            p.drawPoint(QPointF(r.center().x(), r.top() + r.height() * 0.28));
            QPen linePen(c); linePen.setWidthF(1.8); linePen.setCapStyle(Qt::RoundCap);
            p.setPen(linePen);
            p.drawLine(QPointF(r.center().x(), r.top() + r.height() * 0.46),
                       QPointF(r.center().x(), r.bottom() - r.height() * 0.22));
        }
    }

private:
    QString m_level = "info";
};

NotificationBanner::NotificationBanner(QWidget *parent) : QWidget(parent)
{
    setMaximumHeight(0);   // 默认收起，高度动画展开到 headerHeight 就是"滑下来"的效果
    /* iOS 通知卡：无描边、深灰材质、大圆角。
       原来那种"左侧一条粗色条"是 Android/Web 的 alert 语言，
       iOS 通知靠的是图标颜色 + 卡片本身，不画边框。 */
    setStyleSheet(QString("background:%1; border:none; border-radius:16px;")
                  .arg(QColor(0x2C,0x2C,0x2E).name()));

    QHBoxLayout *l = new QHBoxLayout(this);
    l->setContentsMargins(14, 10, 14, 10);
    m_icon = new StatusIcon(this);
    QWidget *textCol = new QWidget(this);
    QVBoxLayout *textLayout = new QVBoxLayout(textCol);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(2);
    m_title = new QLabel(this);
    m_title->setFont(Ios::fontHeadline());
    m_title->setStyleSheet(QString("color:%1;").arg(Ios::label().name()));
    m_msg = new QLabel(this);
    m_msg->setFont(Ios::fontFootnote());
    m_msg->setStyleSheet(QString("color:%1;").arg(Ios::labelSecondary().name(QColor::HexArgb)));
    textLayout->addWidget(m_title);
    textLayout->addWidget(m_msg);
    l->addWidget(m_icon);
    l->addWidget(textCol, 1);

    m_anim = new QPropertyAnimation(this, "maximumHeight", this);
    m_anim->setDuration(Ios::durNormal());
    /* 弹簧曲线：通知滑下来时末尾有一下很轻的回弹，
       这是 iOS 通知最标志性的手感。线性滑下来会显得"硬"。 */
    m_anim->setEasingCurve(Ios::spring());

    m_dismissTimer = new QTimer(this);
    m_dismissTimer->setSingleShot(true);
    connect(m_dismissTimer, &QTimer::timeout, this, &NotificationBanner::dismissCurrent);
}

void NotificationBanner::show(const QString &title, const QString &msg, const QString &level)
{
    m_queue.enqueue({title, msg, level});
    if (!m_showing) showNext();
}

void NotificationBanner::showNext()
{
    if (m_queue.isEmpty()) { m_showing = false; return; }
    m_showing = true;
    QStringList item = m_queue.dequeue();
    QString title = item.value(0), msg = item.value(1), level = item.value(2);

    /* 级别只体现在左侧图标颜色上，卡片本身不变色。
       iOS 不用"整张卡变红"这种表达——那在深色主题下很吵，
       而且连着来几条告警时整屏都在闪。 */
    m_icon->setLevel(level);
    m_title->setText(title);
    m_msg->setText(msg);

    m_anim->stop();
    /* 必须先断开 finished 的连接再启动展开动画。
     *
     * dismissCurrent() 为了"收起动画放完再显示下一条"，会把 finished 接到
     * showNext 上。那条连接在进入 showNext 之后依然挂着——于是【展开】动画
     * 一结束（220ms）又会触发一次 showNext，把队列里的下一条立刻顶上来。
     *
     * 后果是：多条告警排队时，从第二条开始每条只显示 0.2 秒就被冲掉，
     * 根本来不及看。而告警恰恰是成批来的（传感器失联 + 阈值超标 + 命令未生效
     * 可能在同一秒内全部触发），最需要看清的时候反而看不清。
     *
     * 根子在于"一次性的回调用了持久连接"。这里显式断开，让展开动画的
     * finished 不接任何东西，节奏完全由 m_dismissTimer 控制。 */
    disconnect(m_anim, &QPropertyAnimation::finished, this, nullptr);
    m_anim->setStartValue(0);
    m_anim->setEndValue(64);
    m_anim->start();

    m_dismissTimer->start(level == "danger" ? 5000 : 3500);
}

void NotificationBanner::dismissCurrent()
{
    m_anim->stop();
    m_anim->setStartValue(maximumHeight());
    m_anim->setEndValue(0);
    disconnect(m_anim, &QPropertyAnimation::finished, this, nullptr);
    connect(m_anim, &QPropertyAnimation::finished, this, &NotificationBanner::showNext);
    m_anim->start();
}
