#include "iostheme.h"
#include <QPainter>
#include <QLinearGradient>
#include <QFontDatabase>

namespace Ios {

/* 深色语义色。取值贴着 iOS 的 Dark Appearance，但整体再压暗一点点：
   这块 7 寸屏在车里，夜间纯黑底比 iOS 手机上的 #000 更容易反光看不清，
   所以底色用 #0B0B0F 而不是纯黑，卡片也相应提亮。 */
QColor bg()             { return QColor(0x0B, 0x0B, 0x0F); }
QColor bgElevated()     { return QColor(0x1C, 0x1C, 0x1E); }
QColor bgGrouped()      { return QColor(0x2C, 0x2C, 0x2E); }
QColor label()          { return QColor(0xFF, 0xFF, 0xFF); }
QColor labelSecondary() { return QColor(0xEB, 0xEB, 0xF5, 153); }  /* 60% */
QColor labelTertiary()  { return QColor(0xEB, 0xEB, 0xF5, 76);  }  /* 30% */
QColor separator()      { return QColor(0x54, 0x54, 0x58, 140); }
QColor fill()           { return QColor(0x78, 0x78, 0x80, 91);  }

QColor blue()           { return QColor(0x0A, 0x84, 0xFF); }
QColor green()          { return QColor(0x30, 0xD1, 0x58); }
QColor red()            { return QColor(0xFF, 0x45, 0x3A); }
QColor orange()         { return QColor(0xFF, 0x9F, 0x0A); }
QColor gray()           { return QColor(0x8E, 0x8E, 0x93); }

/* 字体族统一走这里。板子上装的是鸿蒙/思源一类的中文字体，
   英文数字的字形跟 SF 不同，但字重和字号节奏才是层级感的主要来源。 */
static QString family()
{
    static QString f;
    if (f.isEmpty()) {
        const char *want[] = { "HarmonyOS Sans SC", "Source Han Sans SC",
                               "Noto Sans CJK SC", "PingFang SC",
                               "WenQuanYi Micro Hei", 0 };
        QStringList have = QFontDatabase().families();
        for (int i = 0; want[i]; i++) {
            if (have.contains(QString::fromUtf8(want[i]))) { f = QString::fromUtf8(want[i]); break; }
        }
        if (f.isEmpty()) f = QFont().family();
    }
    return f;
}

static QFont mk(int px, int weight)
{
    QFont f(family());
    f.setPixelSize(px);          /* 用 px 不用 pt：这块屏 DPI 是固定的，
                                    pt 会被 Qt 按 DPI 再换算一次，不可控 */
    f.setWeight(weight);
    return f;
}

QFont fontLargeTitle() { return mk(34, QFont::Bold); }
QFont fontTitle()      { return mk(22, QFont::DemiBold); }
QFont fontHeadline()   { return mk(17, QFont::DemiBold); }
QFont fontBody()       { return mk(17, QFont::Normal); }
QFont fontFootnote()   { return mk(13, QFont::Normal); }
QFont fontCaption()    { return mk(11, QFont::Normal); }

QFont fontNumber(int px, bool bold)
{
    QFont f(family());
    f.setPixelSize(px);
    f.setWeight(bold ? QFont::Bold : QFont::Normal);
    /* 等宽数字：转速/电压这些值一直在变，不定宽的话整行会左右跳动。
       Qt5.12 有 QFont::setStyleHint 但对中文字体不一定生效，
       所以额外把数字间距固定住。 */
    f.setStyleHint(QFont::Monospace);
    f.setFixedPitch(true);
    return f;
}

/* ── squircle ──
 * 用三次贝塞尔逼近连续曲率圆角。控制点比例 0.5523 是标准的圆弧逼近常数
 * （4/3*(sqrt(2)-1)）；把它调大到约 0.72 之后，曲率过渡变缓，
 * 视觉上就接近 iOS 的连续圆角了。这个数是试出来的，不是推出来的——
 * 苹果没公开真正的曲线定义。 */
QPainterPath squircle(const QRectF &r, qreal radius)
{
    QPainterPath p;
    qreal w = r.width(), h = r.height();
    qreal rr = qMin(radius, qMin(w, h) / 2.0);
    if (rr <= 0.5) { p.addRect(r); return p; }

    const qreal k = rr * 0.72;      /* 控制点伸出量，越大越"绷" */
    qreal x = r.left(), y = r.top();

    p.moveTo(x + rr, y);
    p.lineTo(x + w - rr, y);
    p.cubicTo(x + w - rr + k, y,  x + w, y + rr - k,  x + w, y + rr);
    p.lineTo(x + w, y + h - rr);
    p.cubicTo(x + w, y + h - rr + k,  x + w - rr + k, y + h,  x + w - rr, y + h);
    p.lineTo(x + rr, y + h);
    p.cubicTo(x + rr - k, y + h,  x, y + h - rr + k,  x, y + h - rr);
    p.lineTo(x, y + rr);
    p.cubicTo(x, y + rr - k,  x + rr - k, y,  x + rr, y);
    p.closeSubpath();
    return p;
}

QEasingCurve spring()
{
    QEasingCurve c(QEasingCurve::OutBack);
    /* 默认 overshoot 是 1.70158，过冲很明显。车机上调到 1.1：
       能感觉到"弹"，但不至于晃眼。 */
    c.setOvershoot(1.1);
    return c;
}

int durFast()   { return 180; }
int durNormal() { return 280; }

/* ── 仿毛玻璃卡片 ──
 * 三层叠出来：
 *   1) 半透明底色      —— 让下层背景的明暗透一点上来
 *   2) 顶部 2px 渐变    —— 模拟材质上缘的受光；只做 2px 是因为
 *                          16bpp 下长渐变会有明显色带
 *   3) 1px 内描边      —— iOS 材质边缘那道极淡的亮线，层次全靠它
 * 没有阴影：QGraphicsDropShadowEffect 在软件渲染下同样昂贵，
 * 而且深色背景上阴影本来就几乎看不见，画了纯属浪费。 */
void paintCard(QPainter *p, const QRectF &r, qreal radius, bool pressed)
{
    QPainterPath path = squircle(r, radius);

    QColor base = bgElevated();
    base.setAlpha(pressed ? 255 : 235);
    p->fillPath(path, base);

    /* 顶部高光：只在最上面 2px，避免色带 */
    if (r.height() > 6) {
        QLinearGradient g(r.topLeft(), QPointF(r.left(), r.top() + 2));
        g.setColorAt(0, QColor(255, 255, 255, pressed ? 10 : 22));
        g.setColorAt(1, QColor(255, 255, 255, 0));
        p->save();
        p->setClipPath(path);
        p->fillRect(QRectF(r.left(), r.top(), r.width(), 2), g);
        p->restore();
    }

    p->setPen(QPen(QColor(255, 255, 255, pressed ? 12 : 20), 1));
    p->setBrush(Qt::NoBrush);
    p->drawPath(path);
}

void paintGroup(QPainter *p, const QRectF &r, qreal radius)
{
    QPainterPath path = squircle(r, radius);
    p->fillPath(path, bgElevated());
}

} // namespace Ios
