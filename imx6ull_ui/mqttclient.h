#ifndef MQTTCLIENT_H
#define MQTTCLIENT_H

#include <QObject>
#include <QByteArray>
#include <QString>
#include <mosquitto.h>

/* MqttClient - 对 libmosquitto 的一层瘦封装，把它的回调（跑在 libmosquitto
 * 自己开的后台线程上）安全地转成 Qt 信号（在 GUI 线程上触发）。
 *
 * 直接在 mosquitto 的回调里操作 QWidget 是不允许的——Qt Widgets 只能在创建它们
 * 的那个（GUI）线程里访问。这里统一通过 QMetaObject::invokeMethod(...,
 * Qt::QueuedConnection) 把回调数据投递回 GUI 线程的事件队列，再在那边真正
 * emit 信号，图省事直接在回调里 emit 是新手常踩的一个坑，这里从设计上直接
 * 避开。
 */
class MqttClient : public QObject
{
    Q_OBJECT
public:
    explicit MqttClient(const QString &clientId, QObject *parent = nullptr);
    ~MqttClient() override;

    /* 阻塞时间很短（发起连接+起后台 loop 线程），不是长阻塞，GUI 线程直接调没问题 */
    bool start(const QString &host, int port, const QString &user, const QString &pass);
    void stop();

    /* mosquitto_loop_start() 起的后台线程内部自带加锁，publish/subscribe
       这些 API 本来就可以直接从 GUI 线程调，不需要（也不能）额外调
       mosquitto_threaded_set(true) —— 见 .cpp 里构造函数的注释 */
    void publish(const QString &topic, const QByteArray &payload);

    void subscribe(const QString &topicFilter);

signals:
    void connected();
    void disconnected();
    /* v2: mosquitto_connect_async 几乎总是"立即成功"（它只是把连接请求排进
       后台线程，真正的 TCP 连接/鉴权在那条线程里异步进行），host 填错、
       密码填错、网络不通，这三种情况在 UI 上原来看起来一模一样——一直卡
       在"离线"，没有任何区分度。这个信号专门把"到底是哪里失败了"的具体
       原因（socket 层错误、还是 broker 拒绝鉴权）传出来。 */
    void connectionFailed(const QString &reason);
    void messageReceived(const QString &topic, const QByteArray &payload);

private:
    static void onConnectCb(struct mosquitto *m, void *userdata, int rc);
    static void onDisconnectCb(struct mosquitto *m, void *userdata, int rc);
    static void onMessageCb(struct mosquitto *m, void *userdata, const struct mosquitto_message *msg);
    static void onLogCb(struct mosquitto *m, void *userdata, int level, const char *str);

    /* 真正在 GUI 线程执行、负责 emit 信号的槽函数，由上面几个静态回调
       通过 QueuedConnection 投递过来调用 */
    Q_INVOKABLE void handleConnect(int rc);
    Q_INVOKABLE void handleDisconnect(int rc);
    Q_INVOKABLE void handleMessage(const QString &topic, const QByteArray &payload);
    void onConnectTimeout();
    /* connect_async 即刻失败时的重试。不能指望 mosquitto 自己重连——
       它的自动重连在 loop 线程里，而 connect_async 失败时 loop_start
       根本还没被调用。见 start() 里的说明。 */
    void retryConnect();

    struct mosquitto *m_mosq;
    bool m_libInited;
    class QTimer *m_connectTimeoutTimer = nullptr;
    class QTimer *m_retryTimer = nullptr;
    /* 重试时要用的连接参数。存下来而不是让调用方重传：
       调用方（MainWindow）只在启动和改配置时调 start()，
       重连是这个类自己的职责，不该漏到上层。 */
    QString m_host, m_user, m_pass;
    int  m_port = 1883;
    bool m_loopStarted = false;
};

#endif // MQTTCLIENT_H
