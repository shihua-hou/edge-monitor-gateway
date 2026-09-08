#ifndef WEATHERCLIENT_H
#define WEATHERCLIENT_H

#include <QObject>
#include <QString>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QJsonObject;

/* WeatherClient - 和风天气(QWeather)实时天气查询。
 *
 * 免费开发者额度对这种 demo 项目完全够用。免费版网关地址在和风天气 2023 年
 * 改版后是每个开发者项目专属的一个 host（登录控制台"我的项目"里能看到，
 * 形如 xxxxxxx.re.qweatherapi.com），不是固定域名，所以 apiHost 做成可配置项，
 * 不像 apiKey 那样写死默认值 —— 默认给一个兜底的公共免费网关，用户自己的
 * 项目 host 在设置页里填。
 *
 * 请求一次实时天气只需要一个 GET，没有拿到 key 之前这个类什么都不做，
 * 调 refresh() 只会静默失败(emit error)，不影响其它模块。
 *
 * ---- 关于位置 ----
 * location 填 "auto" 时先用 IP 定位查一次经纬度，再拿去查天气。这台设备是
 * 移动的巡检终端，写死一个城市 ID 在它被搬到别处之后就是错的，而且错得很
 * 安静——温度照样显示，只是显示的是另一个城市的。
 *
 * IP 定位的可信度必须说清楚：它定的是**公网出口所在地**，不是设备所在地。
 * 本项目实测：板子走物联网卡出网，ISP 报的是"IOT Jiangsu network"（江苏），
 * 而定位结果是杭州——出口在运营商网关上。所以：
 *   1) 用户在设置里显式填了 LocationID 或经纬度时，一律以用户填的为准；
 *   2) 自动定位出来的城市名**必须显示在界面上**，否则定位偏了根本没法察觉。
 * 这和这个项目里其它地方的原则一致：数据要带出处，故障不能伪装成正常。
 */
class WeatherClient : public QObject
{
    Q_OBJECT
public:
    explicit WeatherClient(QObject *parent = nullptr);

    /* location 传 "auto" 或空串 = 自动 IP 定位；否则按和风的 location 参数原样用 */
    void configure(const QString &apiHost, const QString &apiKey, const QString &location);
    void refresh();   // 异步发起一次查询，结果通过信号返回

signals:
    /* city 为空表示位置是用户手工配置的（我们只有一个 ID，没有城市名可显示） */
    void weatherUpdated(const QString &tempC, const QString &text,
                        const QString &iconCode, const QString &city);
    void weatherError(const QString &msg);

private:
    /* 给 reply 挂 10 秒超时并接管错误处理，成功时把 body 交给 onOk。
     * 定位和天气两个请求的超时/错误处理完全一样，抽出来免得写两遍——
     * 写两遍的下场通常是只在其中一处修了 bug。 */
    /* onFail 不为空时，失败走 onFail（调用方自己决定怎么降级）；
       为空时才 emit weatherError 并安排重试。
       加这个参数是因为城市名反查失败不应该阻断天气——
       而原来的实现里它会（注释声称不会，代码实际会）。 */
    void getJson(const QString &url, const QString &what,
                 std::function<void(const QJsonObject &)> onOk,
                 std::function<void()> onFail = nullptr);

    /* 失败时统一走这里：报告 + 安排短间隔重试。
     *
     * 为什么需要短重试（开机竞态，实测）：界面起得比网络快，
     * 开机第一次请求必然报 "Host ... not found"（DNS 还没好）。
     * 而刷新定时器是 **30 分钟**——于是开机后半小时内都没有天气。
     * 故障持续几秒，重试间隔半小时，这个比例本身就是 bug。
     *
     * 重试封顶 kMaxRetry 次：和风天气免费额度是有限的，
     * 地址填错这种永久性故障不能每 30 秒烧一次配额。 */
    void failAndRetry(const QString &reason);

    void resolveByIp();      // IP 定位 -> m_location / m_city，完成后接着查天气
    /* 手工配了 LocationID 或经纬度时，拿它反查一个城市名。
       不反查也能用，只是界面上就只剩一个温度——而"不带出处的数字"
       正是这个项目一直在防的东西：101190101 这种 ID 填错一位照样返回 200，
       只是变成了别的城市，没有城市名的话永远不会被发现。
       查不到不阻断天气，最多是没地名可显。 */
    void lookupCityName();
    void fetchWeather();   // 真正查天气，要求 m_location 已就绪

    QNetworkAccessManager *m_nam;
    QString m_apiHost;
    QString m_apiKey;
    QString m_location;   // LocationID 或 "经度,纬度"，和风天气文档里的 location 参数
    QString m_city;       // 自动定位得到的城市名，手工配置时为空
    bool    m_autoLocate; // location 配成 auto
    class QTimer *m_retryTimer = nullptr;
    int     m_retryLeft = 0;
};

#endif // WEATHERCLIENT_H
