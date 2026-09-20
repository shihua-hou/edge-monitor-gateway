#include "iosswitch.h"
#include "iostheme.h"
#include <QPainter>
#include <QPropertyAnimation>

/* iOS 标准尺寸，别改比例 */
static const int kW = 51, kH = 31, kKnob = 27;

IosSwitch::IosSwitch(QWidget *parent)
    : QAbstractButton(parent), m_knob(0.0), m_anim(nullptr)
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFixedSize(kW, kH);
    m_anim = new QPropertyAnimation(this, "knob", this);
    m_anim->setDuration(Ios::durFast());
    /* 用 OutBack（带回弹）而不是线性/OutCubic：iOS 的开关末尾有一下"顶住"的
       感觉，正是这点过冲。没有它，动画再顺滑也只是"移动"，不是"iOS 的移动"。 */
    m_anim->setEasingCurve(Ios::spring());
}

QSize IosSwitch::sizeHint() const { return QSize(kW, kH); }

void IosSwitch::setKnob(qreal v) { m_knob = v; update(); }

void IosSwitch::animateTo(bool on)
{
    m_anim->stop();
    m_anim->setStartValue(m_knob);
    m_anim->setEndValue(on ? 1.0 : 0.0);
    m_anim->start();
}

void IosSwitch::checkStateSet() { animateTo(isChecked()); }

void IosSwitch::nextCheckState()
{
    setChecked(!isChecked());     /* 会触发 checkStateSet -> 动画 */
    emit clicked(isChecked());
}

void IosSwitch::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    /* 轨道颜色随旋钮位置插值，而不是到位才变色——
       变色和位移同步，动画才是一个整体。 */
    QColor off(0x78, 0x78, 0x80, 100);
    QColor on = Ios::green();
    QColor track(
        int(off.red()   + (on.red()   - off.red())   * m_knob),
        int(off.green() + (on.green() - off.green()) * m_knob),
        int(off.blue()  + (on.blue()  - off.blue())  * m_knob),
        int(off.alpha() + (255        - off.alpha()) * m_knob));

    QRectF r(0.5, 0.5, kW - 1, kH - 1);
    p.setPen(Qt::NoPen);
    p.setBrush(track);
    p.drawRoundedRect(r, kH / 2.0, kH / 2.0);   /* 胶囊形，这里就是正圆弧，不用 squircle */

    /* 旋钮：纯白，带一圈极淡描边当作阴影的替代
       （软件渲染下真阴影太贵，而白色旋钮在绿/灰轨道上本来对比就够） */
    qreal travel = kW - kKnob - 4;
    QRectF k(2 + travel * m_knob, 2, kKnob, kKnob);
    p.setBrush(Qt::white);
    p.drawEllipse(k);
    p.setPen(QPen(QColor(0, 0, 0, 30), 1));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(k);
}
