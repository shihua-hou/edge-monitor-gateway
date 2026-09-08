#ifndef VIRTUALKEYBOARD_H
#define VIRTUALKEYBOARD_H

#include <QWidget>
#include <QPointer>

QT_BEGIN_NAMESPACE
class QVBoxLayout;
class QLineEdit;
QT_END_NAMESPACE

/* VirtualKeyboard - 触摸屏软键盘。
 *
 * 为什么要自己写一个：
 *  1) 板子内核（4.1.15）没有编 uinput 模块，`modprobe uinput` 直接 FATAL，
 *     所以"从远程网页注入按键事件到 /dev/input"这条路走不通——那条路本来
 *     是更省事的（能直接用 PC 的物理键盘打字），但没有内核支持就是没有。
 *  2) Qt 官方的 qtvirtualkeyboard 是单独的模块，NXP 的 meta-toolchain-qt5
 *     SDK 里通常不带，为了一个软键盘去改 Yocto 配方重编 SDK 不值当。
 *
 * 所以这里用最朴素的方式：一堆 QPushButton + 往当前焦点控件发 QKeyEvent。
 * 没有任何额外依赖，Qt 5.9 起都能编。
 *
 * 两个关键设计：
 *
 *  - 所有键帽都是 Qt::NoFocus。这是整个类能成立的前提：按钮默认会在点击时
 *    抢走焦点，那样输入框一失焦，"当前焦点控件"就变成了键帽自己，按键发给
 *    谁都不对。设成 NoFocus 之后焦点自始至终留在 QLineEdit 上。
 *
 *  - 这个部件是被放进布局里的普通成员，不是悬浮在页面之上的 overlay。
 *    弹出时布局自然把上面的内容压扁——聊天页的输入框会跟着上移，设置页的
 *    QScrollArea 会缩短。overlay 的做法要额外算"焦点框会不会被盖住"再手动
 *    滚动，在两个结构完全不同的页面上都做对很麻烦，交给布局管更省事也更稳。
 *
 * 自动弹出/收起靠一个应用级事件过滤器：任何 QLineEdit 获得焦点就弹出，
 * 焦点离开输入框就收起。页面代码完全不用改。 */
class VirtualKeyboard : public QWidget
{
    Q_OBJECT
public:
    explicit VirtualKeyboard(QWidget *parent = nullptr);

    /* 装到 QApplication 上，开始自动跟随输入框的焦点。
       单独一个方法而不是塞进构造函数：构造时机在 buildUi 里，而事件过滤器
       最好等界面搭完再装，避免初始化过程中的焦点变化误触发弹出 */
    void attachToApplication();

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    enum Layer { Lower, Upper, Symbol };

    void buildRows();
    void rebuildKeys();
    void addRow(const QStringList &keys);
    QWidget *makeKey(const QString &label, int stretch = 1);
    void onKeyPressed(const QString &label);
    void sendKey(int key, const QString &text);
    void setLayer(Layer l);

    QVBoxLayout *m_rows;
    Layer m_layer = Lower;
    QPointer<QWidget> m_target;   // 当前正在输入的控件；QPointer 防止页面析构后变野指针
};

#endif // VIRTUALKEYBOARD_H
