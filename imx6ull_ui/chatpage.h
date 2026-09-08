#ifndef CHATPAGE_H
#define CHATPAGE_H

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;
class QScrollArea;
class MicButton;

/* ChatPage - 语音助手页：车载中控那种对话气泡 + 大号麦克风按钮的交互形态。
 *
 * 这一页本身不直接碰 VoiceAssistant/讯飞协议——它只管"用户点了什么、要显示
 * 什么气泡"，具体语音识别/大模型请求/语音播报由 MainWindow 持有的
 * VoiceAssistant 实例去做，通过信号槽跟这页对接。这样録音/网络协议的复杂度
 * 不会污染这个纯 UI 类，测试/替换其中一层的时候也互不牵连。 */
class ChatPage : public QWidget
{
    Q_OBJECT
public:
    explicit ChatPage(QWidget *parent = nullptr);

    void appendUserBubble(const QString &text);
    void appendAssistantBubble(const QString &text);
    void updatePartialTranscript(const QString &text);   // 实时显示"正在听"的中间结果
    void setListening(bool listening);
    void setThinking(bool thinking);
    void setTtsEnabled(bool en);
    bool ttsEnabled() const;

signals:
    void micPressed();     // 按下开始录音
    void micReleased();    // 松开结束录音（按住说话的交互方式）
    void sendTextRequested(const QString &text);

private:
    QWidget *makeBubble(const QString &text, bool isUser);
    /* 删掉超出上限的最旧气泡。设备要连续跑几周，聊天记录只增不删
       会一路吃内存，而且布局重算的开销随条数线性上升——用得越久越卡 */
    void trimBubbles();

    QVBoxLayout *m_bubbleLayout;
    QScrollArea *m_scroll;
    QLabel *m_partialLabel;
    QLabel *m_statusLabel;
    MicButton *m_micBtn;
    QLineEdit *m_textInput;
    QPushButton *m_ttsToggle;
    bool m_ttsOn = true;
};

#endif // CHATPAGE_H
