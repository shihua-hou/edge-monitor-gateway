#include "mappage.h"
#include "iostheme.h"
#include "ioscard.h"

#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QUrlQuery>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QtMath>
#include <QTimer>
#include <QSizePolicy>

/* ================= 坐标转换 =================
 *
 * WGS-84 -> GCJ-02。国内的公开地图服务按法规必须用 GCJ-02，
 * 而 GNSS 输出的是 WGS-84，两者在南京一带差 300~500 米。
 *
 * 这是业界通用的近似实现（官方算法不公开），误差 1~5 米。
 * 对"车在地图上大概哪儿"足够；但要注意：**RTK 的厘米级精度过了这一步
 * 就被打掉两个数量级**。所以路径录制/回放用的是本地 ENU 平面（见 pathpage），
 * 不走这条转换——那里精度是要紧的，这里只是给人看个位置。
 */
static const double kGcjA  = 6378245.0;
static const double kGcjEE = 0.00669342162296594323;

static bool outOfChina(double lat, double lon)
{
    return (lon < 72.004 || lon > 137.8347 || lat < 0.8293 || lat > 55.8271);
}

static double transformLat(double x, double y)
{
    double r = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y
               + 0.2 * qSqrt(qFabs(x));
    r += (20.0 * qSin(6.0 * x * M_PI) + 20.0 * qSin(2.0 * x * M_PI)) * 2.0 / 3.0;
    r += (20.0 * qSin(y * M_PI) + 40.0 * qSin(y / 3.0 * M_PI)) * 2.0 / 3.0;
    r += (160.0 * qSin(y / 12.0 * M_PI) + 320.0 * qSin(y * M_PI / 30.0)) * 2.0 / 3.0;
    return r;
}

static double transformLon(double x, double y)
{
    double r = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y
               + 0.1 * qSqrt(qFabs(x));
    r += (20.0 * qSin(6.0 * x * M_PI) + 20.0 * qSin(2.0 * x * M_PI)) * 2.0 / 3.0;
    r += (20.0 * qSin(x * M_PI) + 40.0 * qSin(x / 3.0 * M_PI)) * 2.0 / 3.0;
    r += (150.0 * qSin(x / 12.0 * M_PI) + 300.0 * qSin(x / 30.0 * M_PI)) * 2.0 / 3.0;
    return r;
}

static void wgs84ToGcj02(double lat, double lon, double *oLat, double *oLon)
{
    if (outOfChina(lat, lon)) { *oLat = lat; *oLon = lon; return; }
    double dLat = transformLat(lon - 105.0, lat - 35.0);
    double dLon = transformLon(lon - 105.0, lat - 35.0);
    double radLat = lat / 180.0 * M_PI;
    double magic = qSin(radLat);
    magic = 1 - kGcjEE * magic * magic;
    double sqrtMagic = qSqrt(magic);
    dLat = (dLat * 180.0) / ((kGcjA * (1 - kGcjEE)) / (magic * sqrtMagic) * M_PI);
    dLon = (dLon * 180.0) / (kGcjA / sqrtMagic * qCos(radLat) * M_PI);
    *oLat = lat + dLat;
    *oLon = lon + dLon;
}

/* ================= Web 墨卡托 =================
   缩放级 z 下整个世界是 256*2^z 像素见方。
   经纬度 -> 世界像素，是所有瓦片地图的公共底座。 */
static QPointF lonLatToWorld(double lon, double lat, int z)
{
    double n = 256.0 * qPow(2.0, z);
    double x = (lon + 180.0) / 360.0 * n;
    double s = qSin(lat * M_PI / 180.0);
    if (s > 0.9999) s = 0.9999;
    if (s < -0.9999) s = -0.9999;
    double y = (0.5 - qLn((1 + s) / (1 - s)) / (4 * M_PI)) * n;
    return QPointF(x, y);
}

static void worldToLonLat(const QPointF &p, int z, double *lon, double *lat)
{
    double n = 256.0 * qPow(2.0, z);
    *lon = p.x() / n * 360.0 - 180.0;
    double t = M_PI * (1 - 2.0 * p.y() / n);
    *lat = 180.0 / M_PI * qAtan(0.5 * (qExp(t) - qExp(-t)));
}

