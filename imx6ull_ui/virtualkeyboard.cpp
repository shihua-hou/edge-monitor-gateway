#include "virtualkeyboard.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLineEdit>
#include <QApplication>
#include <QKeyEvent>
#include <QEvent>

/* 特殊键用这几个名字标记，"标签即标识"，省掉再维护一张 label->keycode
   的映射表；这些字符串不会跟任何一个真实键帽冲突。

   全部用中文/ASCII，不用箭头、退格符那类符号字形：板子的字体集不带它们，
   会显示成方块——导航栏图标当初就踩过这个坑（见 naviconbutton.cpp 的注释，
   那次是麦克风 emoji 显示不出来）。中文一定有字形，界面上到处都在用。 */
static const char *K_SHIFT = "大写";
static const char *K_BACK  = "退格";
static const char *K_ENTER = "回车";
static const char *K_SPACE = "空格";
static const char *K_SYM   = "?123";
static const char *K_ABC   = "ABC";
static const char *K_HIDE  = "收起";

VirtualKeyboard::VirtualKeyboard(QWidget *parent)
    : QWidget(parent)
{
    setStyleSheet(
        "VirtualKeyboard { background:#0c111a; border-top:1px solid #1c2536; }"
        "QPushButton { background:#1a2332; color:#e6e6e6; border:1px solid #2a3648;"
        "  border-radius:6px; font-size:16px; padding:0; }"
        "QPushButton:pressed { background:#3ddc97; color:#04140d; }"
        "QPushButton[fn=\"1\"] { background:#141c2a; color:#8a94a6; font-size:14px; }"
        "QPushButton[fn=\"1\"]:pressed { background:#5aa9ff; color:#04140d; }"
    );

    m_rows = new QVBoxLayout(this);
    m_rows->setContentsMargins(4, 4, 4, 4);
    m_rows->setSpacing(4);
    buildRows();
    hide();
}

QWidget *VirtualKeyboard::makeKey(const QString &label, int stretch)
{
    QPushButton *b = new QPushButton(label, this);
    /* 整个类的关键点：键帽绝不能接受焦点。默认的 StrongFocus 会让点击键帽
       时输入框失焦，之后按键就发不到输入框里了 */
    b->setFocusPolicy(Qt::NoFocus);
    /* 36px：五排键 + 间距 + 边距合计约 210px，在 600px 高的屏幕上给上面的
       页面内容留下将近 400px。再高就会把聊天记录区压得只剩一条缝 */
    b->setMinimumHeight(36);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    const bool fn = (label == K_SHIFT || label == K_BACK || label == K_ENTER ||
                     label == K_SYM   || label == K_ABC  || label == K_HIDE);
    if (fn) b->setProperty("fn", "1");

    connect(b, &QPushButton::clicked, this, [this, label]() { onKeyPressed(label); });
    Q_UNUSED(stretch)
    return b;
}

void VirtualKeyboard::addRow(const QStringList &keys)
{
    QHBoxLayout *row = new QHBoxLayout;
    row->setSpacing(5);
    for (const QString &k : keys) {
        QWidget *w = makeKey(k);
        /* 功能键给更大的伸缩因子，视觉上更宽——手指在 7 寸屏上点小键帽很容易点错，
           退格/回车这种点错代价大的键尤其要大一点 */
        int f = (k == QString(K_SPACE)) ? 5
              : (k == QString(K_BACK) || k == QString(K_ENTER) || k == QString(K_SHIFT)) ? 2 : 1;
        row->addWidget(w, f);
    }
    m_rows->addLayout(row);
}

void VirtualKeyboard::buildRows() { rebuildKeys(); }

