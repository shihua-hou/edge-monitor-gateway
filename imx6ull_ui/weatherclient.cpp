#include "weatherclient.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QUrlQuery>
#include <QTimer>
#include <QPointer>

/* IP 定位服务。用 http 不是 https：
 *   - 这里没有任何敏感数据（就是问"我的公网出口在哪"），不值得为它引一层 TLS；
 *   - 更重要的是，板子的根文件系统是 2019 年做的，CA 包过期过一次，天气因此
 *     整整连不上（详见 deploy/优化记录.md 批次 37）。少一个 HTTPS 依赖，
 *     就少一个会随时间静静烂掉的东西。
 * 免费额度 45 次/分钟，我们每次开机才查一次。 */
static const char *IPGEO_URL = "http://ip-api.com/json/?lang=zh-CN&fields=status,city,lat,lon";

/* 开机竞态下的短重试参数。30 秒 × 10 次 = 覆盖开机后 5 分钟，
   足够等 WiFi 关联 + DHCP + DNS；超过之后交给 30 分钟的常规刷新。 */
static const int kRetryMs  = 30 * 1000;
static const int kMaxRetry = 10;

WeatherClient::WeatherClient(QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)), m_autoLocate(false)
{
}

void WeatherClient::configure(const QString &apiHost, const QString &apiKey, const QString &location)
{
    m_apiHost = apiHost;
    m_apiKey = apiKey;

    const QString loc = location.trimmed();
    m_retryLeft = kMaxRetry;      /* 每次重新配置都把重试额度收满 */
    m_autoLocate = loc.isEmpty() || loc.compare("auto", Qt::CaseInsensitive) == 0;
    if (m_autoLocate) {
        /* 每次重新配置都清掉上次定位的结果，让它重新查一次。
           这台设备是会被搬走的，缓存住反而会一直显示旧地方的天气。 */
        m_location.clear();
        m_city.clear();
    } else {
        m_location = loc;
        m_city.clear();   // 手工配置的只有 ID，没有城市名可显示
    }
}

void WeatherClient::failAndRetry(const QString &reason)
{
    emit weatherError(reason);
    if (m_retryLeft <= 0) return;
    m_retryLeft--;
    if (!m_retryTimer) {
        m_retryTimer = new QTimer(this);
        m_retryTimer->setSingleShot(true);
        connect(m_retryTimer, &QTimer::timeout, this, &WeatherClient::refresh);
    }
    if (!m_retryTimer->isActive()) m_retryTimer->start(kRetryMs);
}

void WeatherClient::getJson(const QString &url, const QString &what,
                            std::function<void(const QJsonObject &)> onOk,
                            std::function<void()> onFail)
{
    QNetworkReply *reply = m_nam->get(QNetworkRequest(QUrl(url)));

    /* 自己加超时。QNetworkRequest::setTransferTimeout 是 Qt 5.15 才有的，
       而嵌入式 SDK 常年停在 5.9/5.12，用不了。
       没有超时会怎样：网络半死不活时（TCP 连着但没有任何响应——正是 WiFi
       关联僵死时的典型状态），这个 reply 永远不会 finished。天气永远停在
       上一次的值上，既不更新也不报错；而 refresh 每半小时来一次，
       pending 的 reply 只增不减，跑上几天就是一堆再也回收不了的对象。
       "永远等待"是最坏的一种失败——它连"失败了"这个信息都不给你。

       QPointer 而不是裸指针：reply 正常完成时会 deleteLater，
       定时器要是晚一步触发，裸指针就是野指针；QPointer 在对象析构时
       自动置空，判一下就安全了。 */
    QPointer<QNetworkReply> guard(reply);
    QTimer::singleShot(10000, this, [guard]() {
        if (guard && guard->isRunning()) guard->abort();   /* 触发 finished，走下面的错误分支 */
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, what, onOk, onFail]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            /* 主动 abort 造成的错误单独说明，否则界面上显示的是
               "Operation canceled"，看着像是谁取消了操作，
               实际原因是"等了 10 秒没有任何响应" */
            const QString why = (reply->error() == QNetworkReply::OperationCanceledError)
                ? QString("10 秒无响应（检查板子能否访问外网）")
                : reply->errorString();
            if (onFail) onFail();
            else        failAndRetry(what + "失败: " + why);
            return;
        }
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            if (onFail) onFail();
            else        failAndRetry(what + "失败: 返回的不是 JSON");
            return;
        }
        onOk(doc.object());
    });
}