/* ================= 瓦片画布 ================= */
class MapCanvas : public QWidget
{
    Q_OBJECT
public:
    explicit MapCanvas(QNetworkAccessManager *net, QWidget *parent = nullptr)
        : QWidget(parent), m_net(net)
    {
        /* 最小高度给小，靠 Expanding 吃剩余空间。
           屏幕只有 600px 高，而软键盘是**布局成员**（不是浮层），
           一弹出就要走两百来像素——画布再要 300px 的话，整窗的
           最小尺寸就超过屏高了。Qt 不会把窗口缩到最小尺寸以下，
           结果是窗口比屏幕还大，下半部分直接被裁在屏幕外。
           chatpage.cpp 里 m_scroll->setMinimumHeight(0) 是同一个道理。 */
        setMinimumHeight(120);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setCursor(Qt::OpenHandCursor);
        /* 默认落在南航明故宫校区。没有定位时总得有个地方，
           空着一片灰比什么都不显示更让人以为坏了。 */
        m_lat = 32.0357; m_lon = 118.8036; m_zoom = 15;
        m_cacheDir = QDir::homePath() + "/.cache/em_tiles";
        QDir().mkpath(m_cacheDir);
    }

    void setCenter(double lat, double lon) { m_lat = lat; m_lon = lon; update(); }
    void setZoom(int z) { m_zoom = qBound(4, z, 18); update(); }
    int  zoom() const { return m_zoom; }
    double centerLat() const { return m_lat; }
    double centerLon() const { return m_lon; }

    void setVehicle(bool have, double lat, double lon)
    { m_haveVeh = have; m_vLat = lat; m_vLon = lon; update(); }

    void setMarkers(const QVector<QPointF> &pts, int highlight)
    { m_markers = pts; m_hl = highlight; update(); }

    void setRoute(const QVector<QPointF> &pts) { m_route = pts; update(); }
    void clearRoute() { m_route.clear(); update(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRectF r = rect();
        QPainterPath clip = Ios::squircle(r, 14);
        p.setClipPath(clip);
        p.fillPath(clip, QColor(0x1A, 0x1A, 0x1E));

        const QPointF c = lonLatToWorld(m_lon, m_lat, m_zoom);
        const QPointF origin(c.x() - r.width() / 2.0, c.y() - r.height() / 2.0);

        int x0 = (int)qFloor(origin.x() / 256.0);
        int y0 = (int)qFloor(origin.y() / 256.0);
        int x1 = (int)qFloor((origin.x() + r.width()) / 256.0);
        int y1 = (int)qFloor((origin.y() + r.height()) / 256.0);
        int maxIdx = (1 << m_zoom) - 1;
        int drawn = 0;

        for (int tx = x0; tx <= x1; tx++) {
            for (int ty = y0; ty <= y1; ty++) {
                if (tx < 0 || ty < 0 || tx > maxIdx || ty > maxIdx) continue;
                QPixmap pm = tile(tx, ty, m_zoom);
                QPointF at(tx * 256.0 - origin.x(), ty * 256.0 - origin.y());
                if (pm.isNull()) {
                    /* 还没到的瓦片画个占位格，而不是留黑洞——
                       留黑洞时人分不清"正在加载"和"这里没有地图"。 */
                    p.fillRect(QRectF(at, QSizeF(256, 256)), QColor(0x23, 0x23, 0x28));
                    p.setPen(QPen(QColor(255, 255, 255, 12), 1));
                    p.drawRect(QRectF(at, QSizeF(256, 256)));
                } else {
                    p.drawPixmap(at, pm);
                    drawn++;
                }
            }
        }

        if (drawn == 0) {
            p.setPen(Ios::labelSecondary());
            p.setFont(Ios::fontFootnote());
            p.drawText(r, Qt::AlignCenter, QString::fromUtf8(
                "底图加载中…\n联不上网时这里只有网格，搜索和路线会置灰"));
        }

        /* 路线画两层：先粗的深色描边，再细的亮色。
           不描边的话，线压在深色路面上几乎看不见。 */
        if (m_route.size() > 1) {
            QPainterPath path;
            bool first = true;
            for (int i = 0; i < m_route.size(); i++) {
                QPointF w = lonLatToWorld(m_route[i].x(), m_route[i].y(), m_zoom);
                QPointF pt(w.x() - origin.x(), w.y() - origin.y());
                if (first) { path.moveTo(pt); first = false; } else path.lineTo(pt);
            }
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0, 0, 0, 140), 9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPath(path);
            p.setPen(QPen(Ios::blue(), 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPath(path);
        }

        /* POI 图钉。m_markers 里 x=lon、y=lat */
        for (int i = 0; i < m_markers.size(); i++) {
            QPointF w = lonLatToWorld(m_markers[i].x(), m_markers[i].y(), m_zoom);
            QPointF pt(w.x() - origin.x(), w.y() - origin.y());
            if (!r.adjusted(-40, -60, 40, 40).contains(pt)) continue;
            drawPin(p, pt, i == m_hl ? Ios::orange() : Ios::red(), i == m_hl);
        }

        /* 车辆：蓝点 + 白圈 + 半透明光晕。
           和 POI 图钉的形状刻意做得完全不同——同一张图上两种含义的标记
           长得像，是看图时最容易误读的地方。 */
        if (m_haveVeh) {
            QPointF w = lonLatToWorld(m_vLon, m_vLat, m_zoom);
            QPointF pt(w.x() - origin.x(), w.y() - origin.y());
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(10, 132, 255, 50));
            p.drawEllipse(pt, 18, 18);
            p.setBrush(Qt::white);
            p.drawEllipse(pt, 9, 9);
            p.setBrush(Ios::blue());
            p.drawEllipse(pt, 6, 6);
        }

        /* 比例尺。没有它，图上一段距离是 50 米还是 5 公里完全看不出来。 */
        {
            double mPerPx = 156543.03392 * qCos(m_lat * M_PI / 180.0) / qPow(2.0, m_zoom);
            double want = 80 * mPerPx;
            double step = qPow(10, qFloor(qLn(want) / qLn(10.0)));
            double nice = step;
            if (want / step >= 5) nice = step * 5;
            else if (want / step >= 2) nice = step * 2;
            int px = (int)(nice / mPerPx);
            int bx = (int)r.left() + 14, by = (int)r.bottom() - 16;
            /* 先铺一块半透明深色底托。白线直接画在高德的浅色底图上
               几乎看不见——而比例尺恰恰是**必须看得见**的那种信息：
               没有它，图上一段距离是 50 米还是 5 公里完全无从判断。 */
            QRectF badge(bx - 8, by - 26, px + 74, 36);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 130));
            p.drawRoundedRect(badge, 8, 8);
            p.setPen(QPen(QColor(255, 255, 255, 230), 2));
            p.drawLine(bx, by, bx + px, by);
            p.drawLine(bx, by - 4, bx, by + 4);
            p.drawLine(bx + px, by - 4, bx + px, by + 4);
            p.setFont(Ios::fontCaption());
            /* 用 'f' 不用 'g'：'g' 会在超过有效位数时切成科学计数法，
               200 米被写成 "2e+02"——比例尺上出现科学计数法是纯粹的噪音。 */
            p.drawText(QRect(bx, by - 22, 160, 18), Qt::AlignLeft,
                       nice >= 1000 ? QString::number(nice / 1000, 'f', nice >= 10000 ? 0 : 1) + " km"
                                    : QString::number(nice, 'f', 0) + " m");
        }
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        m_drag = true; m_last = e->pos();
        setCursor(Qt::ClosedHandCursor);
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (!m_drag) return;
        QPoint d = e->pos() - m_last;
        m_last = e->pos();
        QPointF c = lonLatToWorld(m_lon, m_lat, m_zoom);
        c -= QPointF(d.x(), d.y());
        worldToLonLat(c, m_zoom, &m_lon, &m_lat);
        update();
    }
    void mouseReleaseEvent(QMouseEvent *) override
    {
        m_drag = false;
        setCursor(Qt::OpenHandCursor);
    }

