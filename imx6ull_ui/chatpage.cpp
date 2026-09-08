#include "chatpage.h"
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QFrame>
#include <QScrollBar>
#include <QTimer>
#include "micbutton.h"

ChatPage::ChatPage(QWidget *parent) : QWidget(parent)
{
    setStyleSheet(
        "QLineEdit { background:#1a2332; color:#e6e6e6; border:1px solid #2a3648;"
        "  border-radius:20px; padding:10px 16px; font-size:14px; }"
    );

    QVBoxLayout *root = new QVBoxLayout(this);
    /* 边距和间距都压到最小。7 寸屏只有 600px 高，这一页的固定高度部分
       （标题行 + 中间结果标签 + 底部输入行）加上默认的 9px 边距、10px 间距，
       最小高度会超过 600——QMainWindow 全屏时布局最小尺寸一旦大于屏幕，
       Qt 不会压缩内容，而是让窗口比屏幕更大，底部那一行直接被切在屏幕外。
       现象就是"输入框和麦克风按钮只剩上半截"。 */
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(6);

    // ---- 顶部：标题 + TTS 开关 ----
    QHBoxLayout *top = new QHBoxLayout;
    QLabel *title = new QLabel("语音助手", this);
    title->setStyleSheet("font-size:16px; font-weight:700; color:#e6e6e6;");
    top->addWidget(title);
    top->addStretch();
    m_statusLabel = new QLabel("空闲", this);
    m_statusLabel->setStyleSheet("color:#8a94a6; font-size:12px;");
    top->addWidget(m_statusLabel);
    m_ttsToggle = new QPushButton("自动朗读：开", this);
    m_ttsToggle->setStyleSheet("background:#1e2a3f; border:1px solid #33455f; border-radius:8px; padding:6px 12px; font-size:12px;");
    connect(m_ttsToggle, &QPushButton::clicked, this, [this]() { setTtsEnabled(!m_ttsOn); });
    top->addWidget(m_ttsToggle);
    root->addLayout(top);

    // ---- 中间：聊天气泡滚动区 ----
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setStyleSheet("background:#0f1520;");
    QWidget *bubbleContainer = new QWidget;
    bubbleContainer->setStyleSheet("background:#0f1520;");
    m_bubbleLayout = new QVBoxLayout(bubbleContainer);
    m_bubbleLayout->addStretch();
    m_scroll->setWidget(bubbleContainer);
    /* 显式把最小高度设为 0，覆盖 QAbstractScrollArea 自带的 minimumSizeHint。
       聊天记录区是这一页唯一可以让步的部分：屏幕不够高时该缩的是它，
       而不是把底部输入行挤出屏幕。软键盘弹出要占掉两百多像素，
       没有这一行就一定会重演上面那个"底部被切掉"的问题。 */
    m_scroll->setMinimumHeight(0);
    root->addWidget(m_scroll, 1);

    m_partialLabel = new QLabel("", this);
    m_partialLabel->setStyleSheet("color:#5aa9ff; font-size:13px; font-style:italic; padding:0 6px;");
    root->addWidget(m_partialLabel);

    // ---- 底部：文字输入 + 麦克风按钮 ----
    QHBoxLayout *bottom = new QHBoxLayout;
    m_textInput = new QLineEdit(this);
    m_textInput->setPlaceholderText("说点什么，或者直接打字…");
    m_textInput->setMinimumHeight(40);   // 46 -> 40，同样是为 600px 高的屏幕省高度
    connect(m_textInput, &QLineEdit::returnPressed, this, [this]() {
        QString t = m_textInput->text().trimmed();
        if (t.isEmpty()) return;
        appendUserBubble(t);
        emit sendTextRequested(t);
        m_textInput->clear();
    });
    bottom->addWidget(m_textInput, 1);

    m_micBtn = new MicButton(this);
    // 按住说话：按下开始录音，松开结束——车机语音助手最常见的交互方式，
    // 比"点一下开始、再点一下结束"更符合直觉，也不用额外状态提示用户
    // 现在是不是还在录
    connect(m_micBtn, &QPushButton::pressed, this, &ChatPage::micPressed);
    connect(m_micBtn, &QPushButton::released, this, &ChatPage::micReleased);
    bottom->addWidget(m_micBtn);
    root->addLayout(bottom);
}

