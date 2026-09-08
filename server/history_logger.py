#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
history_logger - 服务器侧历史数据落库 + 查询接口

为什么在服务器侧而不是机器人上：机器人存储有限、断电即丢，而且多台设备
本来就要汇总到一处。这个程序订阅 MQTT 落 SQLite，跟网关完全解耦——
网关一行代码都不用改，加设备也不用改这里。

为什么用 Python 而不是跟网关一样用 C：这是跑在服务器上的常驻服务，不是
嵌入式环境，没有内存/依赖的硬约束。sqlite3、http.server 都是标准库，
整个程序只额外依赖一个 paho-mqtt，比用 C 手写 HTTP + SQL 绑定省太多事，
也更好维护。工具要按场景选，不是全项目一种语言到底。

用法：
    pip3 install paho-mqtt
    python3 history_logger.py

环境变量（跟网关侧保持同一套命名）：
    MQTT_HOST / MQTT_USER / MQTT_PASS
    HIST_DB       SQLite 文件路径，默认 ./edgemonitor.db
    HIST_PORT     查询接口端口，默认 8090
    HIST_KEEP_DAYS 数据保留天数，默认 30

查询接口：
    GET /api/history?metric=temp&hours=6[&device=robot01][&limit=2000]
    GET /api/events?limit=100
    GET /api/devices
