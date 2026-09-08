#include "voiceassistant.h"
#include "xfyunauth.h"

#include <QWebSocket>
#include <QAbstractSocket>
#include <QAudioInput>
#include <QAudioFormat>
#include <QIODevice>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUuid>
#include <QUrl>
#include <QProcess>
#include <QFile>
#include <QDir>
#include <QStandardPaths>

/* 给一段原始 16k/16bit/单声道 PCM 数据加上 44 字节的标准 WAV 文件头，
   aplay 认这个格式最省事，不用额外传 -r/-f/-c 这些参数 */
static QByteArray wrapPcmAsWav(const QByteArray &pcm)
{
    QByteArray header;
    quint32 sampleRate = 16000, byteRate = sampleRate * 2;
    quint16 blockAlign = 2, bitsPerSample = 16, channels = 1, audioFormat = 1;
    quint32 dataSize = (quint32)pcm.size();
    quint32 riffSize = 36 + dataSize;

    header.append("RIFF");
    header.append((const char *)&riffSize, 4);
    header.append("WAVE");
    header.append("fmt ");
    quint32 fmtChunkSize = 16;
    header.append((const char *)&fmtChunkSize, 4);
    header.append((const char *)&audioFormat, 2);
    header.append((const char *)&channels, 2);
    header.append((const char *)&sampleRate, 4);
    header.append((const char *)&byteRate, 4);
    header.append((const char *)&blockAlign, 2);
    header.append((const char *)&bitsPerSample, 2);
    header.append("data");
    header.append((const char *)&dataSize, 4);
    return header + pcm;
}

/* 讯飞 IAT/TTS 要求的音频格式：16kHz / 16bit / 单声道 PCM，三个接口统一
   用这一个格式，写死成一个 helper，不用每处都重新拼 */
static QAudioFormat xfyunPcmFormat()
{
    QAudioFormat fmt;
    fmt.setSampleRate(16000);
    fmt.setChannelCount(1);
    fmt.setSampleSize(16);
    fmt.setCodec("audio/pcm");
    fmt.setByteOrder(QAudioFormat::LittleEndian);
    fmt.setSampleType(QAudioFormat::SignedInt);
    return fmt;
}

VoiceAssistant::VoiceAssistant(QObject *parent)
    : QObject(parent), m_ws(nullptr), m_mode(Mode::Idle),
      m_audioIn(nullptr), m_audioInDevice(nullptr), m_iatFrameTimer(nullptr),
      m_iatFirstFrame(true), m_aplayProc(nullptr)
{
}

VoiceAssistant::~VoiceAssistant()
{
    teardownSocket();   // 内部已经处理了 m_audioIn 和定时器
    if (m_aplayProc) {
        /* 析构里 kill 更要先断信号：lambda 捕获的 this 马上就是野指针了，
           让 finished 在对象死后还能打回来就是 use-after-free */
        m_aplayProc->disconnect(this);
        m_aplayProc->kill();
        m_aplayProc->waitForFinished(300);
    }
}

void VoiceAssistant::configure(const Config &cfg) { m_cfg = cfg; }

bool VoiceAssistant::isConfigured() const
{
    return !m_cfg.appId.isEmpty() && !m_cfg.apiKey.isEmpty() && !m_cfg.apiSecret.isEmpty();
}

void VoiceAssistant::teardownSocket()
{
    disarmWatchdog();
    if (m_iatFrameTimer) { m_iatFrameTimer->stop(); m_iatFrameTimer->deleteLater(); m_iatFrameTimer = nullptr; }
    if (m_audioIn) { m_audioIn->stop(); m_audioIn->deleteLater(); m_audioIn = nullptr; m_audioInDevice = nullptr; }
    if (m_ws) {
        /* 必须先断开所有信号再 close()：teardownSocket 基本都是在
           textMessageReceived 的 lambda 里被调用的（收到 status=2 正常收尾），
           close() 之后 QWebSocket 还会补发 disconnected/error，而那些 lambda
           还连着，于是一次正常结束的会话会紧接着弹一条"连接失败"的假错误 */
        m_ws->disconnect(this);
        m_ws->close();
        m_ws->deleteLater();
        m_ws = nullptr;
    }
    m_mode = Mode::Idle;
    m_task = Task::None;
    m_retryLeft = 0;
}

