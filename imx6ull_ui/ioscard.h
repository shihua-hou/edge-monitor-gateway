#ifndef IOSCARD_H
#define IOSCARD_H

#include <QWidget>

/* IosCard - iOS 材质卡片。
 *
 * 为什么不直接用 QFrame + QSS 的 border-radius：
 * QSS 的圆角是**正圆弧**，而 iOS 用的是连续曲率圆角（squircle）。
 * 这个差别单看一个卡片说不清，但整屏十几个卡片一起看，正圆弧会显得"鼓"，
 * 那种"像素级不对劲但说不上哪里不对"的感觉基本就来自这里。
 * 所以卡片自己 paint，走 Ios::paintCard()。
 *
 * 另外 QSS 的 padding 对自绘背景不生效，所以内边距由使用方给 layout 设。
 */
class IosCard : public QWidget
{
    Q_OBJECT
public:
    explicit IosCard(QWidget *parent = nullptr, qreal radius = 14.0);

    void setRadius(qreal r);
    /* 按下态：卡片本身不可点时不用管；可点的卡片（比如控制页的按钮卡）
       靠它做视觉反馈——iOS 上"能点的东西按下去要有反应"是基本预期。 */
    void setPressedLook(bool on);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    qreal m_radius;
    bool  m_pressed;
};

#endif // IOSCARD_H
