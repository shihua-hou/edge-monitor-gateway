#include "naviconbutton.h"
#include "iostheme.h"
#include <QPainter>
#include <QSizePolicy>
#include <QPainterPath>
#include <cmath>

NavIconButton::NavIconButton(IconType type, const QString &label, QWidget *parent)
    : QAbstractButton(parent), m_type(type), m_label(label)
{
    setCheckable(true);
    /* QPushButton 的默认垂直策略是 **Fixed**，而 Fixed 的含义是
       "高度就按 sizeHint 来"——Qt 连 minimumSizeHint() 都不会问。
       所以上面那个 minimumSizeHint 覆盖要配上这一行才有意义：
       改成 Preferred 之后，宽裕时按 46px，紧张时才压到 34px。
       （踩过：只改 minimumSizeHint 不改策略，导航栏纹丝不动，
         日志里算出来还是 7×46。） */
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setCursor(Qt::PointingHandCursor);
}

/* **最小高度要比期望高度小得多。**
 *
 * 期望 46px（iOS 列表行高、也是 HIG 的最小可靠点按尺寸），但那是
 * "版面宽裕时该多高"，不是"最少要多高"。
 *
 * 七个导航项 × 46 + 标题 + 状态行 ≈ 420px，而软键盘是布局成员、
 * 自己要 204px——加起来 624px，超过 600px 的屏高。全屏窗口长不高，
 * Qt 只能违反最小尺寸去裁，挨裁的是排在最后的键盘：
 * 现象是"输入法只显示出两行"，完全看不出跟导航栏有什么关系。
 *
 * 给一个小的 minimumSizeHint，导航栏就能在键盘弹出时自己压扁，
 * 收起后再回到 46px。
 */
QSize NavIconButton::minimumSizeHint() const
{
    return QSize(120, 34);
}

QSize NavIconButton::sizeHint() const
{
    /* 44px 是 iOS 的标准列表行高，也是 Apple HIG 里最小可靠点按目标的尺寸。
       车载场景手会抖、还戴手套，比手机更需要守住这个下限，所以给到 46。 */
    return QSize(160, 46);
}

void NavIconButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    /* iPadOS 侧边栏行：[图标] [文字]，左对齐。
       不用手机版的"图标在上、文字在下"：那是底部 Tab Bar 的范式，
       而这块屏是 1024x600 横屏，底部 Tab 会吃掉本就不多的高度。 */
    QRectF r = QRectF(rect()).adjusted(0, 1, 0, -1);

    /* 选中态：蓝色半透明卡片。用 squircle 而不是 drawRoundedRect——
       侧边栏的选中块是整个界面里最常被看到的圆角，
       这里用正圆弧的话，“不太像 iOS”的感觉会一直在。 */
    /* 选中背景不在这里画了——改由 MainWindow 里一个单独的滑块控件
       带弹簧动画滑过去。每个按钮各自画的话，切页只能是"这个灭、
       那个亮"的瞬变；iOS 的侧边栏选中块是**滑过去**的。 */
    if (isDown() && !isChecked()) {
        p.fillPath(Ios::squircle(r, 10), QColor(255, 255, 255, 20));
    }

    const QColor fg  = isChecked() ? Ios::blue() : Ios::labelSecondary();
    const QColor txt = isChecked() ? Ios::label() : Ios::labelSecondary();

    QRectF iconRect(r.left() + 12, r.center().y() - 11, 22, 22);
    drawIcon(p, iconRect, fg);

    QRectF textRect(r.left() + 44, r.top(), r.width() - 52, r.height());
    p.setFont(isChecked() ? Ios::fontHeadline() : Ios::fontBody());
    p.setPen(txt);
    p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, m_label);
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
    case Map: {
        /* 折叠地图：三段梯形 + 两条折痕。
           和"路线"那个图标要能一眼分开——那个画的是轨迹（蛇行线+起终点），
           这个画的是**底图本身**，所以用纸质地图的外形。 */
        double w = rc.width(), h = rc.height();
        double x0 = rc.left() + w * 0.08, y0 = rc.top() + h * 0.20;
        double x1 = rc.left() + w * 0.36, x2 = rc.left() + w * 0.64, x3 = rc.left() + w * 0.92;
        QPainterPath mp;
        mp.moveTo(x0, y0 + h * 0.10);
        mp.lineTo(x1, y0);
        mp.lineTo(x2, y0 + h * 0.10);
        mp.lineTo(x3, y0);
        mp.lineTo(x3, y0 + h * 0.52);
        mp.lineTo(x2, y0 + h * 0.62);
        mp.lineTo(x1, y0 + h * 0.52);
        mp.lineTo(x0, y0 + h * 0.62);
        mp.closeSubpath();
        p.drawPath(mp);
        p.drawLine(QPointF(x1, y0), QPointF(x1, y0 + h * 0.52));
        p.drawLine(QPointF(x2, y0 + h * 0.10), QPointF(x2, y0 + h * 0.62));
        break;
    }
    case Route: {
        /* 路径：一条蛇行折线 + 起终点。
           不用"地图图钉"那种图标——这一页是轨迹，不是定位。 */
        double w = rc.width(), h = rc.height();
        QPainterPath path;
        path.moveTo(rc.left() + w * 0.16, rc.top() + h * 0.80);
        path.cubicTo(rc.left() + w * 0.55, rc.top() + h * 0.72,
                     rc.left() + w * 0.10, rc.top() + h * 0.40,
                     rc.left() + w * 0.52, rc.top() + h * 0.28);
        p.drawPath(path);
        p.setBrush(color);
        p.drawEllipse(QPointF(rc.left() + w * 0.16, rc.top() + h * 0.80), w * 0.09, w * 0.09);
        p.drawEllipse(QPointF(rc.left() + w * 0.78, rc.top() + h * 0.24), w * 0.09, w * 0.09);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(rc.left() + w * 0.52, rc.top() + h * 0.28),
                   QPointF(rc.left() + w * 0.78, rc.top() + h * 0.24));
        break;
    }
    case Vehicle: {
        /* 侧面小车：车身圆角矩形 + 两个轮。
           和其它图标一样全矢量绘制，不依赖字体字形。 */
        double w = rc.width(), h = rc.height();
        QRectF body(rc.left() + w * 0.08, rc.top() + h * 0.34, w * 0.84, h * 0.30);
        p.drawRoundedRect(body, w * 0.10, w * 0.10);
        /* 车顶 */
        QPainterPath roof;
        roof.moveTo(rc.left() + w * 0.26, rc.top() + h * 0.34);
        roof.lineTo(rc.left() + w * 0.36, rc.top() + h * 0.16);
        roof.lineTo(rc.left() + w * 0.66, rc.top() + h * 0.16);
        roof.lineTo(rc.left() + w * 0.76, rc.top() + h * 0.34);
        p.drawPath(roof);
        double wr = w * 0.11;
        p.drawEllipse(QPointF(rc.left() + w * 0.28, rc.top() + h * 0.70), wr, wr);
        p.drawEllipse(QPointF(rc.left() + w * 0.72, rc.top() + h * 0.70), wr, wr);
        break;
    }
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