void VoiceAssistant::armWatchdog(int ms)
{
    if (!m_watchdog) {
        m_watchdog = new QTimer(this);
        m_watchdog->setSingleShot(true);
        connect(m_watchdog, &QTimer::timeout, this, [this]() {
            /* 超时按"可重试"处理：多半是网络卡住而不是参数错，重试比直接
               让用户重按一次更合理 */
            failTask("语音服务响应超时（检查板子能不能连外网、时间是否同步）", true);
        });
    }
    m_watchdog->start(ms);
}

void VoiceAssistant::disarmWatchdog()
{
    if (m_watchdog) m_watchdog->stop();
}

void VoiceAssistant::failTask(const QString &msg, bool retryable)
{
    if (retryable && m_retryLeft > 0) {
        m_retryLeft--;
        Task task = m_task;
        QString text = m_taskText;
        int left = m_retryLeft;
        teardownSocket();          // 会把 m_task/m_retryLeft 清掉，所以上面先存下来
        m_task = task; m_taskText = text; m_retryLeft = left;
        fprintf(stderr, "[voice] %s，重试中（剩 %d 次）\n", msg.toUtf8().constData(), left);
        // 退避一下再重连：刚失败就立刻重试，大概率还是同样的失败
        QTimer::singleShot(800, this, &VoiceAssistant::retryCurrentTask);
        return;
    }
    /* 同样先收尾再 emit。目前接 assistantError 的槽只是弹横幅+写气泡，不会
       发起新任务，所以顺序反了也看不出问题——但"信号发出去时状态必须已经
       是一致的"这条规矩不该只在会出事的地方守，否则哪天有人在错误处理里
       加一句"失败了就播报一下"，就会撞上同一个坑 */
    teardownSocket();
    emit assistantError(msg);
}

void VoiceAssistant::retryCurrentTask()
{
    switch (m_task) {
    case Task::Iat: {
        /* 重试前必须重新打开麦克风：teardownSocket() 已经把上一个关掉并置空了。
           能走到这里说明是握手阶段就失败的（连上之后 m_retryLeft 会清零，
           不再重试），用户这会儿通常还按着按钮在说，重开之后能接着录，
           代价只是丢掉握手这一小段音频 */
        m_audioIn = new QAudioInput(xfyunPcmFormat(), this);
        m_audioInDevice = m_audioIn->start();
        if (!m_audioInDevice) {
            emit assistantError("打不开麦克风（检查板子上的录音设备/驱动）");
            delete m_audioIn; m_audioIn = nullptr;
            teardownSocket();
            return;
        }
        openIatSocket();
        break;
    }
    case Task::Tts:  openTtsSocket(m_taskText); break;
    case Task::Chat: openSparkSocket(m_taskText); break;
    default: break;
    }
}

// ================= 语音听写 IAT =================
void VoiceAssistant::startListening()
{
    if (!isConfigured()) { emit assistantError("语音助手未配置讯飞 APPID/APIKey/APISecret"); return; }
    if (m_mode != Mode::Idle) { emit assistantError("正在进行其它语音任务，请稍后再试"); return; }

    m_audioIn = new QAudioInput(xfyunPcmFormat(), this);
    m_audioInDevice = m_audioIn->start();
    if (!m_audioInDevice) {
        emit assistantError("打不开麦克风（检查板子上的录音设备/驱动）");
        delete m_audioIn; m_audioIn = nullptr;
        return;
    }

    m_task = Task::Iat;
    m_retryLeft = 2;
    openIatSocket();
}

