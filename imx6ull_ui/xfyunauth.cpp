#include "xfyunauth.h"
#include <QDateTime>
#include <QLocale>
#include <QMessageAuthenticationCode>
#include <QCryptographicHash>
#include <QUrl>
#include <cstdio>

namespace XfyunAuth {

QString buildWssUrl(const QString &host, const QString &path,
                     const QString &apiKey, const QString &apiSecret)
{
    // 讯飞签名里嵌的 date 字符串必须是标准 RFC1123（"GMT"结尾），不用
    // Qt::RFC2822Date 是因为那个格式默认输出 +0000 时区偏移而不是字面 "GMT"，
    // 跟讯飞文档示例的字符串格式对不上，用英文 locale 手动拼，避免运行环境
    // locale 是中文时把星期/月份拼成中文导致签名字符串跟服务端算的不一致
    QDateTime now = QDateTime::currentDateTimeUtc();
    QLocale enLocale(QLocale::English, QLocale::UnitedStates);
    QString date = enLocale.toString(now, "ddd, dd MMM yyyy HH:mm:ss") + " GMT";

    /* 把签名用的 UTC 时间打出来。板子没有 NTP、RTC 又常年存本地时间当 UTC 用，
       时钟漂移会让签名超出讯飞的 5 分钟窗口，报的同样是 401——跟密钥填错
       的表现完全一样。有这一行就能一眼分开这两种情况 */
    fprintf(stderr, "[xfyun] sign date=%s host=%s\n",
            date.toUtf8().constData(), host.toUtf8().constData());

    QString signatureOrigin = QString("host: %1\ndate: %2\nGET %3 HTTP/1.1")
                                   .arg(host, date, path);
    QByteArray sigRaw = QMessageAuthenticationCode::hash(
        signatureOrigin.toUtf8(), apiSecret.toUtf8(), QCryptographicHash::Sha256);
    QString signature = QString::fromLatin1(sigRaw.toBase64());

    /* 字段间不带空格。讯飞官方 demo 写的是 ", "（逗号后有空格），这里没有——
       这一整串会被 base64 编码后再传，服务端解出来按逗号切分并 trim，两种写法
       都能过。留个记号：如果哪天鉴权莫名失败而这里的编码又确认没问题，
       把分隔符改成 ", " 对齐官方示例是下一个该试的点 */
    QString authOrigin = QString(
        "api_key=\"%1\",algorithm=\"hmac-sha256\",headers=\"host date request-line\",signature=\"%2\"")
        .arg(apiKey, signature);
    QString authorization = QString::fromLatin1(authOrigin.toUtf8().toBase64());

    QUrl url;
    url.setScheme("wss");
    url.setHost(host);
    url.setPath(path);

    /* 三个查询参数必须自己做完整的百分号编码，不能交给 QUrlQuery::addQueryItem。
     *
     * 原因：authorization 是 base64，字符集里含 '+' '/' '='。QUrlQuery 只编码它
     * 认为在查询串里"非法"的字符，而 '+' 在查询串里是合法字符，它原样保留。
     * 但按 application/x-www-form-urlencoded 的约定，查询串里的 '+' 解码后是
     * 【空格】——服务端还原出来的 authorization 就跟我们签名时用的那一串不是
     * 同一个字符串了，HMAC 校验必然失败。
     *
     * 现象是握手直接被拒、返回 401 Unauthorized，跟"密钥填错了"一模一样，
     * 极难往编码上想。而且它是概率性的：只有当 base64 里恰好出现 '+' 才会犯，
     * authorization 有两百多字符，中招率接近 100%，所以表现为稳定失败。
     *
     * QUrl::toPercentEncoding 只保留 unreserved 字符（A-Za-z0-9-._~），'+' '/'
     * '=' 空格逗号统统会被编码，正是这里需要的。编好之后用 setQuery 的
     * StrictMode 传进去——那个重载接收的就是【已编码】的查询串，不会再编一遍，
     * 所以不存在双重编码的问题。 */
    auto enc = [](const QString &s) {
        return QString::fromLatin1(QUrl::toPercentEncoding(s));
    };
    QString query = "authorization=" + enc(authorization)
                  + "&date="         + enc(date)
                  + "&host="         + enc(host);
    url.setQuery(query, QUrl::StrictMode);
    return url.toString(QUrl::FullyEncoded);
}

} // namespace XfyunAuth
