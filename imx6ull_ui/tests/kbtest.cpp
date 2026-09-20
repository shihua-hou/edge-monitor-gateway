/* kbtest - 软键盘的无屏回归测试
 *
 * 为什么需要它：板子的触摸注入（screen_share 的 /touch）会被 tslib 重映射，
 * 时灵时不灵，靠它验证界面行为非常不可靠。而键盘这类纯逻辑的东西
 * 完全可以在 -platform offscreen 下跑，不需要 framebuffer 也不需要触摸屏。
 *
 * 它已经抓到过一个真 bug：rebuildKeys() 里对嵌套布局既 delete sub 又
 * delete item，而 QLayout 继承自 QLayoutItem、两者是同一个对象——
 * 双重释放。原来只有按"大写""?123"才会走到，加了中英键后每次切换都崩。
 *
 * 编译（在 SDK 环境里，工程目录下）：
 *   . /opt/fsl-imx-x11/4.1.15-2.1.0/environment-setup-cortexa7hf-neon-poky-linux-gnueabi
 *   $CXX -std=gnu++11 -fPIC tests/kbtest.cpp virtualkeyboard.cpp pinyinime.cpp  *        moc_virtualkeyboard.cpp -o /tmp/kbtest  *        -I. -I$SDKTARGETSYSROOT/usr/include  *        -I$SDKTARGETSYSROOT/usr/include/QtCore  *        -I$SDKTARGETSYSROOT/usr/include/QtGui  *        -I$SDKTARGETSYSROOT/usr/include/QtWidgets  *        -L$SDKTARGETSYSROOT/usr/lib -lQt5Core -lQt5Gui -lQt5Widgets
 *   （moc_virtualkeyboard.cpp 由 qmake 构建时生成，直接复用）
 *
 * 在板子上跑：
 *   QT_QPA_PLATFORM=offscreen /tmp/kbtest
 *
 * 期望输出：OK0 是 61（英文 a），OK4 是 e4bda0e5a5bd（你好）。
 * 汉字一律打成 UTF-8 十六进制——板子的区域设置是 C，
 * 直接打中文会被 QTextStream 转成一串问号，看着像乱码其实是好的。
 */
#include "virtualkeyboard.h"
#include <QApplication>
#include <QLineEdit>
#include <QPushButton>
#include <QList>
#include <QDebug>
#include <QVBoxLayout>

static QPushButton *findKey(VirtualKeyboard *kb, const QString &text)
{
    QList<QPushButton *> bs = kb->findChildren<QPushButton *>();
    for (int i = 0; i < bs.size(); i++)
        if (bs[i]->text() == text) return bs[i];
    return nullptr;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    /* 按真实结构搭：一个窗口里 输入框 + 键盘（键盘是子控件，不是顶层窗口）。
       键盘若是独立顶层窗口，show() 会把焦点从输入框抢走，
       m_target 变空——那是测试自己造的假故障。 */
    QWidget win;
    QVBoxLayout *lay = new QVBoxLayout(&win);
    QLineEdit *ed = new QLineEdit(&win);
    VirtualKeyboard kb(&win);
    lay->addWidget(ed);
    lay->addWidget(&kb);
    win.show();
    win.activateWindow();
    QApplication::setActiveWindow(&win);
    QApplication::processEvents();
    kb.attachToApplication();
    /* show() 时输入框就已经拿到焦点了，FocusIn 发生在装过滤器之前。
       清一次再给，才能让过滤器看到这个事件、把 m_target 记上。 */
    ed->clearFocus();
    QApplication::processEvents();
    ed->setFocus();
    QApplication::processEvents();
    qWarning("focus=%s", qApp->focusWidget()
             ? qApp->focusWidget()->metaObject()->className() : "(无)");
    QLineEdit &edit = *ed;

    /* 先验英文路径：这一条本来就是好的，用来区分
       "上屏机制坏了" 和 "中文候选没接上" */
    {
        QPushButton *ka = findKey(&kb, "a");
        if (ka) { ka->click(); qWarning("OK0 英文输入 utf8=%s",
                 edit.text().toUtf8().toHex().constData()); }
        edit.clear();
    }

    QPushButton *lang = findKey(&kb, QString::fromUtf8("英"));
    if (!lang) { qWarning("FAIL 没找到中英键"); return 1; }
    lang->click();
    qWarning("OK1 已切到中文，键面现在是 '%s'",
             findKey(&kb, QString::fromUtf8("中")) ? "中" : "(没变成中)");

    const char *seq = "nihao";
    for (const char *p = seq; *p; p++) {
        QPushButton *k = findKey(&kb, QString(QChar(*p)));
        if (!k) { qWarning("FAIL 没找到键 %c", *p); return 1; }
        k->click();
    }
    qWarning("OK2 已输入 nihao，输入框此时应为空：len=%d", edit.text().size());

    /* 候选按钮：可见、文本非 ASCII 的那些 */
    QList<QPushButton *> bs = kb.findChildren<QPushButton *>();
    QPushButton *first = nullptr;
    int nCand = 0;
    for (int i = 0; i < bs.size(); i++) {
        const QString t = bs[i]->text();
        if (!t.isEmpty() && bs[i]->isVisibleTo(&kb) && t.at(0).unicode() > 0x2000
            && t != QString::fromUtf8("中")) {
            nCand++;
            if (!first) first = bs[i];
        }
    }
    qWarning("OK3 候选数=%d 首选=%s", nCand,
             first ? first->text().toUtf8().toHex().constData() : "(无)");
    if (!first) { qWarning("FAIL 没有候选"); return 1; }

    first->click();
    qWarning("OK4 上屏后输入框 utf8=%s", edit.text().toUtf8().toHex().constData());

    QPushButton *back = findKey(&kb, QString::fromUtf8("退格"));
    if (back) { back->click(); qWarning("OK5 退格后 len=%d", edit.text().size()); }
    qWarning("DONE");
    return 0;
}