void VoiceAssistant::openIatSocket()
{
    QString url = XfyunAuth::buildWssUrl("iat-api.xfyun.cn", "/v2/iat", m_cfg.apiKey, m_cfg.apiSecret);
    m_ws = new QWebSocket();
    m_mode = Mode::Listening;
    m_iatFirstFrame = true;
    m_iatAccum.clear();

    connect(m_ws, &QWebSocket::connected, this, [this]() {
        // 连上之后开一个定时器，按 40ms 节奏从麦克风缓冲区取一段 PCM 发出去，
        // 讯飞文档建议的发送间隔就是 40ms 一帧（16k*16bit*1ch 下大约 1280 字节），
        // 发太快容易被限流，发太慢会攒帧延迟增大
        m_iatFrameTimer = new QTimer(this);
        connect(m_iatFrameTimer, &QTimer::timeout, this, [this]() {
            if (!m_audioInDevice || !m_ws) return;
            QByteArray chunk = m_audioInDevice->read(1280);
            if (chunk.isEmpty() && !m_iatFirstFrame) return;   // 还没攒够数据，等下一个 tick

            QJsonObject data;
            data["status"] = m_iatFirstFrame ? 0 : 1;
            data["format"] = "audio/L16;rate=16000";
            data["encoding"] = "raw";
            data["audio"] = QString::fromLatin1(chunk.toBase64());

            QJsonObject root;
            if (m_iatFirstFrame) {
                QJsonObject common; common["app_id"] = m_cfg.appId;
                QJsonObject business;
                business["language"] = "zh_cn"; business["domain"] = "iat";
                business["accent"] = "mandarin"; business["vad_eos"] = 3000;
                root["common"] = common; root["business"] = business;
            }
            root["data"] = data;
            m_ws->sendTextMessage(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
            m_iatFirstFrame = false;
        });
        m_iatFrameTimer->start(40);
        m_retryLeft = 0;        // 已经连上并开始推流，后面再断就不是"连不上"了
        armWatchdog(20000);     // 一句话最长按 20s 算，超了当异常收尾
    });

    connect(m_ws, &QWebSocket::textMessageReceived, this, [this](const QString &msg) {
        armWatchdog(20000);     // 有数据回来就续一次命，只在真正卡住时才触发
        QJsonObject root = QJsonDocument::fromJson(msg.toUtf8()).object();
        if (root.value("code").toInt() != 0) {
            /* 服务端明确返回错误码，说明请求本身有问题（鉴权/参数），
               重试多少次都是一样的结果，不重试 */
            failTask("语音听写出错: " + root.value("message").toString(), false);
            return;
        }
        QJsonObject data = root.value("data").toObject();
        QJsonObject result = data.value("result").toObject();
        QJsonArray ws = result.value("ws").toArray();
        QString piece;
        for (const QJsonValue &wv : ws) {
            QJsonArray cw = wv.toObject().value("cw").toArray();
            for (const QJsonValue &cv : cw) piece += cv.toObject().value("w").toString();
        }
        if (!piece.isEmpty()) {
            m_iatAccum += piece;
            emit partialTranscript(m_iatAccum);
        }
        if (data.value("status").toInt() == 2) {
        /* 必须先 teardownSocket() 再 emit：这两句的顺序不是风格问题。
           teardown 才会把 m_mode 复位成 Idle，而外面接收信号的槽几乎一定会
           紧接着发起下一个语音任务（听写结果 -> 送大模型，大模型回复 -> 朗读），
           那些入口第一句就是 if (m_mode != Idle) 报错返回。
           先 emit 的话，下一个任务是在"上一个任务还没收尾"的状态下发起的，
           必然撞上"正在进行其它语音任务，请稍后再试"——链路上每一环都正常，
           唯独整条链走不通。teardown 不会清空累积的结果，但仍然先取出本地
           副本再收尾，避免以后有人在 teardown 里加了清理动作时踩雷。 */
            QString text = m_iatAccum;
            teardownSocket();
            emit finalTranscript(text);
        }
    });

    connect(m_ws, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error), this,
            [this](QAbstractSocket::SocketError) {
        failTask("语音听写连接失败: " + (m_ws ? m_ws->errorString() : QString("未知错误")), true);
    });

    armWatchdog(10000);   // 握手阶段给 10s，连不上就走重试
    m_ws->open(QUrl(url));
}

