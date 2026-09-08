#ifndef CONTROLPAGE_H
#define CONTROLPAGE_H

#include <QWidget>

class QPushButton;
class QSlider;
class QLabel;
class QTimer;

/* ControlPage - 设备控制页：LED/蜂鸣器直接开关 + 小车方向控制。
 * 只负责 UI 和把点击翻译成 (dev,act,p1,p2) 四元组，不知道 MQTT 是什么——
 * 具体怎么发出去是 MainWindow 的事，这页只管交互。 */
class ControlPage : public QWidget
{
    Q_OBJECT
public:
    explicit ControlPage(QWidget *parent = nullptr);

    void setLedState(bool on);
    void setBuzzerState(bool on);

    /* 每收到一帧状态就调一次，用来兑现等待中的命令回执。
       界面上"按钮变了色"只能证明消息发出去了，证不明设备真的执行了——
       中间还隔着 MQTT、网关、CAN、STM32 四层。等状态位回传才算数，
       顺带量出整条链路的往返时延。跟 Web 大屏那边是同一套判定。 */
    void onStateBits(int stateBits);

signals:
    void cmdRequested(int dev, int act, int p1, int p2);

private:
    void toggleLed();
    void toggleBuzzer();
    void carCmd(int act);

    /* 登记一条待确认的命令。bit=0 表示这个动作在状态字里没有对应位
       （转向、调速），只能标成"已发送"——诚实标注比假装确认强 */
    void noteCommand(const QString &label, int bit, bool want);
    void showAck(const QString &text, const char *color);

    QPushButton *m_ledBtn;
    QPushButton *m_buzzerBtn;
    QSlider *m_speedSlider;
    QLabel *m_speedVal;
    QLabel *m_ackLabel = nullptr;    // 最近一条命令的回执
    QTimer *m_ackTimer = nullptr;    // 超时判定，不依赖数据帧驱动（见 .cpp）
    bool m_ledOn = false;
    bool m_buzzerOn = false;

    // 等待确认的那条命令
    QString  m_pendLabel;
    int      m_pendBit = 0;
    bool     m_pendWant = false;
    qint64   m_pendAt = 0;
};

#endif // CONTROLPAGE_H
