#include "virtualkeyboard.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLineEdit>
#include <QApplication>
#include <QKeyEvent>
#include <QEvent>
#include <QLabel>
#include <QFile>

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
/* 中英切换键。标签直接显示当前状态（"中"/"英"），而不是显示"要切到哪"——
   后者每次都要在脑子里绕一道，而人看键盘是为了确认现在能打什么。 */
static const char *K_LANG  = "中/英";

/* 候选条最多放几个。屏幕宽 1024，键盘占满宽度，
   一个候选按钮连中文带边距约 90px，放 9 个还留得下编码显示区。 */
static const int kMaxCands = 9;

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

    /* 候选条。**默认隐藏**：英文输入时它一行都不占，
       不然键盘白白高出 40px，而 600px 的屏每一像素都要省。 */
    m_candBar = new QWidget(this);
    m_candLayout = new QHBoxLayout(m_candBar);
    m_candLayout->setContentsMargins(0, 0, 0, 0);
    m_candLayout->setSpacing(4);

    m_composeLabel = new QLabel(m_candBar);
    m_composeLabel->setMinimumWidth(96);
    m_composeLabel->setStyleSheet("color:#5aa9ff; font-size:15px; padding-left:6px;");
    m_candLayout->addWidget(m_composeLabel);

    for (int i = 0; i < kMaxCands; i++) {
        QPushButton *b = new QPushButton(m_candBar);
        b->setFocusPolicy(Qt::NoFocus);
        b->setMinimumHeight(34);
        b->setStyleSheet(
            "QPushButton{background:#141c2a; color:#e6e6e6; border:1px solid #2a3648;"
            "  border-radius:6px; font-size:17px; padding:0 8px;}"
            "QPushButton:pressed{background:#3ddc97; color:#04140d;}");
        connect(b, &QPushButton::clicked, this, [this, i]() { commitCandidate(i); });
        b->hide();
        m_candBtns.append(b);
        m_candLayout->addWidget(b);
    }
    m_candLayout->addStretch();
    m_candBar->hide();
    m_rows->addWidget(m_candBar);

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

    /* 中英键显示的是**当前状态**（"中"/"英"），不是"点了会切到哪"。
       后者每次都要在脑子里绕一道，而人看键盘是为了确认现在能打什么。 */
    if (label == K_LANG) b->setText(m_chinese ? QStringLiteral("中") : QStringLiteral("英"));

    const bool fn = (label == K_SHIFT || label == K_BACK || label == K_ENTER ||
                     label == K_SYM   || label == K_ABC  || label == K_HIDE ||
                     label == K_LANG);
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
    /* 清空旧的行（切换大小写/符号层时整层重建，比逐个改按钮文字简单可靠）。
       **从 1 开始，不动 0 号**：0 号是候选条，它跨层存在，
       跟着一起清掉的话切一次大小写它就从布局里掉出来了——
       控件还活着，只是不再被布局管，表现为"候选条莫名其妙消失"。 */
    while (m_rows->count() > 1) {
        QLayoutItem *item = m_rows->takeAt(1);
        if (QLayout *sub = item->layout()) {
            while (QLayoutItem *ci = sub->takeAt(0)) {
                if (ci->widget()) ci->widget()->deleteLater();
                delete ci;
            }
            /* **这里不能再 delete sub。**
               QLayout 继承自 QLayoutItem，takeAt() 取出一个嵌套布局时，
               item 和 item->layout() 是**同一个对象**——分别删一次就是双重释放。
               原来的代码两个都删，只是平时走不到：只有切大小写/符号层时
               才会进这个清理循环，而那两个键大概没人按过。
               加了中英切换键之后每次切换都走这条路，当场崩。 */
        }
        delete item;   /* item 就是那个子布局本身，删这一次就够 */
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
        addRow({QString(K_SYM), QString(K_LANG), QString(K_HIDE),
                QString(K_SPACE), ".", QString(K_ENTER)});
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
    if (label == K_LANG)  { setChinese(!m_chinese); return; }

    /* 中文态先让拼音逻辑处理。它处理不了的（比如符号层的键）再往下走，
       按原来的路径直接发给输入框。 */
    if (m_chinese && handleChineseKey(label)) return;

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


/* ==================== 中文输入 ==================== */

void VirtualKeyboard::setChinese(bool on)
{
    if (on && !m_imeTried) {
        m_imeTried = true;
        /* 词库跟可执行文件放一起。找不到就退回英文——
           **不能假装切成功了**：一个按了没反应的"中"键，
           比一个明确告诉你没装词库的键坏得多。 */
        m_ime.load(QStringLiteral("/home/root/pinyin.dict"));
    }
    if (on && !m_ime.isLoaded()) {
        qWarning("[pinyin] 词库没加载成功，保持英文输入");
        return;
    }

    m_chinese = on;
    clearComposing();
    /* 切到中文时强制回小写层：中文态下"大写"没有意义，
       而停在上档层会让字母键显示成大写，看着像还在英文。 */
    if (m_chinese && m_layer != Lower) setLayer(Lower);
    else rebuildKeys();
}

void VirtualKeyboard::clearComposing()
{
    m_composing.clear();
    m_cands.clear();
    m_composeLabel->clear();
    for (int i = 0; i < m_candBtns.size(); i++) m_candBtns[i]->hide();
    m_candBar->setVisible(false);
}

void VirtualKeyboard::updateCandidates()
{
    if (m_composing.isEmpty()) { clearComposing(); return; }

    m_cands = m_ime.candidates(m_composing, kMaxCands);
    m_composeLabel->setText(m_composing);

    for (int i = 0; i < m_candBtns.size(); i++) {
        if (i < m_cands.size()) {
            m_candBtns[i]->setText(m_cands[i]);
            m_candBtns[i]->show();
        } else {
            m_candBtns[i]->hide();
        }
    }
    /* 即使一个候选都没有也要把候选条显示出来——那样至少能看到
       自己打的拼音是什么，才知道是打错了还是词库里没有。 */
    m_candBar->setVisible(true);
}

void VirtualKeyboard::commitCandidate(int index)
{
    if (index < 0 || index >= m_cands.size()) return;
    const QString word = m_cands.at(index);
    /* 一次一个字符地发。QKeyEvent 带多字符 text 时，部分控件只取第一个字符，
       "你好"会变成"你"——逐字符发没有这个问题，代价可以忽略。 */
    for (int i = 0; i < word.size(); i++)
        sendKey(Qt::Key_unknown, QString(word.at(i)));
    clearComposing();
}

bool VirtualKeyboard::handleChineseKey(const QString &label)
{
    /* 回车/收起：有未上屏的拼音就先清掉，别把拼音字母漏到输入框里 */
    if (label == K_ENTER || label == K_HIDE) {
        if (!m_composing.isEmpty()) { clearComposing(); return true; }
        return false;
    }

    if (label == K_BACK) {
        if (m_composing.isEmpty()) return false;   /* 没在打字，退格照常删输入框 */
        m_composing.chop(1);
        updateCandidates();
        return true;
    }

    if (label == K_SPACE) {
        /* 空格上屏第一个候选，和常见输入法一致。
           没有候选时（拼音打错了）就当普通空格。 */
        if (!m_cands.isEmpty()) { commitCandidate(0); return true; }
        return false;
    }

    /* 只有单个小写字母进编码缓冲。数字和符号在中文态下直接上屏，
       不参与拼音——打"1"就该出"1"。 */
    if (label.size() == 1) {
        const QChar ch = label.at(0);
        if (ch >= QLatin1Char('a') && ch <= QLatin1Char('z')) {
            m_composing += ch;
            updateCandidates();
            return true;
        }
    }
    return false;
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
            /* 未上屏的拼音要丢掉。留着的话下次在**另一个**输入框弹出键盘时，
               候选条还挂着上一个框的半截拼音，一按就把字上到错的地方。 */
            clearComposing();
            m_target = nullptr;
            hide();
        }
    }
    return QWidget::eventFilter(obj, ev);   // 一律放行，纯观察不拦截
}
