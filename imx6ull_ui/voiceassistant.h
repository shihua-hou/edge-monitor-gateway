#ifndef VOICEASSISTANT_H
#define VOICEASSISTANT_H

#include <QObject>
#include <QString>
#include <QByteArray>

QT_BEGIN_NAMESPACE
class QWebSocket;
class QAudioInput;
class QIODevice;
class QTimer;
class QProcess;
QT_END_NAMESPACE

/* VoiceAssistant - 讯飞语音听写(IAT) + 语音合成(TTS) + 星火大模型(Spark) 三件套，
 * 封装成一个"能听会说会聊"的语音助手控制器。
 *
 * 密钥不写死在代码里：构造出来是"未配置"状态，configure() 传空字符串也能
 * "成功"，真正连 WebSocket 时才会鉴权失败。三个值由 Web 大屏经 MQTT 下发、
 * 存进设备本地的 QSettings（见 mainwindow.cpp 的 config/voice 分支）。
 *
 * 真机联调时踩过的坑，按排查顺序记在这里——这几个的表现都是"没声音"，
 * 但原因天差地别：
 *   1. 板子连不上外网       -> 看门狗超时，报"响应超时"
 *   2. 板子时钟偏了 8 小时  -> 签名超出讯飞的 5 分钟窗口，报 401
 *      （RTC 存的是本地时间却被当成 UTC 读，date 显示的数字反而是"对"的，
 *        极具迷惑性；见 xfyunauth.cpp 里打印 sign date 的那行）
 *   3. authorization 的 base64 直接塞进 URL 查询串 -> '+' 被解成空格，
 *      签名对不上，同样报 401（见 xfyunauth.cpp 的编码说明）
 *   4. sparkPath/sparkDomain 跟开通的版本对不上 -> 403 或业务错误码
 * 401 和 403 要分清：401 是签名没过（密钥/时间/编码），403 是签名没问题
 * 但没有该服务的权限（版本/路径）。
 *
 * 三个能力各自是独立的 WebSocket 连接（讯飞这几个 API 目前是分开的服务，
 * 不是一个连接多路复用），同一时间只会有一个在用（要么在听、要么在说、
 * 要么在等大模型回复），没有并发访问的需求，所以共用一个 QWebSocket* 成员，
 * 而不是三个常驻连接。
 *
 * 音频格式：讯飞 IAT/TTS 都要求 16k 采样率、16bit、单声道 PCM（TTS 返回的
 * 也是这个格式，除非配置了其它编码），QAudioFormat 按这个固定写死。 */
class VoiceAssistant : public QObject
{
    Q_OBJECT
public:
    explicit VoiceAssistant(QObject *parent = nullptr);
    ~VoiceAssistant() override;

    struct Config {
        QString appId;
        QString apiKey;
        QString apiSecret;
        QString sparkHost = "spark-api.xf-yun.com";
        /* 路径和 domain 必须跟控制台里【实际开通的版本】对应，填错了握手阶段
           就被拒，返回 403 Forbidden——注意跟密钥错误的 401 是两回事：
           401 是签名校验没过，403 是签名没问题但这个 appid 没有该服务的权限。
           这个账号开通的是 Spark X2/X1.5，控制台给出的接口地址是
               X2  : wss://spark-api.xf-yun.com/x2
               X1.5: wss://spark-api.xf-yun.com/v1.1/x1
           下面按 X2 填。
           domain 不是"版本号"，服务端有一张固定的白名单，实测报错信息里
           给出的正则是：^((?i)spark-x|x1|asean)$
           所以 X2 这条路径对应的 domain 是 "spark-x"，不是 "x2"——填 "x2"
           握手能过，但服务端会回一条业务错误说 value does not match pattern。
           换版本时 Path 和 Domain 要一起改，只改一个照样不通。 */
        QString sparkPath = "/x2";
        QString sparkDomain = "spark-x";
    };
    void configure(const Config &cfg);
    bool isConfigured() const;

    void startListening();   // 打开麦克风，开始语音听写
    void stopListening();    // 结束本轮听写（讯飞按静音自动断句，也支持手动结束）
    void sendChat(const QString &userText);   // 发一条文本给星火大模型（语音识别结果或手打文字都走这个）
    void speak(const QString &text);          // 用 TTS 把文本读出来
    void stopSpeaking();

signals:
    void partialTranscript(const QString &text);   // 听写过程中的中间结果
    void finalTranscript(const QString &text);     // 一句话结束后的最终结果
    void chatChunk(const QString &deltaText);      // 大模型流式回复的增量片段
    void chatFinished(const QString &fullText);    // 大模型回复完整结束
    void speakingStarted();
    void speakingFinished();
    void assistantError(const QString &msg);

private:
    void openIatSocket();
    void openTtsSocket(const QString &text);
    void openSparkSocket(const QString &userText);
    void teardownSocket();
    void playAccumulatedTts();   // 把 m_ttsAccum 包成 wav，调 aplay 播放

    /* 看门狗：讯飞这三个接口都是"发完请求等服务端推结果"，一旦网络半死不活
       （TCP 连着但没数据回来），状态机就永远停在非 Idle，之后所有语音功能
       都会被"正在进行其它语音任务"挡掉——只能重启程序。这里给每个阶段挂一个
       超时，到点就当失败处理，保证状态一定能回到 Idle */
    void armWatchdog(int ms);
    void disarmWatchdog();

    /* 连接类错误统一走这里：还有重试次数就退避后重开，没有就报错收工。
       板子是 WiFi，握手期间偶发失败很常见，一次失败就让用户重按一次按钮
       体验太差 */
    void failTask(const QString &msg, bool retryable);
    void retryCurrentTask();

    Config m_cfg;
    QWebSocket *m_ws;
    enum class Mode { Idle, Listening, Speaking, Chatting } m_mode;

    /* 当前任务的类型和入参，重试时要靠它重建请求 */
    enum class Task { None, Iat, Tts, Chat } m_task = Task::None;
    QString m_taskText;
    int m_retryLeft = 0;
    QTimer *m_watchdog = nullptr;

    // 听写相关
    QAudioInput *m_audioIn;
    QIODevice *m_audioInDevice;
    QTimer *m_iatFrameTimer;
    bool m_iatFirstFrame;
    QString m_iatAccum;   // 当前这句话已经确认的文字（讯飞按"最简中间结果"逐段替换/追加）

    // 播报相关：不用 QAudioOutput 在 Qt 内部解码播放——嵌入式板子上 Qt
    // Multimedia 的音频后端配置是否齐全没有真机验证过，风险不可控；讯飞
    // TTS 流式返回的 PCM 分片先攒起来，收完整段后封装成 WAV 文件，直接
    // 调系统自带的 aplay 播放，路径更短、依赖更少，行为也更容易预测
    QByteArray m_ttsAccum;
    QProcess *m_aplayProc;
    /* aplay 播放期间 WebSocket 其实已经关掉了（音频早收完了），m_mode 会回到
       Idle。但"还在出声"这件事必须单独记一个状态：否则这段时间里再点一次
       播报，Idle 检查会放行，两段音频叠着响，旧进程被 kill 时还会误报一次
       "播放结束"，UI 状态直接乱掉 */
    bool m_playing = false;

    // 对话相关
    QString m_chatAccum;
};

#endif // VOICEASSISTANT_H