private:
    static void drawPin(QPainter &p, const QPointF &tip, const QColor &col, bool big)
    {
        double rr = big ? 9.0 : 7.0;
        QPointF ctr(tip.x(), tip.y() - rr * 2.0);
        QPainterPath pin;
        pin.moveTo(tip);
        pin.quadTo(QPointF(tip.x() - rr * 1.3, ctr.y() + rr * 0.6), QPointF(ctr.x() - rr, ctr.y()));
        pin.arcTo(QRectF(ctr.x() - rr, ctr.y() - rr, rr * 2, rr * 2), 180, -180);
        pin.quadTo(QPointF(tip.x() + rr * 1.3, ctr.y() + rr * 0.6), tip);
        p.setPen(QPen(QColor(0, 0, 0, 120), 1));
        p.setBrush(col);
        p.drawPath(pin);
        p.setBrush(Qt::white);
        p.setPen(Qt::NoPen);
        p.drawEllipse(ctr, rr * 0.42, rr * 0.42);
    }

    QPixmap tile(int x, int y, int z)
    {
        const QString key = QString("%1/%2/%3").arg(z).arg(x).arg(y);
        QHash<QString, QPixmap>::const_iterator it = m_mem.find(key);
        if (it != m_mem.end()) return it.value();

        /* 磁盘缓存不设过期：同一个 z/x/y 的瓦片是**不变内容**。
           板子上网不稳，跑过一次的区域下次离线也能看，这比"保持最新"重要得多。 */
        const QString path = QString("%1/%2_%3_%4.png").arg(m_cacheDir).arg(z).arg(x).arg(y);
        if (QFile::exists(path)) {
            QPixmap pm(path);
            if (!pm.isNull()) { insertMem(key, pm); return pm; }
        }

        if (!m_pending.contains(key)) {
            m_pending.insert(key);
            /* 四个域名轮流用：浏览器时代的老做法，在这里同样有效——
               单域名的并发连接数有限，一屏十几张图排队会明显卡顿。 */
            QString url = QString("https://webrd0%1.is.autonavi.com/appmaptile"
                                  "?lang=zh_cn&size=1&scale=1&style=8&x=%2&y=%3&z=%4")
                          .arg((qAbs(x + y) % 4) + 1).arg(x).arg(y).arg(z);
            QNetworkRequest req((QUrl(url)));
            req.setHeader(QNetworkRequest::UserAgentHeader, QByteArray("EdgeMonitor/1.0"));
            QNetworkReply *rep = m_net->get(req);
            rep->setProperty("tileKey", key);
            rep->setProperty("tilePath", path);
            connect(rep, &QNetworkReply::finished, this, &MapCanvas::onTile);
        }
        return QPixmap();
    }

    void insertMem(const QString &k, const QPixmap &pm)
    {
        /* 内存缓存上限。一屏约 12 张，留 200 张够来回拖几屏；
           不设上限的话连续平移十分钟就能吃掉几十 MB——这块板子总共 512MB。 */
        if (m_mem.size() > 200) m_mem.clear();
        m_mem.insert(k, pm);
    }

