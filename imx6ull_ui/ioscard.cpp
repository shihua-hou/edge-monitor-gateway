#include "ioscard.h"
#include "iostheme.h"
#include <QPainter>

IosCard::IosCard(QWidget *parent, qreal radius)
    : QWidget(parent), m_radius(radius), m_pressed(false)
{
    /* 卡片自己画背景，必须让 Qt 知道它不透明区域由自己负责，
       否则父窗口的背景不会被正确重绘，滚动时会拖出残影。 */
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void IosCard::setRadius(qreal r) { m_radius = r; update(); }

void IosCard::setPressedLook(bool on)
{
    if (m_pressed == on) return;
    m_pressed = on;
    update();
}

void IosCard::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    /* 内缩 0.5px：1px 描边画在整数坐标上会跨两个像素、看起来发虚，
       半像素偏移之后描边正好落在一个像素里。 */
    Ios::paintCard(&p, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), m_radius, m_pressed);
}
