#ifndef XFYUNAUTH_H
#define XFYUNAUTH_H

#include <QString>

/* xfyunauth - 科大讯飞 WebSocket 系列 API（语音听写 IAT / 语音合成 TTS /
 * 星火大模型 Spark）统一用的鉴权算法，三个接口的签名方式完全一样，
 * 只是 host/path 不同，所以抽成一个公共函数，不用在三个 client 里各写一遍。
 *
 * 算法（讯飞文档"WebAPI 接口鉴权"）：
 *   date = 当前 UTC 时间，RFC1123 格式
 *   signature_origin = "host: " + host + "\n" + "date: " + date + "\n" +
 *                       "GET " + path + " HTTP/1.1"
 *   signature = Base64( HMAC-SHA256(signature_origin, APISecret) )
 *   authorization_origin = 'api_key="'+APIKey+'", algorithm="hmac-sha256", '
 *                           'headers="host date request-line", signature="'+signature+'"'
 *   authorization = Base64(authorization_origin)
 *   最终 URL = wss://host + path + "?authorization=" + urlEncode(authorization)
 *              + "&date=" + urlEncode(date) + "&host=" + host
 */
namespace XfyunAuth {
    // host 不带协议前缀，比如 "iat-api.xfyun.cn"；path 比如 "/v2/iat"
    QString buildWssUrl(const QString &host, const QString &path,
                         const QString &apiKey, const QString &apiSecret);
}

#endif // XFYUNAUTH_H