QWidget *ChatPage::makeBubble(const QString &text, bool isUser)
{
    QFrame *bubble = new QFrame;
    bubble->setStyleSheet(QString(
        "background:%1; border-radius:14px; padding:10px 14px;"
    ).arg(isUser ? "#1e6b4f" : "#1a2332"));
    QVBoxLayout *l = new QVBoxLayout(bubble);
    l->setContentsMargins(14, 10, 14, 10);
    QLabel *lab = new QLabel(text, bubble);
    lab->setWordWrap(true);
    lab->setStyleSheet("color:#e6e6e6; font-size:14px; background:transparent;");
    lab->setMaximumWidth(420);
    l->addWidget(lab);

    QWidget *row = new QWidget;
    QHBoxLayout *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    if (isUser) { rowLayout->addStretch(); rowLayout->addWidget(bubble); }
    else { rowLayout->addWidget(bubble); rowLayout->addStretch(); }
    return row;
}

/* 聊天记录的条数上限。
 *
 * 气泡原来只增不删：每一条消息（包括每一条错误提示）都会永久留下一组
 * Qt 对象。这在桌面程序上无所谓，但这是一台要连续跑几周的设备——
 *   1) 内存无上限增长；
 *   2) 更早暴露的是【卡】：QVBoxLayout 每次插入都要重算全部子项，
 *      条数一多，每发一句话的开销就线性上升，用得越久界面越迟钝。
 * 而且出错时增长最快：网络断了的话，每条命令都会追加一条错误气泡。
 *
 * 60 条对一块 7 寸屏来说远超一屏能显示的量，往上翻已经足够。 */
static const int kMaxBubbles = 60;

void ChatPage::trimBubbles()
{
    /* 布局末尾那个 addStretch() 占一个 item，所以气泡数 = count() - 1。
       从头部删最旧的，直到不超过上限 */
    while (m_bubbleLayout->count() - 1 > kMaxBubbles) {
        QLayoutItem *item = m_bubbleLayout->takeAt(0);
        if (!item) break;
        /* 用 deleteLater 而不是 delete：这个函数是在消息回调链里被调到的，
           而被删的部件可能正处在 Qt 的事件派发路径上，直接 delete 有可能
           在返回时踩到已释放的对象 */
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
}

void ChatPage::appendUserBubble(const QString &text)
{
    m_bubbleLayout->insertWidget(m_bubbleLayout->count() - 1, makeBubble(text, true));
    trimBubbles();
    m_partialLabel->clear();
    QScrollBar *sb = m_scroll->verticalScrollBar();
    QTimer::singleShot(0, this, [sb]() { sb->setValue(sb->maximum()); });
}
void ChatPage::appendAssistantBubble(const QString &text)
{
    m_bubbleLayout->insertWidget(m_bubbleLayout->count() - 1, makeBubble(text, false));
    trimBubbles();
    QScrollBar *sb = m_scroll->verticalScrollBar();
    QTimer::singleShot(0, this, [sb]() { sb->setValue(sb->maximum()); });
}
void ChatPage::updatePartialTranscript(const QString &text) { m_partialLabel->setText(text); }
void ChatPage::setListening(bool listening) { m_statusLabel->setText(listening ? "正在听…" : "空闲"); }
void ChatPage::setThinking(bool thinking) { if (thinking) m_statusLabel->setText("思考中…"); }
void ChatPage::setTtsEnabled(bool en)
{
    m_ttsOn = en;
    m_ttsToggle->setText(en ? "自动朗读：开" : "自动朗读：关");
}
bool ChatPage::ttsEnabled() const { return m_ttsOn; }
