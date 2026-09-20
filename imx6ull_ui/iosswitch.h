#ifndef IOSSWITCH_H
#define IOSSWITCH_H

#include <QAbstractButton>

/* IosSwitch - iOS 那个绿色滑动开关。
 *
 * 为什么值得自己写一个，而不是用 QCheckBox 换个皮：
 * 这个控件是 iOS 观感里辨识度最高的一个。它的"对不对"不在颜色，
 * 而在**旋钮移动的手感**——iOS 用的是弹簧动画，末尾有一点点回弹，
 * 而 QCheckBox 无论怎么套 QSS 都是瞬间跳变。
 *
 * 尺寸照抄 iOS：轨道 51x31、旋钮直径 27。这个比例是苹果调了很多年的，
 * 自己改宽高比会立刻"不像"。
 */
class IosSwitch : public QAbstractButton
{
    Q_OBJECT
    /* 旋钮位置做成 Q_PROPERTY 才能被 QPropertyAnimation 驱动 */
    Q_PROPERTY(qreal knob READ knob WRITE setKnob)
public:
    explicit IosSwitch(QWidget *parent = nullptr);

    QSize sizeHint() const override;
    qreal knob() const { return m_knob; }
    void  setKnob(qreal v);

protected:
    void paintEvent(QPaintEvent *) override;
    void checkStateSet() override;       /* setChecked 时也要动画 */
    void nextCheckState() override;      /* 点击时 */

private:
    void animateTo(bool on);
    qreal m_knob;                        /* 0=左 1=右 */
    class QPropertyAnimation *m_anim;
};

#endif // IOSSWITCH_H
