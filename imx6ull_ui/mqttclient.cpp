#include "mqttclient.h"
#include <QMetaObject>
#include <QTimer>
#include <QDebug>
#include <cstring>
#include <cstdio>

/* 连接发起后等多久还没收到 CONNACK 就判定为"连不上"，主动给用户一个反馈。
   mosquitto 自己的 keepalive 是 60s，但那是"已连接后多久没心跳算断线"，
   跟"第一次连接握手要等多久"是两回事，这里给 8 秒——网络通的话正常应该
   1 秒内就有响应，8 秒足够宽松又不会让人干等太久 */
static const int kConnectTimeoutMs = 8000;

MqttClient::MqttClient(const QString &clientId, QObject *parent)
    : QObject(parent), m_mosq(nullptr), m_libInited(false)
{
    mosquitto_lib_init();
    m_libInited = true;
    m_mosq = mosquitto_new(clientId.toUtf8().constData(), true, this);
    if (m_mosq) {
        /* 不要调 mosquitto_threaded_set(true)——那是给"调用者自己起线程跑
           mosquitto_loop()"这种用法准备的，跟下面 start() 里用的
           mosquitto_loop_start()（让库自己开后台线程）是两套互斥的线程
           模型。两个一起用，mosquitto_loop_start 会直接返回 MOSQ_ERR_INVAL
           （之前踩过这个坑：loop_start 失败，日志报 "Invalid function
           arguments"，就是这两个调用撞车了）。用 loop_start 这条路径本身
           内部就已经做好了线程安全，不需要也不能再手动 threaded_set。 */
        mosquitto_connect_callback_set(m_mosq, &MqttClient::onConnectCb);
        mosquitto_disconnect_callback_set(m_mosq, &MqttClient::onDisconnectCb);
        mosquitto_message_callback_set(m_mosq, &MqttClient::onMessageCb);
        /* 日志回调直接打到 stderr，不走 Qt 信号——这只是给人在终端里看的
           调试信息，不是要驱动 UI 状态，没必要走一遍线程转发那一套 */
        mosquitto_log_callback_set(m_mosq, &MqttClient::onLogCb);
    }
}

MqttClient::~MqttClient()
{
    stop();
    if (m_mosq) mosquitto_destroy(m_mosq);
    if (m_libInited) mosquitto_lib_cleanup();
}

/* 开机竞态（真实事故）：界面起得比 WiFi 快，第一次 connect_async 直接
   返回 ENETUNREACH。原代码到这里就 return false，于是：
     ① mosquitto_loop_start 根本没被调用；
     ② 而 mosquitto 的"自动重连"恰恰就跑在 loop 线程里。
   结果是这个客户端变成一具尸体，屏幕永远"离线"，而同一块板子上自己
   写了重连循环的 gateway_mqtt 却一切正常——两个进程行为不一致，
   正是定位到这里的线索。

   重试间隔用固定 5 秒而不是退避：内网设备，失败原因几乎总是"网络还没好"
   而不是"broker 被我们压垓了"，退避只会拖长恢复时间。 */
static const int kRetryMs = 5000;

void MqttClient::retryConnect()
{
    if (m_host.isEmpty()) return;
    start(m_host, m_port, m_user, m_pass);
}

bool MqttClient::start(const QString &host, int port, const QString &user, const QString &pass)
{
    if (!m_mosq) return false;

    /* 存下参数供重试用。重连是这个类自己的职责，不该漏到上层 */
    m_host = host; m_port = port; m_user = user; m_pass = pass;
    if (m_retryTimer) { m_retryTimer->stop(); m_retryTimer->deleteLater(); m_retryTimer = nullptr; }

    if (!user.isEmpty()) {
        mosquitto_username_pw_set(m_mosq, user.toUtf8().constData(), pass.toUtf8().constData());
    }
    fprintf(stderr, "[mqtt] connecting to %s:%d ...\n", host.toUtf8().constData(), port);
    int rc = mosquitto_connect_async(m_mosq, host.toUtf8().constData(), port, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        QString reason = QString("connect_async 失败: %1（5 秒后重试）")
                             .arg(mosquitto_strerror(rc));
        fprintf(stderr, "[mqtt] %s\n", reason.toUtf8().constData());
        emit connectionFailed(reason);
        m_retryTimer = new QTimer(this);
        m_retryTimer->setSingleShot(true);
        connect(m_retryTimer, &QTimer::timeout, this, &MqttClient::retryConnect);
        m_retryTimer->start(kRetryMs);
        return false;
    }
    /* mosquitto_loop_start 会另开一个线程做网络收发+自动重连，跟 Qt 的事件循环
       是两条独立线程，互不阻塞——这也是为什么上面那套"回调转信号"的机制是
       必需的，而不是可省的花架子 */
    /* loop 线程只能启一次。启起来之后，后续的掉线重连就由它接管了，
       上面那个重试定时器只负责"连都没连上过"这一种情况。 */
    if (!m_loopStarted) {
        rc = mosquitto_loop_start(m_mosq);
        if (rc != MOSQ_ERR_SUCCESS) {
            QString reason = QString("loop_start 失败: %1").arg(mosquitto_strerror(rc));
            fprintf(stderr, "[mqtt] %s\n", reason.toUtf8().constData());
            emit connectionFailed(reason);
            return false;
        }
        m_loopStarted = true;
    }

    if (m_connectTimeoutTimer) { m_connectTimeoutTimer->stop(); m_connectTimeoutTimer->deleteLater(); }
    m_connectTimeoutTimer = new QTimer(this);
    m_connectTimeoutTimer->setSingleShot(true);
    connect(m_connectTimeoutTimer, &QTimer::timeout, this, &MqttClient::onConnectTimeout);
    m_connectTimeoutTimer->start(kConnectTimeoutMs);
    return true;
}