private slots:
    void onTile()
    {
        QNetworkReply *rep = qobject_cast<QNetworkReply *>(sender());
        if (!rep) return;
        rep->deleteLater();
        const QString key = rep->property("tileKey").toString();
        m_pending.remove(key);
        if (rep->error() != QNetworkReply::NoError) return;
        QByteArray data = rep->readAll();
        QPixmap pm;
        if (!pm.loadFromData(data)) return;
        QFile f(rep->property("tilePath").toString());
        if (f.open(QIODevice::WriteOnly)) { f.write(data); f.close(); }
        insertMem(key, pm);
        update();
    }

private:
    QNetworkAccessManager *m_net;
    QHash<QString, QPixmap> m_mem;
    QSet<QString> m_pending;
    QString m_cacheDir;

    double m_lat, m_lon;
    int    m_zoom;
    bool   m_drag = false;
    QPoint m_last;

    bool   m_haveVeh = false;
    double m_vLat = 0, m_vLon = 0;

    QVector<QPointF> m_markers;   /* x=lon, y=lat */
    int m_hl = -1;
    QVector<QPointF> m_route;     /* x=lon, y=lat */
};

/* ================= 页面 ================= */
namespace {

/* 高德接口的坐标串统一是 "lon,lat"，小数点后 6 位。
   位数少了定位会飘几十米，多了接口会拒——这是接口文档明确要求的。 */
QString amapCoord(double lon, double lat)
{
    return QString("%1,%2").arg(lon, 0, 'f', 6).arg(lat, 0, 'f', 6);
}

/* 解析 "lon,lat" */
bool parseAmapCoord(const QString &s, double *lon, double *lat)
{
    const QStringList parts = s.split(',');
    if (parts.size() != 2) return false;
    bool ok1 = false, ok2 = false;
    double a = parts[0].toDouble(&ok1);
    double b = parts[1].toDouble(&ok2);
    if (!ok1 || !ok2) return false;
    *lon = a; *lat = b;
    return true;
}

} // namespace