void VoiceAssistant::stopListening()
{
    if (m_mode != Mode::Listening || !m_ws) return;
    // 发最后一帧(status=2, 空音频)告诉讯飞这句话说完了，让它返回最终结果，
    // 而不是直接把 socket 掐掉——直接断连服务端不会给最终识别结果
    QJsonObject data; data["status"] = 2; data["format"] = "audio/L16;rate=16000";
    data["encoding"] = "raw"; data["audio"] = "";
    QJsonObject root; root["data"] = data;
    m_ws->sendTextMessage(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    if (m_iatFrameTimer) m_iatFrameTimer->stop();
    if (m_audioIn) { m_audioIn->stop(); }
    /* 松开按钮后就等这一个最终结果，给 8s；不设的话服务端要是不回，
       m_mode 会一直卡在 Listening，之后连播报都用不了 */
    armWatchdog(8000);
}

// ================= 语音合成 TTS =================
void VoiceAssistant::speak(const QString &text)
{
    if (!isConfigured()) { emit assistantError("语音助手未配置讯飞 APPID/APIKey/APISecret"); return; }
    if (m_mode != Mode::Idle) { emit assistantError("正在进行其它语音任务，请稍后再试"); return; }
    /* 上一段还在响就先掐掉，不然两段声音会叠在一起。这里是"打断上一句"，
       是合理交互，所以不报错 */
    if (m_playing) stopSpeaking();

    m_task = Task::Tts;
    m_taskText = text;
    m_retryLeft = 2;
    openTtsSocket(text);
}

void VoiceAssistant::openTtsSocket(const QString &text)
{
    QString url = XfyunAuth::buildWssUrl("tts-api.xfyun.cn", "/v2/tts", m_cfg.apiKey, m_cfg.apiSecret);
    m_ws = new QWebSocket();
    m_mode = Mode::Speaking;
    m_ttsAccum.clear();

    connect(m_ws, &QWebSocket::connected, this, [this, text]() {
        QJsonObject common; common["app_id"] = m_cfg.appId;
        QJsonObject business;
        business["aue"] = "raw"; business["auf"] = "audio/L16;rate=16000";
        business["vcn"] = "xiaoyan";   // 发音人，讯飞控制台可选更多音色
        business["tte"] = "UTF8";
        QJsonObject data;
        data["status"] = 2;   // 一次性把整段文本发完，不分帧
        data["text"] = QString::fromLatin1(text.toUtf8().toBase64());
        QJsonObject root; root["common"] = common; root["business"] = business; root["data"] = data;
        m_ws->sendTextMessage(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
        m_retryLeft = 0;   // 请求已经发出去了，后面的失败不该再重发一遍
        /* speakingStarted 放到 playAccumulatedTts 里真正调用 aplay 时才 emit——
           这里只是发起合成请求，声音还没影，emit 两次也不对 */
    });

    connect(m_ws, &QWebSocket::textMessageReceived, this, [this](const QString &msg) {
        armWatchdog(15000);
        QJsonObject root = QJsonDocument::fromJson(msg.toUtf8()).object();
        if (root.value("code").toInt() != 0) {
            failTask("语音合成出错: " + root.value("message").toString(), false);
            return;
        }
        QJsonObject data = root.value("data").toObject();
        QByteArray pcm = QByteArray::fromBase64(data.value("audio").toString().toLatin1());
        if (!pcm.isEmpty()) m_ttsAccum.append(pcm);   // 先攒着，等这段话完全合成完再一次性播放
        if (data.value("status").toInt() == 2) {
            /* 同下面 chatFinished 的理由：先收尾再播放。播放期间 m_mode 应该
               已经是 Idle，"还在出声"由 m_playing 单独记录 */
            teardownSocket();
            playAccumulatedTts();
        }
    });

    connect(m_ws, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error), this,
            [this](QAbstractSocket::SocketError) {
        failTask("语音合成连接失败: " + (m_ws ? m_ws->errorString() : QString("未知错误")), true);
    });

    armWatchdog(15000);
    m_ws->open(QUrl(url));
}

void VoiceAssistant::stopSpeaking()
{
    if (m_aplayProc) {
        /* 先把信号断掉再 kill：否则 kill 触发的 finished 会 emit 一次
           speakingFinished，而调用方往往是"打断当前播报、马上播下一句"，
           这个迟到的结束信号会把新播报的 UI 状态直接改回"未播放" */
        m_aplayProc->disconnect(this);
        m_aplayProc->kill();
        m_aplayProc->deleteLater();
        m_aplayProc = nullptr;
    }
    if (m_playing) { m_playing = false; emit speakingFinished(); }
    if (m_mode == Mode::Speaking) teardownSocket();
}

void VoiceAssistant::playAccumulatedTts()
{
    if (m_ttsAccum.isEmpty()) { emit speakingFinished(); return; }

    QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (dir.isEmpty()) dir = "/tmp";
    QString path = dir + "/edgemonitor_tts.wav";

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        emit assistantError("TTS 播放失败：写临时文件失败 " + path);
        emit speakingFinished();
        return;
    }
    f.write(wrapPcmAsWav(m_ttsAccum));
    f.close();

    if (m_aplayProc) {
        m_aplayProc->disconnect(this);   // 同 stopSpeaking()：别让旧进程的死亡信号打断新播报
        m_aplayProc->kill();
        m_aplayProc->deleteLater();
    }
    m_aplayProc = new QProcess(this);
    connect(m_aplayProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int, QProcess::ExitStatus) {
        m_playing = false;
        emit speakingFinished();
    });
    connect(m_aplayProc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        m_playing = false;
        emit assistantError("TTS 播放失败：aplay 启动失败（检查板子上有没有装 alsa-utils）");
        emit speakingFinished();
    });
    m_playing = true;
    emit speakingStarted();
    m_aplayProc->start("aplay", { path });
}