void WeatherClient::resolveByIp()
{
    getJson(IPGEO_URL, "IP 定位", [this](const QJsonObject &o) {
        if (o.value("status").toString() != "success") {
            emit weatherError("IP 定位失败，去设置里手工填城市");
            return;
        }
        /* 和风的 location 要 "经度,纬度"，最多两位小数——注意是经度在前，
           和一般人念的"纬度,经度"正好相反，写反了会查到地球上另一个地方
           （而且照样返回 200，只是天气不对）。 */
        const double lat = o.value("lat").toDouble();
        const double lon = o.value("lon").toDouble();
        if (lat == 0.0 && lon == 0.0) {
            emit weatherError("IP 定位返回的坐标无效");
            return;
        }
        m_location = QString::number(lon, 'f', 2) + "," + QString::number(lat, 'f', 2);
        m_city = o.value("city").toString();
        fetchWeather();
    });
}

void WeatherClient::lookupCityName()
{
    QUrl url("https://" + m_apiHost + "/geo/v2/city/lookup");
    QUrlQuery q;
    q.addQueryItem("location", m_location);
    q.addQueryItem("key", m_apiKey);
    url.setQuery(q);

    /* 地名只是锦上添花，拿不到也必须继续查天气。
       这一点原来只写在注释里、代码里没实现：getJson 的错误分支直接
       return 了，fetchWeather() 根本走不到。开机 DNS 没好时反查失败，
       天气就被整个挡死——屏幕上只看到"城市名反查失败"，
       完全看不出天气本身其实没人去请求过。现在用 onFail 真的降级。 */
    getJson(url.toString(), "城市名反查",
        [this](const QJsonObject &root) {
            if (root.value("code").toString() == "200") {
                QJsonArray arr = root.value("location").toArray();
                if (!arr.isEmpty()) m_city = arr.at(0).toObject().value("name").toString();
            }
            if (m_city.isEmpty()) m_city = "-";   /* 占位，避免每次刷新都重查 */
            fetchWeather();
        },
        [this]() {                 /* 反查失败：不报错、不重试，直接无名字去查天气 */
            m_city.clear();
            fetchWeather();
        });
}

void WeatherClient::fetchWeather()
{
    QUrl url("https://" + m_apiHost + "/v7/weather/now");
    QUrlQuery q;
    q.addQueryItem("location", m_location);
    q.addQueryItem("key", m_apiKey);
    url.setQuery(q);

    getJson(url.toString(), "天气请求", [this](const QJsonObject &root) {
        if (root.value("code").toString() != "200") {
            /* 接口返回非 200（key 错、额度用完、location 非法）多半是配置问题，
               重试没用也烧配额，直接报错不重试 */
            emit weatherError("天气接口返回异常 code=" + root.value("code").toString());
            return;
        }
        m_retryLeft = kMaxRetry;   /* 成功一次就把重试额度收满，供下次故障用 */
        QJsonObject now = root.value("now").toObject();
        emit weatherUpdated(now.value("temp").toString(),
                            now.value("text").toString(),
                            now.value("icon").toString(),
                            m_city);
    });
}

void WeatherClient::refresh()
{
    if (m_retryTimer) m_retryTimer->stop();
    if (m_apiKey.isEmpty() || m_apiHost.isEmpty()) {
        emit weatherError("天气服务未配置（缺少 API Host/Key），去系统设置里填");
        return;
    }
    if (m_autoLocate && m_location.isEmpty()) { resolveByIp(); return; }
    if (m_location.isEmpty()) {
        emit weatherError("天气服务未配置（缺少位置），去系统设置里填");
        return;
    }
    /* 手工配置的只有一个 ID/坐标，先反查一次城市名（只查一次，之后有缓存） */
    if (m_city.isEmpty()) { lookupCityName(); return; }
    fetchWeather();
}
