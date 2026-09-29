#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
web_server.py - Web 大屏的静态文件服务 + 视频中转

── 为什么不再用 python3 -m http.server ──

原来大屏只是两个静态文件，视频由浏览器**直接去连板子的 IP** 拉 MJPEG。
这要求"看大屏的那台设备"能直接访问到板子，实测这条路很脆：

  1. 浏览器越来越严格地限制"一个网页去访问局域网里另一台设备"
     （Chrome/Edge 的 Private/Local Network Access）。实测同一个 Chromium：
     页面从 127.0.0.1 打开时视频能加载，从 fly260305.local 或
     192.168.100.6 打开时，去 192.168.100.3 的请求被浏览器当场拦掉。
  2. 远程访问时根本到不了：通过 Tailscale 在外面打开大屏，
     手机能访问服务器，但访问不到板子所在局域网的 192.168.100.3。
  3. 板子换一次 IP，浏览器里的地址就失效，要等下一条上线通告。

改成**同源中转**：浏览器只访问 /video（和页面同一个地址），
由服务器去板子取流再转发过来。浏览器从头到尾只跟一个地址打交道，
上面三条问题一起消失。

── 安全：这个接口不能变成开放代理 ──

/video 接收一个 host 参数。不加限制的话，任何能访问大屏的人都能让服务器
替他去连任意地址（SSRF）——包括服务器所在内网的其它机器、云厂商的元数据接口。
所以：
  - 只接受**字面 IP**，不接受主机名（主机名解析结果可以被操纵，
    DNS rebinding 就是这么绕过检查的）
  - 只允许**私有地址**（10/8、172.16/12、192.168/16）
  - 端口**写死 8081**，调用方改不了
这样它最多只能被用来看"局域网里某台设备 8081 端口上的内容"，
而那本来就是给局域网看的视频。

── Python 版本 ──

这台 VM 是 3.6.9：SimpleHTTPRequestHandler 的 directory 参数、
ThreadingHTTPServer 都是 3.7 才有的。这里用 os.chdir + ThreadingMixIn，
两个版本都能跑。

── 为什么必须多线程 ──

一路视频流是一条**永不结束**的 HTTP 响应。单线程服务器被它占住之后，
连 index.html 都发不出来了——打开一次大屏，整个网站就卡死。
"""
import http.server
import ipaddress
import os
import socketserver
import sys
import urllib.parse
import urllib.request

PORT = int(os.environ.get('EM_WEB_PORT', '8080'))
WEB_DIR = os.environ.get('EM_WEB_DIR',
                         os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'web'))
VIDEO_PORT = 8081          # 板子上 video_v4l2 的端口，写死，调用方改不了
CONNECT_TIMEOUT = 5        # 连不上板子就快速失败，让前端去走重连逻辑


class Handler(http.server.SimpleHTTPRequestHandler):
    # 静态文件访问日志太吵（每秒好几条），只记视频中转；错误一律记
    def log_message(self, fmt, *args):
        if getattr(self, 'path', '').startswith('/video'):
            sys.stderr.write('[web] %s %s\n' % (self.address_string(), fmt % args))

    def log_error(self, fmt, *args):
        sys.stderr.write('[web] ERROR %s %s\n' % (self.address_string(), fmt % args))

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        if u.path == '/video':
            return self.relay_video(urllib.parse.parse_qs(u.query))
        return super().do_GET()

    def end_headers(self):
        # 页面和脚本不缓存：改了前端之后，浏览器还拿旧的 app.js，
        # 表现是"改了没生效"，而且每个人的浏览器状态不一样，非常难排查。
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

    def relay_video(self, q):
        host = (q.get('host') or [''])[0].strip()
        token = (q.get('token') or [''])[0]

        try:
            ip = ipaddress.ip_address(host)       # 只认字面 IP，主机名直接抛异常
        except ValueError:
            # send_error 的第二个参数会写进 HTTP **状态行**，状态行只能是 latin-1，
            # 放中文会在编码时抛异常、连接直接被掐断——客户端看到的不是 400，
            # 而是"服务器莫名其妙断开"。中文说明放第三个参数，那是响应体（UTF-8）。
            return self.send_error(400, 'Bad Request', 'host 必须是 IP 地址')
        if ip.version != 4 or not ip.is_private or ip.is_loopback:
            # 回环也拒：否则可以借它访问服务器自己本机上的其它服务
            return self.send_error(403, 'Forbidden', '只允许中转到局域网私有地址')

        url = 'http://%s:%d/?action=stream&token=%s' % (
            host, VIDEO_PORT, urllib.parse.quote(token, safe=''))
        try:
            upstream = urllib.request.urlopen(url, timeout=CONNECT_TIMEOUT)
        except Exception as e:
            # 502 而不是 500：服务器自己没坏，是后面的设备没接上。
            # 前端的 <img> 会走 onerror，进入"N 秒后自动重连"。
            return self.send_error(502, 'Bad Gateway',
                                   '连不上设备视频 %s:%d（%s）' % (host, VIDEO_PORT, type(e).__name__))

        self.send_response(200)
        self.send_header('Content-Type',
                         upstream.headers.get('Content-Type', 'multipart/x-mixed-replace; boundary=frame'))
        self.end_headers()

        # 原样转发字节流，不解析 MJPEG：解析它没有任何好处，
        # 中间多一道处理只会多一处可能把数据改坏的地方。
        try:
            while True:
                chunk = upstream.read(16384)
                if not chunk:
                    break
                self.wfile.write(chunk)
        except (BrokenPipeError, ConnectionResetError):
            pass            # 浏览器关了页面，正常
        except Exception as e:
            sys.stderr.write('[web] 视频中转中断 %s: %s\n' % (host, e))
        finally:
            upstream.close()


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True        # 进程退出时不等那些永不结束的视频线程
    allow_reuse_address = True   # 重启时不必等 TIME_WAIT 过去


def main():
    os.chdir(os.path.abspath(WEB_DIR))
    srv = Server(('0.0.0.0', PORT), Handler)
    sys.stderr.write('[web] 静态文件 %s，端口 %d，视频中转 /video -> <局域网IP>:%d\n'
                     % (os.getcwd(), PORT, VIDEO_PORT))
    srv.serve_forever()


if __name__ == '__main__':
    main()