MapPage::MapPage(QWidget *parent) : QWidget(parent)
{
    m_net = new QNetworkAccessManager(this);

    /* ── 布局思路：学高德 App，地图铺满整页，控件浮在地图之上 ──
     *
     * 原来是"左边一列面板 + 右边一块地图"的后台管理布局。问题不是不好看，
     * 是**把最该大的东西做小了**：地图页上唯一重要的是地图，搜索框和结果
     * 只在你用它们的那几秒钟重要。左右分栏把 300px 宽——将近三分之一的
     * 屏幕——永久划给了一个大部分时间空着的面板。
     *
     * 浮层的代价是不能用 QLayout 自动排版，得在 resizeEvent 里手动定位。
     * 这点代码换来的是：地图从 505px 宽变成 1024px 满屏。
     */
    m_canvas = new MapCanvas(m_net, this);

    /* ---- 浮层：搜索条 ---- */
    m_searchBar = new QWidget(this);
    m_searchBar->setObjectName("searchBar");
    m_searchBar->setStyleSheet(
        "#searchBar{background:rgba(28,28,30,235);border-radius:14px;}");
    {
        QHBoxLayout *h = new QHBoxLayout(m_searchBar);
        h->setContentsMargins(8, 8, 8, 8);
        h->setSpacing(8);

        m_search = new QLineEdit(m_searchBar);
        m_search->setPlaceholderText("搜索地点");
        m_search->setMinimumHeight(38);
        m_search->setFont(Ios::fontBody());
        m_search->setStyleSheet(QString(
            "QLineEdit{background:%1;color:%2;border:none;border-radius:10px;padding:0 12px;}"
            "QLineEdit:focus{background:%3;}")
            .arg(QColor(0x2C, 0x2C, 0x2E).name())
            .arg(Ios::label().name())
            .arg(QColor(0x3A, 0x3A, 0x3C).name()));
        h->addWidget(m_search, 1);

        m_searchBtn = new QPushButton("搜索", m_searchBar);
        m_searchBtn->setMinimumHeight(38);
        m_searchBtn->setFixedWidth(64);
        m_searchBtn->setFont(Ios::fontBody());
        m_searchBtn->setStyleSheet(QString(
            "QPushButton{background:%1;color:white;border:none;border-radius:10px;font-weight:600;}"
            "QPushButton:pressed{background:%2;}"
            "QPushButton:disabled{background:%3;color:%4;}")
            .arg(Ios::blue().name()).arg(Ios::blue().darker(120).name())
            .arg(QColor(0x2C, 0x2C, 0x2E).name()).arg(Ios::gray().name()));
        h->addWidget(m_searchBtn);
    }

    /* ---- 浮层：结果列表（只在有结果时出现） ---- */
    m_resultPanel = new QWidget(this);
    m_resultPanel->setObjectName("resPanel");
    m_resultPanel->setStyleSheet(
        "#resPanel{background:rgba(28,28,30,235);border-radius:14px;}");
    {
        QVBoxLayout *v = new QVBoxLayout(m_resultPanel);
        v->setContentsMargins(8, 8, 8, 8);
        v->setSpacing(6);

        m_results = new QListWidget(m_resultPanel);
        m_results->setFont(Ios::fontFootnote());
        m_results->setFrameShape(QFrame::NoFrame);
        m_results->setMinimumHeight(0);
        m_results->setStyleSheet(QString(
            "QListWidget{background:transparent;color:%1;}"
            "QListWidget::item{padding:8px 8px;border-radius:9px;}"
            "QListWidget::item:selected{background:%2;color:white;}")
            .arg(Ios::label().name())
            .arg(QColor(10, 132, 255, 140).name(QColor::HexArgb)));
        v->addWidget(m_results, 1);

        m_routeBtn = new QPushButton("到这里去", m_resultPanel);
        m_routeBtn->setMinimumHeight(38);
        m_routeBtn->setFont(Ios::fontBody());
        m_routeBtn->setStyleSheet(QString(
            "QPushButton{background:%1;color:white;border:none;border-radius:10px;font-weight:600;}"
            "QPushButton:pressed{background:%2;}"
            "QPushButton:disabled{background:%3;color:%4;}")
            .arg(Ios::green().name()).arg(Ios::green().darker(120).name())
            .arg(QColor(0x2C, 0x2C, 0x2E).name()).arg(Ios::gray().name()));
        m_routeBtn->setEnabled(false);
        v->addWidget(m_routeBtn);
    }
    m_resultPanel->hide();

    /* ---- 浮层：状态条（搜索中 / 报错 / 提示） ---- */
    m_status = new QLabel(this);
    m_status->setObjectName("mapStatus");
    m_status->setWordWrap(true);
    m_status->setFont(Ios::fontFootnote());
    m_status->setStyleSheet(
        "#mapStatus{background:rgba(28,28,30,235);border-radius:12px;"
        "padding:10px 14px;color:#EBEBF5;}");
    m_status->hide();

    /* ---- 浮层：右侧圆形工具按钮 ---- */
    QPushButton *zoomIn  = roundButton("＋");
    QPushButton *zoomOut = roundButton("－");
    m_locBtn = roundButton("车");
    m_zoomIn = zoomIn;
    m_zoomOut = zoomOut;

    /* ---- 浮层：底部路线卡 ---- */
    m_routeCard = new QWidget(this);
    m_routeCard->setObjectName("routeCard");
    m_routeCard->setStyleSheet(
        "#routeCard{background:rgba(28,28,30,240);border-radius:14px;}");
    {
        QHBoxLayout *h = new QHBoxLayout(m_routeCard);
        h->setContentsMargins(16, 10, 10, 10);
        h->setSpacing(12);
        m_routeInfo = new QLabel("", m_routeCard);
        m_routeInfo->setFont(Ios::fontBody());
        m_routeInfo->setStyleSheet(QString("color:%1;").arg(Ios::label().name()));
        h->addWidget(m_routeInfo, 1);
        m_clearBtn = new QPushButton("清除", m_routeCard);
        m_clearBtn->setMinimumHeight(34);
        m_clearBtn->setFixedWidth(64);
        m_clearBtn->setFont(Ios::fontFootnote());
        m_clearBtn->setStyleSheet(QString(
            "QPushButton{background:%1;color:%2;border:none;border-radius:9px;}"
            "QPushButton:pressed{background:%3;}")
            .arg(QColor(0x3A, 0x3A, 0x3C).name()).arg(Ios::label().name())
            .arg(QColor(0x48, 0x48, 0x4A).name()));
        h->addWidget(m_clearBtn);
    }
    m_routeCard->hide();

    connect(m_searchBtn, &QPushButton::clicked, this, &MapPage::doSearch);
    connect(m_search, &QLineEdit::returnPressed, this, &MapPage::doSearch);
    connect(m_routeBtn, &QPushButton::clicked, this, &MapPage::planRouteToSelected);
    connect(m_results, &QListWidget::currentRowChanged, this, &MapPage::onResultClicked);
    connect(zoomIn,  &QPushButton::clicked, this, [this]() { m_canvas->setZoom(m_canvas->zoom() + 1); });
    connect(zoomOut, &QPushButton::clicked, this, [this]() { m_canvas->setZoom(m_canvas->zoom() - 1); });
    connect(m_clearBtn, &QPushButton::clicked, this, [this]() {
        m_canvas->clearRoute();
        m_routeCard->hide();
    });
    connect(m_locBtn, &QPushButton::clicked, this, [this]() {
        if (!m_haveVehicle) {
            setStatus("车辆还没有定位。RTK/GNSS 没有有效解时，地图不会假装知道车在哪。");
            return;
        }
        m_canvas->setCenter(m_vLat, m_vLon);
    });
}

