#ifndef MAPPAGE_H
#define MAPPAGE_H

#include <QWidget>
#include <QHash>
#include <QPointF>
#include <QVector>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QNetworkAccessManager;
class QNetworkReply;
class MapCanvas;

/* MapPage - 车载地图（搜索 / 路线 / 底图）
 *
 * ── 为什么是自己画，而不是嵌高德的 JS SDK ──
 * 板子是单核 Cortex-A7、软件渲染、没有 GPU，Qt WebEngine 在这块硬件上
 * 既编不出来也跑不动（Chromium 内核光内存就吃掉大半）。
 * 所以走原生路线：
 *   底图  自己按 Web 墨卡托算瓦片号，HTTP 拉栅格瓦片，QPainter 贴图
 *   搜索  高德 Web 服务 REST 接口，返回 JSON，自己渲染列表
 *   路线  同上，返回的 polyline 自己解析、自己画
 * 好处是完全可控：缓存策略、离线降级、渲染开销都在自己手里；
 * 代价是交互细节要一行行写（拖拽、惯性、点击命中）。
 *
 * ── 坐标系 ──
 * 高德的瓦片和接口**全部是 GCJ-02**（火星坐标）。而 RTK/GPS 给的是 WGS-84。
 * 两者在国内差 300~500 米，直接叠图车会跑到隔壁街区去。
 * 所以车辆位置画上去之前必须过一次 wgs84ToGcj02()。
 * 反过来，接口返回的 POI 和路线本身就是 GCJ-02，不用转——
 * **转两次和不转一样错**，这里最容易搞混。
 *
 * ── 离线时什么样 ──
 * 拉不到瓦片就画网格底纹 + 一行说明，搜索和路线按钮置灰。
 * 不弹错误框：车在园区里跑，网络断断续续是常态，
 * 每断一次弹一个框比没有地图更烦人。
 */
class MapPage : public QWidget
{
    Q_OBJECT
public:
    explicit MapPage(QWidget *parent = nullptr);

    /* 车辆位置（WGS-84）。fix<=0 时传进来也没关系，内部会忽略。 */
    void updateVehicle(double lat, double lon, int fix);

private slots:
    void doSearch();
    void onSearchReply(QNetworkReply *reply);
    void onRouteReply(QNetworkReply *reply);
    void onResultClicked(int row);
    void planRouteToSelected();

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    void setStatus(const QString &text, bool busy = false);
    QString amapKey() const;
    QPushButton *roundButton(const QString &text);

    MapCanvas   *m_canvas;
    /* 以下都是**浮在地图上**的部件，没有 QLayout 管，
       位置统一在 resizeEvent 里按边角算，见 mappage.cpp */
    QWidget     *m_searchBar, *m_resultPanel, *m_routeCard;
    QLineEdit   *m_search;
    QPushButton *m_searchBtn, *m_routeBtn, *m_clearBtn, *m_locBtn;
    QPushButton *m_zoomIn, *m_zoomOut;
    QListWidget *m_results;
    QLabel      *m_status, *m_routeInfo;

    QNetworkAccessManager *m_net;

    struct Poi { QString name, addr; double lat, lon; };
    QVector<Poi> m_pois;
    int m_selected = -1;

    bool   m_haveVehicle = false;
    double m_vLat = 0, m_vLon = 0;   /* 已转成 GCJ-02 */
};

#endif // MAPPAGE_H
