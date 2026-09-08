#include "micbutton.h"
#include <QPainter>

MicButton::MicButton(QWidget *parent) : QAbstractButton(parent)
{
    /* 56 -> 48：底部这一行的高度由麦克风按钮决定，而聊天页在 600px 高的
       屏幕上原本就差几十像素放不下（见 chatpage.cpp 里的说明）。
       48px 仍然远大于触摸操作的舒适下限（约 44px） */
    setFixedSize(48, 48);
    setCursor(Qt::PointingHandCursor);
}

QSize MicButton::sizeHint() const
{
    /* 跟 setFixedSize 保持一致。固定尺寸下 sizeHint 不会被采纳，
       但留一个对不上的值是给以后埋雷——哪天去掉 setFixedSize，
       按钮会突然变回 56 而布局又放不下 */
    return QSize(48, 48);
}

void MicButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QColor bg = isDown() ? QColor("#2fb87e") : QColor("#3ddc97");
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawEllipse(rect());

    /* 深色描边麦克风图标（跟亮绿色背景对比），画法跟导航栏 Voice 图标
       一致：圆角矩形头 + 弧形支架 + 竖杆 + 底座 */
    QColor fg("#04140d");
    QPen pen(fg);
    pen.setWidthF(2.2);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    /* 图标区按【比例】算，不要写死内边距。
     *
     * 原来是 rect().adjusted(18, 13, -18, -13)，那是照 56x56 配的：
     * 图标区 20x30。后来为了让聊天页在 600px 屏高上放得下，把按钮改成
     * 48x48，而这两个常数没跟着改——图标区变成 12x22，宽度缩了 40%，
     * 麦克风被挤扁了。
     *
     * 写死的内边距和尺寸是【隐式耦合】：改一个必须记得改另一个，
     * 而编译器不会提醒。按比例算之后，尺寸怎么变图标都是对的。
     * 比例取自原始设计：18/56≈0.32、13/56≈0.23、20/56≈0.36、30/56≈0.54 */
    const double w = width(), h = height();
    QRectF rc(w * 0.32, h * 0.23, w * 0.36, h * 0.54);

    QRectF head(rc.center().x() - rc.width() * 0.22, rc.top(), rc.width() * 0.44, rc.height() * 0.5);
    p.drawRoundedRect(head, head.width() / 2, head.width() / 2);

    QRectF arcRect(rc.left(), rc.top() + rc.height() * 0.12, rc.width(), rc.height() * 0.62);
    p.drawArc(arcRect, 200 * 16, 140 * 16);

    double stemTop = rc.top() + rc.height() * 0.68;
    p.drawLine(QPointF(rc.center().x(), stemTop), QPointF(rc.center().x(), rc.bottom()));
    p.drawLine(QPointF(rc.center().x() - rc.width() * 0.24, rc.bottom()),
               QPointF(rc.center().x() + rc.width() * 0.24, rc.bottom()));
}