"""

import io
import json
import os
import sqlite3
import sys
import threading
import time
from datetime import datetime, timedelta
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

# ThreadingHTTPServer 是 Python 3.7 才加的，VM 上的 Python 更老。
# 用 ThreadingMixIn 手动拼一个，行为一样。必须是多线程的：单线程
# HTTPServer 一次只能处理一个请求，前端同时拉曲线和事件时会互相阻塞。
try:
    from http.server import ThreadingHTTPServer
except ImportError:
    from socketserver import ThreadingMixIn

    class ThreadingHTTPServer(ThreadingMixIn, HTTPServer):
        daemon_threads = True   # 主线程退出时不被工作线程拖住

try:
    import paho.mqtt.client as mqtt
except ImportError:
    raise SystemExit("缺少依赖，请先执行: sudo apt install python3-paho-mqtt")

# 行缓冲。这个服务常驻后台跑，stdout 都是重定向到日志文件的，而 Python
# 对非终端输出默认全缓冲——启动信息会一直卡在缓冲区里不落盘，看日志只见
# 空白，还以为服务没起来。等价于命令行加 -u，但不用每次都记得加。
# （网关那几个 C 程序也踩过同一个坑，那边是漏了 setvbuf。）
try:
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", line_buffering=True)
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", line_buffering=True)
except Exception:
    pass   # 某些环境下 stdout 没有 buffer 属性（比如被别的库包装过），不影响主流程

MQTT_HOST = os.getenv("MQTT_HOST", "localhost")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_USER = os.getenv("MQTT_USER", "")
MQTT_PASS = os.getenv("MQTT_PASS", "")
DB_PATH = os.getenv("HIST_DB", "./edgemonitor.db")
HTTP_PORT = int(os.getenv("HIST_PORT", "8090"))
KEEP_DAYS = int(os.getenv("HIST_KEEP_DAYS", "30"))

# 传感器数据 200ms 上报一次，全存的话一天 43 万条，纯属浪费——
# 曲线图上肉眼也分辨不出这个密度。按固定间隔降采样存储；
# 但"事件"（上下线、越界告警）是稀疏且重要的，一条都不能丢，全存。
SAMPLE_INTERVAL_S = 5

_db_lock = threading.Lock()


def db_connect():
    # check_same_thread=False + 全局锁：MQTT 回调线程和 HTTP 线程都要访问，
    # SQLite 本身支持并发读，写操作用锁串起来即可，这个数据量下够用
    conn = sqlite3.connect(DB_PATH, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    try:
        # WAL 模式：读写不再互斥。默认的 rollback journal 下，一次写入会阻塞
        # 所有读——前端拉 6 小时曲线（几千行）的同时正好有数据落库，两边就
        # 互相等。WAL 下读的是快照，写不挡读，这正是本程序的访问模式：
        # 一个写线程 + 若干个 HTTP 读线程。
        conn.execute("PRAGMA journal_mode=WAL")
        # NORMAL：每次事务不强制 fsync，靠 WAL 的检查点保证一致性。
        # 极端断电时最多丢最近几条采样——对降采样后的监控数据完全可以接受，
        # 换来的是写入快一个数量级（FULL 模式下每条 INSERT 都要等磁盘）。
        conn.execute("PRAGMA synchronous=NORMAL")
        # 等锁最多 5 秒再报错，而不是立刻抛 "database is locked"
        conn.execute("PRAGMA busy_timeout=5000")
    except Exception as e:
        print("[db] PRAGMA 设置失败（不影响功能，仅并发性能变差）: %s" % e)
    return conn


def init_db(conn):
    with _db_lock:
        conn.executescript("""
        CREATE TABLE IF NOT EXISTS sensor_history (
            ts INTEGER NOT NULL, device TEXT NOT NULL,
            temp REAL, humi REAL, light INTEGER, dist INTEGER, state INTEGER
        );
        CREATE TABLE IF NOT EXISTS imu_history (
            ts INTEGER NOT NULL, device TEXT NOT NULL,
            pitch REAL, roll REAL, yaw REAL
        );
        CREATE TABLE IF NOT EXISTS motor_history (
            ts INTEGER NOT NULL, device TEXT NOT NULL,
            rpm_l INTEGER, rpm_r INTEGER, odo_l INTEGER, odo_r INTEGER
        );
        CREATE TABLE IF NOT EXISTS gps_history (
            ts INTEGER NOT NULL, device TEXT NOT NULL,
            lat REAL, lon REAL, fix INTEGER, sat INTEGER, alt REAL, speed REAL
        );
        CREATE TABLE IF NOT EXISTS event_log (
            ts INTEGER NOT NULL, device TEXT NOT NULL,
            type TEXT, level TEXT, title TEXT, msg TEXT
        );
        -- 查询清一色是"某设备最近一段时间"，(device, ts) 复合索引正好命中
        CREATE INDEX IF NOT EXISTS idx_sensor  ON sensor_history(device, ts);
        CREATE INDEX IF NOT EXISTS idx_imu     ON imu_history(device, ts);
        CREATE INDEX IF NOT EXISTS idx_motor   ON motor_history(device, ts);
        CREATE INDEX IF NOT EXISTS idx_gps     ON gps_history(device, ts);
        CREATE INDEX IF NOT EXISTS idx_event   ON event_log(device, ts);
        """)
        conn.commit()


def now_ms():
    return int(time.time() * 1000)


class Logger:
    def __init__(self, conn):
        self.conn = conn
        self.last_sample = {}   # (device, kind) -> 上次落库时间，用于降采样
        self.last_state = {}    # device -> 上次的状态字，变化时立即落库
        self._last_err_at = 0    # 写库报错的限流时间戳，见 _insert

    def _should_sample(self, device, kind, force=False):
        key = (device, kind)
        t = time.time()
        if force or (t - self.last_sample.get(key, 0)) >= SAMPLE_INTERVAL_S:
            self.last_sample[key] = t
            return True
        return False

    def _insert(self, sql, args):
        # 必须自己兜住异常。这个方法是从 MQTT 回调线程里调的，抛出去之后
        # paho 只会打一行堆栈然后继续跑——数据没写进去，而日志里那行堆栈
        # 混在正常输出里很容易被忽略，表现为"历史曲线莫名其妙缺了一段"。
        # 磁盘满、库被锁、文件权限变了都会走到这里，必须说清楚是哪种。
        try:
            with _db_lock:
                self.conn.execute(sql, args)
                self.conn.commit()
        except Exception as e:
            now = time.time()
            # 出错往往是持续性的（磁盘满不会自己好），每条都打会把日志刷爆。
            # 限流成每 30 秒一条，既不淹没日志也不会让问题彻底无声
            if now - self._last_err_at > 30:
                self._last_err_at = now
                print("[db][error] 写入失败: %s（SQL: %s）" % (e, sql.split("(")[0].strip()))

    def on_status(self, device, d):
        # 状态字（LED/蜂鸣器/电机开关）一变就立刻记一条，
        # 否则降采样会把"什么时候开的灯"这种关键跳变吃掉
        state = d.get("s", 0)
        changed = self.last_state.get(device) != state
        self.last_state[device] = state
        if not self._should_sample(device, "status", force=changed):
            return
        self._insert(
            "INSERT INTO sensor_history(ts,device,temp,humi,light,dist,state) VALUES(?,?,?,?,?,?,?)",
            (now_ms(), device, d.get("t"), d.get("h"), d.get("l"), d.get("d"), state))

    def on_imu(self, device, d):
        if not self._should_sample(device, "imu"):
            return
        self._insert(
            "INSERT INTO imu_history(ts,device,pitch,roll,yaw) VALUES(?,?,?,?,?)",
            (now_ms(), device, d.get("p"), d.get("r"), d.get("y")))

    def on_motor(self, device, d):
        if not self._should_sample(device, "motor"):
            return
        self._insert(
            "INSERT INTO motor_history(ts,device,rpm_l,rpm_r,odo_l,odo_r) VALUES(?,?,?,?,?,?)",
            (now_ms(), device, d.get("rl"), d.get("rr"), d.get("ol"), d.get("or")))

    def on_gps(self, device, d):
        # GPS 本来就 1Hz 上报，且轨迹点稀疏了会断断续续，不降采样
        self._insert(
            "INSERT INTO gps_history(ts,device,lat,lon,fix,sat,alt,speed) VALUES(?,?,?,?,?,?,?,?)",
            (now_ms(), device, d.get("lat"), d.get("lon"), d.get("fix"),
             d.get("sat"), d.get("alt"), d.get("speed")))

    def on_event(self, device, etype, level, title, msg):
        self._insert(
            "INSERT INTO event_log(ts,device,type,level,title,msg) VALUES(?,?,?,?,?,?)",
            (now_ms(), device, etype, level, title, msg))
        print("[event] %s %s: %s - %s" % (device, level, title, msg))

    def purge_old(self):
        """删掉超过保留期的数据，免得库无限长大。"""
        cutoff = now_ms() - KEEP_DAYS * 86400 * 1000
        deleted = 0
        try:
            with _db_lock:
                for t in ("sensor_history", "imu_history", "motor_history", "gps_history", "event_log"):
                    cur = self.conn.execute("DELETE FROM %s WHERE ts < ?" % t, (cutoff,))
                    deleted += cur.rowcount if cur.rowcount and cur.rowcount > 0 else 0
                self.conn.commit()
                # DELETE 不会把文件缩回去，VACUUM 才会。但 VACUUM 是把整个库
                # 重写一遍，期间锁住数据库——没删掉任何东西时做这件事纯属白白
                # 卡住一次写入窗口，所以只在真的删了数据之后才做。
                if deleted > 0:
                    self.conn.execute("VACUUM")
        except Exception as e:
            print("[purge][error] 清理失败: %s" % e)
            return
        print("[purge] 已清理 %d 天前的数据，删除 %d 行" % (KEEP_DAYS, deleted))


# ---------------- MQTT ----------------

def topic_device(topic, default="robot01"):
    """
    从 topic 里取设备 id，支持两种结构：
        monitor/online/<id>        上线通告
        monitor/<id>/data/xxx      数据（v3 起带设备号）
    老结构 monitor/data/xxx 没有设备段，归到默认设备——留着这条是为了
    在滚动升级期间（网关已换、还没全部重启）不丢数据。
    """
    parts = topic.split("/")
    if len(parts) >= 3 and parts[0] == "monitor":
        if parts[1] == "online":
            return parts[2]
        if len(parts) >= 4 and parts[2] == "data":
            return parts[1]
    return default


def start_mqtt(logger):
    client = mqtt.Client(client_id="history_logger")
    if MQTT_USER:
        client.username_pw_set(MQTT_USER, MQTT_PASS)

    def on_connect(cli, userdata, flags, rc):
        if rc != 0:
            print("[mqtt] 连接被拒绝, rc=%d（检查账号密码）" % rc)
            return
        # 和网关侧同样的道理：订阅必须放在连接回调里，重连后才会恢复。
        # monitor/+/data/# 用通配符订阅所有设备（+ 匹配一层，正好是设备号）；
        # monitor/data/# 是老结构，滚动升级期间一并收着，避免丢数据
        cli.subscribe([("monitor/+/data/#", 0), ("monitor/data/#", 0), ("monitor/online/#", 1)])
        print("[mqtt] connected %s:%d, subscribed" % (MQTT_HOST, MQTT_PORT))

    def on_message(cli, userdata, msg):
        try:
            d = json.loads(msg.payload.decode("utf-8"))
        except Exception:
            return
        topic = msg.topic
        device = topic_device(topic)

        if topic.startswith("monitor/online/"):
            online = d.get("online") == 1
            logger.on_event(device, "presence", "info" if online else "danger",
                            "设备上线" if online else "设备离线",
                            json.dumps(d, ensure_ascii=False))
            return

        if topic.endswith("/status"):
            logger.on_status(device, d)
        elif topic.endswith("/imu"):
            logger.on_imu(device, d)
        elif topic.endswith("/motor"):
            logger.on_motor(device, d)
        elif topic.endswith("/gps"):
            logger.on_gps(device, d)

    client.on_connect = on_connect
    client.on_message = on_message
    # loop_start 起后台线程，自带断线重连
    client.connect_async(MQTT_HOST, MQTT_PORT, 60)
    client.loop_start()
    return client


# ---------------- HTTP 查询接口 ----------------

METRICS = {
    "temp":  ("sensor_history", "temp"),
    "humi":  ("sensor_history", "humi"),
    "light": ("sensor_history", "light"),
    "dist":  ("sensor_history", "dist"),
    "pitch": ("imu_history", "pitch"),
    "roll":  ("imu_history", "roll"),
    "yaw":   ("imu_history", "yaw"),
    "rpm_l": ("motor_history", "rpm_l"),
    "rpm_r": ("motor_history", "rpm_r"),
    "odo_l": ("motor_history", "odo_l"),
    "odo_r": ("motor_history", "odo_r"),
}


def make_handler(conn):
    class Handler(BaseHTTPRequestHandler):
        def _send(self, obj, code=200):
            body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            # Web 大屏和本服务不一定同源（大屏可能由 nginx 提供），要放行跨域
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            u = urlparse(self.path)
            q = parse_qs(u.query)
            one = lambda k, dv=None: q.get(k, [dv])[0]

            def clamp_int(name, default, lo, hi):
                """取整数参数并夹在 [lo, hi] 内。

                下界不能省：SQLite 里 `LIMIT -1` 的语义是【不限制】，
                所以 ?limit=-1 会把整张表一次吐出来——按 30 天保留、5 秒采样
                算是几十万行，服务端要在内存里拼一个巨大的 JSON，
                浏览器那边也接不住。原来只 min(x, 20000) 挡了上界，
                负数直接穿过去了。
                参数非法时返回 None，由调用方回 400，而不是让 int() 抛异常
                走到最外层 except 变成一个 500——那会让人以为是服务端崩了。"""
                raw = one(name, str(default))
                try:
                    v = int(float(raw))
                except (TypeError, ValueError):
                    return None
                return max(lo, min(v, hi))

            try:
                if u.path == "/api/history":
                    metric = one("metric", "temp")
                    if metric not in METRICS:
                        return self._send({"error": "unknown metric",
                                           "available": sorted(METRICS)}, 400)
                    table, col = METRICS[metric]
                    hours = clamp_int("hours", 6, 1, 24 * 90)      # 最多回看 90 天
                    limit = clamp_int("limit", 2000, 1, 20000)
                    if hours is None or limit is None:
                        return self._send({"error": "hours/limit 必须是数字"}, 400)
                    device = one("device", "robot01")
                    since = now_ms() - int(hours * 3600 * 1000)
                    with _db_lock:
                        rows = conn.execute(
                            "SELECT ts, %s AS v FROM %s WHERE device=? AND ts>=? "
                            "AND v IS NOT NULL ORDER BY ts DESC LIMIT ?" % (col, table),
                            (device, since, limit)).fetchall()
                    # 倒序取最近 limit 条，再翻回正序给前端画图
                    data = [{"ts": r["ts"], "v": r["v"]} for r in reversed(rows)]
                    return self._send({"metric": metric, "device": device, "points": data})

                if u.path == "/api/events":
                    limit = clamp_int("limit", 100, 1, 1000)
                    if limit is None:
                        return self._send({"error": "limit 必须是数字"}, 400)
                    with _db_lock:
                        rows = conn.execute(
                            "SELECT ts,device,type,level,title,msg FROM event_log "
                            "ORDER BY ts DESC LIMIT ?", (limit,)).fetchall()
                    return self._send({"events": [dict(r) for r in rows]})

                if u.path == "/api/devices":
                    with _db_lock:
                        rows = conn.execute(
                            "SELECT device, COUNT(*) n, MAX(ts) last_ts "
                            "FROM sensor_history GROUP BY device").fetchall()
                    return self._send({"devices": [dict(r) for r in rows]})

                return self._send({"error": "not found"}, 404)
            except Exception as e:
                return self._send({"error": str(e)}, 500)

        def log_message(self, fmt, *args):
            pass   # 默认每个请求打一行，曲线刷新时会刷屏，静音

    return Handler


def purge_loop(logger):
    while True:
        time.sleep(24 * 3600)
        try:
            logger.purge_old()
        except Exception as e:
            print("[purge] 失败:", e)


def acquire_single_instance_lock():
    """
    保证同一台机器上只跑一个实例。

    为什么需要：MQTT 不允许同一个 broker 上出现两个相同的 client_id，
    后连上的会把先连上的踢掉，被踢的一方自动重连、又把对方踢掉……形成
    无限互踢。日志上看是"连上、断开、连上、断开"反复刷，而两个进程都
    "在运行"，非常难往"跑了两份"这个方向想——这个坑在网关那边踩过一次。

    用 flock 而不是 PID 文件：进程被 kill -9 时 PID 文件会留下来变成
    "陈旧的锁"，下次启动就被自己拦住了；flock 由内核在进程退出时自动
    释放，怎么死的都不会留下残留。
    """
    try:
        import fcntl
    except ImportError:
        return None      # 非 Unix 平台（比如在 Windows 上调试），不强求
    path = os.getenv("HIST_LOCK", "/tmp/history_logger.lock")
    try:
        fp = open(path, "w")
        fcntl.flock(fp.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        fp.write(str(os.getpid()))
        fp.flush()
        return fp        # 返回值必须被调用方持有：文件对象被回收就等于解锁
    except IOError:
        raise SystemExit(
            "已经有一个 history_logger 在运行了（锁文件 %s）。\n"
            "两个实例会用同一个 MQTT client_id 互相踢下线，所以这里直接退出。\n"
            "确认要重启的话，先 kill 掉旧进程。" % path)


def main():
    lock = acquire_single_instance_lock()   # 必须用变量接住，见函数注释
    conn = db_connect()
    init_db(conn)
    logger = Logger(conn)
    logger.purge_old()

    start_mqtt(logger)
    threading.Thread(target=purge_loop, args=(logger,), daemon=True).start()

    srv = ThreadingHTTPServer(("0.0.0.0", HTTP_PORT), make_handler(conn))
    print("history_logger 已启动")
    print("  数据库   : %s (保留 %d 天, 采样间隔 %ds)" % (DB_PATH, KEEP_DAYS, SAMPLE_INTERVAL_S))
    print("  查询接口 : http://0.0.0.0:%d/api/history?metric=temp&hours=6" % HTTP_PORT)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n退出")
    finally:
        if lock:
            lock.close()   # 显式关闭，语义清楚；进程退出时内核也会自动释放


if __name__ == "__main__":
    main()
