#ifndef MICBUTTON_H
#define MICBUTTON_H

#include <QAbstractButton>

/* MicButton - 语音助手页的圆形麦克风按钮，图标矢量绘制（不用 🎙 emoji，
 * 嵌入式系统字体基本不带彩色 emoji 字体集，会显示成方块/空白）。
 * pressed/released 信号继承自 QAbstractButton，跟原来 QPushButton 的用法
 * 完全一样，调用方（ChatPage）不用改连接逻辑。 */
class MicButton : public QAbstractButton
{
    Q_OBJECT
public:
    explicit MicButton(QWidget *parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
};

#endif // MICBUTTON_H