/* 圆形浮动按钮。高德那几个悬浮控件的形状——在满屏地图上，
   方角面板会把视线切断，圆形不会。 */
QPushButton *MapPage::roundButton(const QString &text)
{
    QPushButton *b = new QPushButton(text, this);
    b->setFixedSize(46, 46);
    b->setFont(Ios::fontBody());
    b->setStyleSheet(QString(
        "QPushButton{background:rgba(28,28,30,235);color:%1;border:none;border-radius:23px;"
        "font-weight:600;}"
        "QPushButton:pressed{background:rgba(58,58,60,240);}")
        .arg(Ios::label().name()));
    return b;
}

/* 浮层没有 QLayout 管，位置在这里统一算。
   全部相对边角定位，所以换分辨率不用改代码。 */
void MapPage::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    const int W = width(), H = height();
    const int M = 14;                 /* 统一外边距 */
    const int barW = qMin(420, W - 2 * M - 70);

    m_canvas->setGeometry(0, 0, W, H);

    m_searchBar->setGeometry(M, M, barW, 54);

    /* 结果面板贴在搜索条下面，高度按剩余空间收敛，最多占到屏高的六成——
       再高就把地图挡没了，而人来这一页是为了看地图。 */
    const int panelTop = M + 54 + 8;
    const int panelH = qMin(int(H * 0.6), H - panelTop - M - 70);
    m_resultPanel->setGeometry(M, panelTop, barW, qMax(120, panelH));

    m_status->setGeometry(M, panelTop, barW, m_status->heightForWidth(barW) + 20);

    /* 右侧竖排：放大 / 缩小 / 定位到车 */
    const int bx = W - M - 46;
    m_zoomIn->move(bx, H / 2 - 80);
    m_zoomOut->move(bx, H / 2 - 26);
    m_locBtn->move(bx, H / 2 + 42);

    /* 底部路线卡居中 */
    const int cardW = qMin(520, W - 2 * M);
    m_routeCard->setGeometry((W - cardW) / 2, H - M - 56, cardW, 56);
}

QString MapPage::amapKey() const
{
    /* 密钥只从设备本地的 QSettings 读，**不写进源码**：
       这个仓库是公开的，而 git 历史删不干净。
       和讯飞、和风的密钥走同一套存放方式。 */
    QSettings s("EdgeMonitor", "edgemonitor_ui");
    return s.value("amap/webKey").toString().trimmed();
}