// ================= 星火大模型对话 =================
void VoiceAssistant::sendChat(const QString &userText)
{
    if (!isConfigured()) { emit assistantError("语音助手未配置讯飞 APPID/APIKey/APISecret"); return; }
    if (m_mode != Mode::Idle) { emit assistantError("正在进行其它语音任务，请稍后再试"); return; }
    m_task = Task::Chat;
    m_taskText = userText;
    m_retryLeft = 2;
    openSparkSocket(userText);
}

void VoiceAssistant::openSparkSocket(const QString &userText)
{
    QString url = XfyunAuth::buildWssUrl(m_cfg.sparkHost, m_cfg.sparkPath, m_cfg.apiKey, m_cfg.apiSecret);
    m_ws = new QWebSocket();
    m_mode = Mode::Chatting;
    m_chatAccum.clear();

    connect(m_ws, &QWebSocket::connected, this, [this, userText]() {
        QJsonObject header;
        header["app_id"] = m_cfg.appId;
        // QUuid::Id128 这个格式枚举 Qt 5.11 才有，嵌入式 SDK 常见的 5.9/5.12
        // 上编译不过，用默认格式再手动去掉花括号，兼容所有 Qt5 版本
        header["uid"] = QUuid::createUuid().toString().remove('{').remove('}');

        QJsonObject chat;
        chat["domain"] = m_cfg.sparkDomain;
        chat["temperature"] = 0.5;
        chat["max_tokens"] = 1024;
        QJsonObject parameter; parameter["chat"] = chat;

        QJsonObject msgObj; msgObj["role"] = "user"; msgObj["content"] = userText;
        QJsonArray textArr; textArr.append(msgObj);
        QJsonObject message; message["text"] = textArr;
        QJsonObject payload; payload["message"] = message;

        QJsonObject root; root["header"] = header; root["parameter"] = parameter; root["payload"] = payload;
        m_ws->sendTextMessage(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
        m_retryLeft = 0;
    });

    connect(m_ws, &QWebSocket::textMessageReceived, this, [this](const QString &msg) {
        /* 流式回复，每来一个分片就续一次看门狗；大模型首字可能要几秒，
           但两个分片之间不该隔太久 */
        armWatchdog(20000);
        QJsonObject root = QJsonDocument::fromJson(msg.toUtf8()).object();
        QJsonObject header = root.value("header").toObject();
        if (header.value("code").toInt() != 0) {
            failTask("大模型对话出错: " + header.value("message").toString(), false);
            return;
        }
        QJsonObject payload = root.value("payload").toObject();
        QJsonObject choices = payload.value("choices").toObject();
        QJsonArray textArr = choices.value("text").toArray();
        QString delta;
        for (const QJsonValue &tv : textArr) delta += tv.toObject().value("content").toString();
        if (!delta.isEmpty()) { m_chatAccum += delta; emit chatChunk(delta); }

        if (header.value("status").toInt() == 2) {
        /* 必须先 teardownSocket() 再 emit：这两句的顺序不是风格问题。
           teardown 才会把 m_mode 复位成 Idle，而外面接收信号的槽几乎一定会
           紧接着发起下一个语音任务（听写结果 -> 送大模型，大模型回复 -> 朗读），
           那些入口第一句就是 if (m_mode != Idle) 报错返回。
           先 emit 的话，下一个任务是在"上一个任务还没收尾"的状态下发起的，
           必然撞上"正在进行其它语音任务，请稍后再试"——链路上每一环都正常，
           唯独整条链走不通。teardown 不会清空累积的结果，但仍然先取出本地
           副本再收尾，避免以后有人在 teardown 里加了清理动作时踩雷。 */
            QString full = m_chatAccum;
            teardownSocket();
            emit chatFinished(full);
        }
    });

    connect(m_ws, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error), this,
            [this](QAbstractSocket::SocketError) {
        failTask("大模型对话连接失败: " + (m_ws ? m_ws->errorString() : QString("未知错误")), true);
    });

    armWatchdog(20000);
    m_ws->open(QUrl(url));
}