void VirtualKeyboard::rebuildKeys()
{
    // 清空旧的行（切换大小写/符号层时整层重建，比逐个改按钮文字简单可靠）
    while (QLayoutItem *item = m_rows->takeAt(0)) {
        if (QLayout *sub = item->layout()) {
            while (QLayoutItem *ci = sub->takeAt(0)) {
                if (ci->widget()) ci->widget()->deleteLater();
                delete ci;
            }
            delete sub;
        }
        delete item;
    }

    if (m_layer == Symbol) {
        addRow({"1","2","3","4","5","6","7","8","9","0"});
        addRow({"+","/","=","_","-",":",".",",","@","#"});
        addRow({"(",")","[","]","{","}","<",">","%","&"});
        addRow({QString(K_ABC), "*", "!", "?", "\"", "'", ";", "$", QString(K_BACK)});
        addRow({QString(K_HIDE), QString(K_SPACE), QString(K_ENTER)});
    } else {
        const bool up = (m_layer == Upper);
        auto C = [up](const char *s) { return up ? QString(s).toUpper() : QString(s); };
        addRow({"1","2","3","4","5","6","7","8","9","0"});
        addRow({C("q"),C("w"),C("e"),C("r"),C("t"),C("y"),C("u"),C("i"),C("o"),C("p")});
        addRow({C("a"),C("s"),C("d"),C("f"),C("g"),C("h"),C("j"),C("k"),C("l")});
        addRow({QString(K_SHIFT),C("z"),C("x"),C("c"),C("v"),C("b"),C("n"),C("m"),QString(K_BACK)});
        addRow({QString(K_SYM), QString(K_HIDE), QString(K_SPACE), ".", QString(K_ENTER)});
    }
}

void VirtualKeyboard::setLayer(Layer l)
{
    if (m_layer == l) return;
    m_layer = l;
    rebuildKeys();
}

void VirtualKeyboard::sendKey(int key, const QString &text)
{
    QWidget *w = m_target;
    if (!w) return;
    /* 发合成的 QKeyEvent 而不是直接调 QLineEdit::insert()：这样任何接受键盘
       输入的控件都能用，不用给每种控件写一份特化。press + release 都要发，
       只发 press 有些控件的内部状态机会不平衡 */
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
    QApplication::sendEvent(w, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, text);
    QApplication::sendEvent(w, &release);
}

void VirtualKeyboard::onKeyPressed(const QString &label)
{
    if (label == K_SHIFT) { setLayer(m_layer == Upper ? Lower : Upper); return; }
    if (label == K_SYM)   { setLayer(Symbol); return; }
    if (label == K_ABC)   { setLayer(Lower);  return; }
    if (label == K_HIDE)  { if (m_target) m_target->clearFocus(); hide(); return; }
    if (label == K_BACK)  { sendKey(Qt::Key_Backspace, QString()); return; }
    if (label == K_SPACE) { sendKey(Qt::Key_Space, " "); return; }
    if (label == K_ENTER) {
        /* 回车照常发给输入框——聊天页靠 returnPressed 发送消息，这是那里
           唯一的发送方式，不能吞掉。发完顺手收起键盘 */
        sendKey(Qt::Key_Return, "\r");
        hide();
        return;
    }

    sendKey(Qt::Key_unknown, label);

    /* 上档只对一个字符生效，跟手机输入法一致：打完一个大写字母自动回到小写。
       密钥这种大小写混排的字符串，如果上档是"锁定"的，每打一个大写字母都要
       手动关一次，比自动回落更烦 */
    if (m_layer == Upper) setLayer(Lower);
}

void VirtualKeyboard::attachToApplication()
{
    qApp->installEventFilter(this);
}

bool VirtualKeyboard::eventFilter(QObject *obj, QEvent *ev)
{
    if (ev->type() == QEvent::FocusIn) {
        if (qobject_cast<QLineEdit *>(obj)) {
            m_target = static_cast<QWidget *>(obj);
            show();
        }
    } else if (ev->type() == QEvent::FocusOut) {
        /* 只在焦点确实离开了输入框时收起。键帽是 NoFocus 的，点它们不会
           产生 FocusOut，所以这里不会被"点一下键盘就把自己收起来"误伤 */
        if (obj == m_target) {
            m_target = nullptr;
            hide();
        }
    }
    return QWidget::eventFilter(obj, ev);   // 一律放行，纯观察不拦截
}