void MapPage::setStatus(const QString &text, bool busy)
{
    /* 状态条是浮层，压在结果面板的位置上。空文本就收起来——
       常驻一个写着"输入关键词后点搜索"的条子，等于永久占掉一块地图。 */
    m_searchBtn->setEnabled(!busy);
    if (text.isEmpty()) { m_status->hide(); return; }
    m_status->setText(text);
    m_status->show();
    m_status->raise();
    /* 文本长度变了高度也要变，重新走一次定位 */
    resizeEvent(nullptr);
}

void MapPage::updateVehicle(double lat, double lon, int fix)
{
    if (fix <= 0 || (qFuzzyIsNull(lat) && qFuzzyIsNull(lon))) {
        m_haveVehicle = false;
        m_canvas->setVehicle(false, 0, 0);
        return;
    }
    /* GNSS 给的是 WGS-84，高德的瓦片是 GCJ-02，差 300~500 米。
       不转的话车会画到隔壁街区去，而且看着完全像"定位不准"。 */
    wgs84ToGcj02(lat, lon, &m_vLat, &m_vLon);
    m_haveVehicle = true;
    m_canvas->setVehicle(true, m_vLat, m_vLon);
}

void MapPage::doSearch()
{
    const QString kw = m_search->text().trimmed();
    if (kw.isEmpty()) { setStatus("先输入要搜什么"); return; }

    const QString key = amapKey();
    if (key.isEmpty()) {
        setStatus("没有配置高德 Web 服务 Key。到「设置」里填一个——"
                  "注意要的是 Web 服务 Key，不是 JS API Key，两者不通用。");
        return;
    }

    QUrl url("https://restapi.amap.com/v3/place/text");
    QUrlQuery q;
    q.addQueryItem("key", key);
    q.addQueryItem("keywords", kw);
    q.addQueryItem("offset", "20");
    q.addQueryItem("page", "1");
    q.addQueryItem("extensions", "base");
    /* 以**当前地图中心**为圆心搜附近，而不是全国搜。
       不带 location 的话搜"加油站"会返回北京的结果，
       在车载场景下这是彻底没用的答案。 */
    q.addQueryItem("location", amapCoord(m_canvas->centerLon(), m_canvas->centerLat()));
    url.setQuery(q);

    setStatus("搜索中…", true);
    QNetworkReply *rep = m_net->get(QNetworkRequest(url));
    connect(rep, &QNetworkReply::finished, this, [this, rep]() { onSearchReply(rep); });
}

void MapPage::onSearchReply(QNetworkReply *reply)
{
    reply->deleteLater();
    setStatus("", false);

    if (reply->error() != QNetworkReply::NoError) {
        setStatus(QString("搜索失败：%1").arg(reply->errorString()));
        return;
    }

    const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
    /* 高德用 status 字段表示成败，**HTTP 状态码永远是 200**——
       只看 HTTP 码会把"key 无效""配额用完"当成成功，然后得到一个空列表，
       现象是"搜什么都没结果"，很难往鉴权上想。 */
    if (o.value("status").toString() != "1") {
        setStatus(QString("高德返回错误：%1（%2）")
                  .arg(o.value("info").toString())
                  .arg(o.value("infocode").toString()));
        return;
    }

    m_pois.clear();
    m_results->clear();
    m_status->hide();
    const QJsonArray pois = o.value("pois").toArray();
    QVector<QPointF> marks;
    for (int i = 0; i < pois.size(); i++) {
        const QJsonObject pj = pois.at(i).toObject();
        double lon = 0, lat = 0;
        if (!parseAmapCoord(pj.value("location").toString(), &lon, &lat)) continue;
        Poi poi;
        poi.name = pj.value("name").toString();
        poi.addr = pj.value("address").toString();
        poi.lat = lat; poi.lon = lon;
        m_pois.append(poi);
        marks.append(QPointF(lon, lat));
        m_results->addItem(QString("%1\n%2").arg(poi.name, poi.addr));
    }

    if (m_pois.isEmpty()) {
        setStatus("没搜到结果。换个关键词，或把地图拖到目标城市再搜——"
                  "搜索是以当前地图中心为圆心的。");
        m_canvas->setMarkers(QVector<QPointF>(), -1);
        m_resultPanel->hide();
        return;
    }

    m_canvas->setMarkers(marks, -1);
    m_canvas->setCenter(m_pois[0].lat, m_pois[0].lon);
    m_resultPanel->show();
    m_resultPanel->raise();
    m_status->hide();
}

void MapPage::onResultClicked(int row)
{
    if (row < 0 || row >= m_pois.size()) {
        m_selected = -1;
        m_routeBtn->setEnabled(false);
        return;
    }
    m_selected = row;
    m_canvas->setCenter(m_pois[row].lat, m_pois[row].lon);
    QVector<QPointF> marks;
    for (int i = 0; i < m_pois.size(); i++) marks.append(QPointF(m_pois[i].lon, m_pois[i].lat));
    m_canvas->setMarkers(marks, row);
    m_routeBtn->setEnabled(true);
}