void MqttClient::onConnectTimeout()
{
    QString reason = "连接超时（8 秒内没收到 broker 响应），检查 host/端口是否正确、"
                      "网络是否可达（broker 是否在跑、板子和 broker 是否互通）";
    fprintf(stderr, "[mqtt] %s\n", reason.toUtf8().constData());
    emit connectionFailed(reason);
}

void MqttClient::stop()
{
    if (!m_mosq) return;
    /* 主动断开时一定要把连接超时定时器停掉：设置页改完配置会先 stop() 再
       重新 start()，如果这个定时器还在跑，8 秒后会对着已经作废的这次连接
       弹一条"连接超时"，而那时新连接可能早就连上了 */
    if (m_connectTimeoutTimer) m_connectTimeoutTimer->stop();
    mosquitto_disconnect(m_mosq);
    mosquitto_loop_stop(m_mosq, true);
}

void MqttClient::publish(const QString &topic, const QByteArray &payload)
{
    if (!m_mosq) return;
    mosquitto_publish(m_mosq, nullptr, topic.toUtf8().constData(),
                       payload.size(), payload.constData(), 0, false);
}

void MqttClient::subscribe(const QString &topicFilter)
{
    if (!m_mosq) return;
    mosquitto_subscribe(m_mosq, nullptr, topicFilter.toUtf8().constData(), 1);
}

void MqttClient::onConnectCb(struct mosquitto *, void *userdata, int rc)
{
    MqttClient *self = static_cast<MqttClient *>(userdata);
    QMetaObject::invokeMethod(self, "handleConnect", Qt::QueuedConnection, Q_ARG(int, rc));
}

void MqttClient::onDisconnectCb(struct mosquitto *, void *userdata, int rc)
{
    MqttClient *self = static_cast<MqttClient *>(userdata);
    QMetaObject::invokeMethod(self, "handleDisconnect", Qt::QueuedConnection, Q_ARG(int, rc));
}

void MqttClient::onMessageCb(struct mosquitto *, void *userdata, const struct mosquitto_message *msg)
{
    MqttClient *self = static_cast<MqttClient *>(userdata);
    QString topic = QString::fromUtf8(msg->topic);
    QByteArray payload(static_cast<const char *>(msg->payload), msg->payloadlen);
    /* payload 在这个回调返回后 libmosquitto 就会释放，所以必须在这里就地拷贝
       成 QByteArray（值语义，自带深拷贝），不能只存指针再排队过去 */
    QMetaObject::invokeMethod(self, "handleMessage", Qt::QueuedConnection,
                               Q_ARG(QString, topic), Q_ARG(QByteArray, payload));
}

void MqttClient::onLogCb(struct mosquitto *, void *, int level, const char *str)
{
    /* 只放行警告和错误，丢掉 INFO/DEBUG。
     *
     * 这不是"嫌日志多"，是这些噪声真的害过一次：DEBUG 级别里有每一条
     * "Client xxx received PUBLISH"，而传感器每秒推 5 帧，日志一秒五行。
     * 排查语音鉴权失败时 `tail -25` 翻上去全是 PUBLISH，真正的错误一条都
     * 看不见——日志的作用是让问题浮出来，不是把问题埋进去。
     *
     * 而且 /tmp 在这块板子上是 tmpfs（吃的是内存不是 flash），按这个速率
     * 一天几十 MB，跑上几天就是实打实的内存压力。
     *
     * 需要看完整 MQTT 交互时，把下面这行注释掉即可。 */
    if (!(level & (MOSQ_LOG_WARNING | MOSQ_LOG_ERR))) return;
    fprintf(stderr, "[mosquitto] %s\n", str);
}

void MqttClient::handleConnect(int rc)
{
    if (m_connectTimeoutTimer) { m_connectTimeoutTimer->stop(); }
    if (rc == 0) {
        emit connected();
    } else {
        /* rc 非 0 是 broker 主动拒绝（比如账号密码错、未授权），跟"网络根本
           没通"是两回事，mosquitto_connack_string 能把这个原因翻成人话，
           比如 "Connection Refused: not authorised."——这是排查"密码是不是
           填错了"最直接的证据 */
        QString reason = QString("broker 拒绝连接: %1").arg(mosquitto_connack_string(rc));
        fprintf(stderr, "[mqtt] %s\n", reason.toUtf8().constData());
        emit connectionFailed(reason);
        emit disconnected();
    }
}

void MqttClient::handleDisconnect(int rc)
{
    if (m_connectTimeoutTimer) { m_connectTimeoutTimer->stop(); }
    fprintf(stderr, "[mqtt] disconnected (rc=%d: %s)\n", rc, mosquitto_strerror(rc));
    emit disconnected();
}

void MqttClient::handleMessage(const QString &topic, const QByteArray &payload)
{
    emit messageReceived(topic, payload);
}
