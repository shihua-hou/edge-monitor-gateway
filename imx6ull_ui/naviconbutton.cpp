#include "naviconbutton.h"
#include <QPainter>
#include <QPainterPath>
#include <cmath>

static const QColor kColorNormal("#8a94a6");
static const QColor kColorChecked("#3ddc97");
static const QColor kColorBgChecked("#182131");
static const QColor kColorBgHover("#141b28");

NavIconButton::NavIconButton(IconType type, const QString &label, QWidget *parent)
    : QAbstractButton(parent), m_type(type), m_label(label)
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
}

QSize NavIconButton::sizeHint() const
{
    return QSize(64, 64);
}

void NavIconButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QRectF r = rect();

    /* 背景：选中/悬停两种状态，跟原来 QSS 里的配色保持一致 */
    if (isChecked()) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#182131"));
        p.drawRoundedRect(r, 12, 12);
    } else if (underMouse()) {
        p.setPen(Qt::NoPen);
        p.setBrush(kColorBgHover);
        p.drawRoundedRect(r, 12, 12);
    }

    QColor fg = isChecked() ? kColorChecked : kColorNormal;

    /* 图标区域：按钮上半部分，居中一个 24x24 的正方形画布 */
    QRectF iconRect(r.center().x() - 12, r.top() + 8, 24, 24);
    drawIcon(p, iconRect, fg);

    /* 文字标签：下半部分 */
    QRectF textRect(r.left(), r.top() + 36, r.width(), 18);
    QFont f = p.font();
    f.setPixelSize(12);
    p.setFont(f);
    p.setPen(fg);
    p.drawText(textRect, Qt::AlignHCenter | Qt::AlignVCenter, m_label);
}

void NavIconButton::drawIcon(QPainter &p, const QRectF &rc, const QColor &color) const
{
    p.save();
    QPen pen(color);
    pen.setWidthF(1.8);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    switch (m_type) {
    case Home: {
        /* 三角屋顶 + 矩形墙身，两笔画成，最经典的"首页"符号 */
        QPainterPath roof;
        roof.moveTo(rc.left(), rc.top() + rc.height() * 0.45);
        roof.lineTo(rc.center().x(), rc.top());
        roof.lineTo(rc.right(), rc.top() + rc.height() * 0.45);
        p.drawPath(roof);
        QRectF body(rc.left() + rc.width() * 0.18, rc.top() + rc.height() * 0.42,
                    rc.width() * 0.64, rc.height() * 0.58);
        p.drawRect(body);
        /* 门 */
        QRectF door(rc.center().x() - rc.width() * 0.1, rc.bottom() - rc.height() * 0.32,
                    rc.width() * 0.2, rc.height() * 0.32);
        p.drawRect(door);
        break;
    }
    case Control: {
        /* 三条横向调节杆，每条上一个不同位置的滑块——经典"控制面板"图标 */
        double ys[3] = { rc.top() + rc.height() * 0.15, rc.top() + rc.height() * 0.5, rc.top() + rc.height() * 0.85 };
        double knobX[3] = { rc.left() + rc.width() * 0.65, rc.left() + rc.width() * 0.3, rc.left() + rc.width() * 0.75 };
        for (int i = 0; i < 3; i++) {
            p.drawLine(QPointF(rc.left(), ys[i]), QPointF(rc.right(), ys[i]));
            p.setBrush(color);
            p.drawEllipse(QPointF(knobX[i], ys[i]), 2.6, 2.6);
            p.setBrush(Qt::NoBrush);
        }
        break;
    }
    case Voice: {
        /* 麦克风：圆角矩形头 + 弧形支架 + 竖杆 + 底座 */
        QRectF head(rc.center().x() - rc.width() * 0.16, rc.top(),
                    rc.width() * 0.32, rc.height() * 0.55);
        p.drawRoundedRect(head, head.width() / 2, head.width() / 2);

        QRectF arcRect(rc.left() + rc.width() * 0.08, rc.top() + rc.height() * 0.18,
                       rc.width() * 0.84, rc.height() * 0.6);
        p.drawArc(arcRect, 200 * 16, 140 * 16);

        double stemTop = rc.top() + rc.height() * 0.72;
        p.drawLine(QPointF(rc.center().x(), stemTop), QPointF(rc.center().x(), rc.bottom()));
        p.drawLine(QPointF(rc.center().x() - rc.width() * 0.22, rc.bottom()),
                   QPointF(rc.center().x() + rc.width() * 0.22, rc.bottom()));
        break;
    }
    case Settings: {
        /* 齿轮：中心圆环 + 均匀分布的 8 个短齿 */
        QPointF c = rc.center();
        double rOuter = rc.width() * 0.34;
        double rInner = rc.width() * 0.16;
        double toothLen = rc.width() * 0.14;
        p.drawEllipse(c, rInner, rInner);
        p.drawEllipse(c, rOuter * 0.62, rOuter * 0.62);
        for (int i = 0; i < 8; i++) {
            double a = i * (M_PI / 4.0);
            QPointF p1(c.x() + std::cos(a) * rOuter * 0.62, c.y() + std::sin(a) * rOuter * 0.62);
            QPointF p2(c.x() + std::cos(a) * (rOuter * 0.62 + toothLen),
                       c.y() + std::sin(a) * (rOuter * 0.62 + toothLen));
            p.drawLine(p1, p2);
        }
        break;
    }
    }

    p.restore();
}