void MapPage::planRouteToSelected()
{
    if (m_selected < 0 || m_selected >= m_pois.size()) return;
    if (!m_haveVehicle) {
        setStatus("车辆还没有定位，算不了从车到目的地的路线。"
                  "RTK/GNSS 拿到解之后再试。");
        return;
    }
    const QString key = amapKey();
    if (key.isEmpty()) { setStatus("没有配置高德 Web 服务 Key"); return; }

    QUrl url("https://restapi.amap.com/v3/direction/driving");
    QUrlQuery q;
    q.addQueryItem("key", key);
    /* 起点用**已经转成 GCJ-02 的车辆坐标**。
       高德的接口和瓦片都是 GCJ-02，这里再转一次就是转了两次——
       和不转一样错，只是错的方向相反。 */
    q.addQueryItem("origin", amapCoord(m_vLon, m_vLat));
    q.addQueryItem("destination", amapCoord(m_pois[m_selected].lon, m_pois[m_selected].lat));
    q.addQueryItem("extensions", "all");
    q.addQueryItem("strategy", "0");
    url.setQuery(q);

    setStatus("规划路线中…", true);
    QNetworkReply *rep = m_net->get(QNetworkRequest(url));
    connect(rep, &QNetworkReply::finished, this, [this, rep]() { onRouteReply(rep); });
}

void MapPage::onRouteReply(QNetworkReply *reply)
{
    reply->deleteLater();
    setStatus("", false);

    if (reply->error() != QNetworkReply::NoError) {
        setStatus(QString("路线请求失败：%1").arg(reply->errorString()));
        return;
    }
    const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
    if (o.value("status").toString() != "1") {
        setStatus(QString("高德返回错误：%1").arg(o.value("info").toString()));
        return;
    }

    const QJsonArray paths = o.value("route").toObject().value("paths").toArray();
    if (paths.isEmpty()) { setStatus("没有可行路线"); return; }

    const QJsonObject path0 = paths.at(0).toObject();
    const QJsonArray steps = path0.value("steps").toArray();

    /* 每个 step 的 polyline 是 "lon,lat;lon,lat;..."，
       把所有 step 首尾接起来就是整条路线。 */
    QVector<QPointF> line;
    for (int i = 0; i < steps.size(); i++) {
        const QStringList pts = steps.at(i).toObject().value("polyline").toString().split(';');
        for (int j = 0; j < pts.size(); j++) {
            double lon = 0, lat = 0;
            if (parseAmapCoord(pts[j], &lon, &lat)) line.append(QPointF(lon, lat));
        }
    }
    if (line.size() < 2) { setStatus("路线数据为空"); return; }

    m_canvas->setRoute(line);

    const double dist = path0.value("distance").toString().toDouble();     /* 米 */
    const double dur  = path0.value("duration").toString().toDouble();     /* 秒 */
    m_routeCard->show();
    m_routeCard->raise();
    m_routeInfo->setText(QString("到「%1」  %2  约 %3 分钟")
                         .arg(m_pois[m_selected].name)
                         .arg(dist >= 1000 ? QString::number(dist / 1000, 'f', 1) + " km"
                                           : QString::number(dist, 'f', 0) + " m")
                         .arg(qRound(dur / 60.0)));

    /* 把整条路线框进视野：只对准起点的话，路线一长就只能看见个头。
       这里用最朴素的做法——算包围盒，按跨度反推缩放级。 */
    double minLon = line[0].x(), maxLon = line[0].x();
    double minLat = line[0].y(), maxLat = line[0].y();
    for (int i = 1; i < line.size(); i++) {
        minLon = qMin(minLon, line[i].x()); maxLon = qMax(maxLon, line[i].x());
        minLat = qMin(minLat, line[i].y()); maxLat = qMax(maxLat, line[i].y());
    }
    m_canvas->setCenter((minLat + maxLat) / 2, (minLon + maxLon) / 2);
    double spanLon = qMax(1e-5, maxLon - minLon);
    int z = 18;
    while (z > 4 && spanLon * 256.0 * qPow(2.0, z) / 360.0 > m_canvas->width() * 0.85) z--;
    m_canvas->setZoom(z);

    /* 路线卡已经把结果说清楚了，不再叠一条状态提示挡地图。
       "这是导航建议、不是自动驾驶路径"这句话放在页面说明里，
       不该每算一次路线就弹一次。 */
    m_status->hide();
}

#include "mappage.moc"
