#include "attitudewidget.h"
#include <QPainter>
#include <QLinearGradient>
#include <QColor>
#include <QtMath>
#include <algorithm>

AttitudeWidget::AttitudeWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(160);
}

void AttitudeWidget::setAttitude(double pitchDeg, double rollDeg, double yawDeg)
{
    m_pitch = pitchDeg; m_roll = rollDeg; m_yaw = yawDeg;
    update();   // 触发重绘，不用每次都整窗口重排布局，比 QLabel 换文字/换图代价小
}

void AttitudeWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const int w = width(), h = height();
    p.fillRect(rect(), Qt::black);

    // ---- 旋转的天地球 ----
    p.save();
    p.translate(w / 2.0, h / 2.0);
    p.rotate(-m_roll);
    double clampedPitch = std::max(-45.0, std::min(45.0, m_pitch));
    p.translate(0, clampedPitch * (h / 90.0));

    // 画得比控件对角线还大，旋转之后也不会露出画布边缘的空白角
    double diag = std::sqrt(double(w) * w + double(h) * h) * 1.2;
    QRectF skyRect(-diag, -diag, diag * 2, diag);
    QRectF groundRect(-diag, 0, diag * 2, diag);
    QLinearGradient skyGrad(0, -diag, 0, 0);
    skyGrad.setColorAt(0, QColor("#173a6e")); skyGrad.setColorAt(1, QColor("#2f6fc4"));
    QLinearGradient groundGrad(0, 0, 0, diag);
    groundGrad.setColorAt(0, QColor("#7a5626")); groundGrad.setColorAt(1, QColor("#3a2a12"));
    p.fillRect(skyRect, skyGrad);
    p.fillRect(groundRect, groundGrad);
    p.setPen(QPen(QColor("#e6e6e6"), 2));
    p.drawLine(QPointF(-diag, 0), QPointF(diag, 0));   // 地平线本身
    p.restore();

    // ---- 固定不转的十字准心（代表设备本体朝向）----
    p.setPen(QPen(QColor("#3ddc97"), 2));
    p.drawLine(w / 2 - 34, h / 2, w / 2 - 10, h / 2);
    p.drawLine(w / 2 + 10, h / 2, w / 2 + 34, h / 2);
    p.drawLine(w / 2, h / 2 - 8, w / 2, h / 2 + 8);

    // ---- 右上角罗盘（yaw）----
    const int cx = w - 38, cy = 38, r = 24;
    p.setPen(QPen(QColor(255, 255, 255, 140), 1));
    p.setBrush(QColor(10, 14, 20, 160));
    p.drawEllipse(QPointF(cx, cy), r, r);
    p.save();
    p.translate(cx, cy);
    p.rotate(m_yaw);
    p.setPen(QPen(QColor("#ffb454"), 2));
    p.drawLine(0, 0, 0, -r + 6);
    p.restore();

    // ---- 数值读数 ----
    p.setPen(QColor("#cfe3ff"));
    QFont f = p.font(); f.setFamily("Consolas"); f.setPointSize(9); p.setFont(f);
    p.drawText(8, h - 8, QString("P %1°  R %2°  Y %3°")
               .arg(m_pitch, 0, 'f', 1).arg(m_roll, 0, 'f', 1).arg(m_yaw, 0, 'f', 1));

    /* ---- 失联蒙版 ----
       画在最后，盖住整个仪表。半透明而不是全黑：让人还能看见最后的姿态，
       但一眼就知道这是"冻结的画面"而不是实时姿态 */
    if (m_stale) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(10, 14, 20, 170));
        p.drawRect(0, 0, w, h);
        p.setPen(QColor("#ffb454"));
        QFont wf = p.font(); wf.setPointSize(11); wf.setBold(true); p.setFont(wf);
        p.drawText(QRect(0, 0, w, h), Qt::AlignCenter,
                   QString::fromUtf8("姿态传感器失联"));
    }
}

void AttitudeWidget::setStale(bool stale)
{
    if (m_stale == stale) return;
    m_stale = stale;
    update();
}
