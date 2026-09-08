#ifndef NAVICONBUTTON_H
#define NAVICONBUTTON_H

#include <QAbstractButton>

/* NavIconButton - 左侧导航栏用的图标按钮，图标完全用 QPainter 矢量绘制，
 * 不依赖任何字体字形或图片资源。
 *
 * 起因：原来导航栏用 Unicode 符号（⌂⚙🎙☰）当图标文字画，其中 🎙 是真彩色
 * emoji，嵌入式 Linux 系统的字体基本不带 emoji 字体集，显示成方块/空白；
 * 其余几个是普通符号，能不能显示也要看具体字体覆盖率，不稳妥。矢量绘制
 * 完全不依赖字体，在任何平台上效果都一致。 */
class NavIconButton : public QAbstractButton
{
    Q_OBJECT
public:
    enum IconType { Home, Control, Voice, Settings };

    explicit NavIconButton(IconType type, const QString &label, QWidget *parent = nullptr);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void drawIcon(class QPainter &p, const QRectF &rect, const QColor &color) const;

    IconType m_type;
    QString m_label;
};

#endif // NAVICONBUTTON_H
