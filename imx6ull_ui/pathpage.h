#ifndef PATHPAGE_H
#define PATHPAGE_H

#include <QWidget>
#include <QVector>
#include <QPointF>

class QLabel;
class QPushButton;
class QTimer;
class TrackView;

/* PathPage - 巡检路线录制 / 回放（**动作流**方案）。
 *
 * ── 为什么不用航迹推算 ──
 * 第一版录的是"推算出来的位置"：里程 + IMU 偏航积分出 x/y，回放时用纯追踪
 * 去追这串点。实测下来轨迹和真实路线对不上，正方形闭不回去。
 * 根因不是一个 bug，是**误差链太长**：
 *   偏航来自陀螺积分（无磁力计，会漂）× 轮径/轮距系数没标定 ×
 *   轮子打滑 —— 三项误差乘在一起，再拿这个错的位置去做闭环跟踪，
 *   等于用一把不准的尺子量完再照着它走一遍，错上加错。
 *
 * 现在改成录**车实际怎么动的**，回放时原样重放：
 *   [ 左轮35% 右轮35% 持续2.1s ][ 左轮-25% 右轮25% 持续0.6s ] ...
 * 位置估计彻底退出关键路径 —— 回放准不准只取决于车本身重不重复，
 * 而不取决于我们算得准不准。
 *
 * ── 录的是**指令流**，不是实测转速 ──
 * 第二版录的是 0x102 上报的实测转速，实测转角对不上。原因是采样：
 * 0x102 固定 **200ms** 一帧（用 candump -t d 量过），而人点一下转向
 * 只有 300~500ms —— 三次采样都不到，转向时长被量化到 200ms 的格子上，
 * 误差 20~30%；采到的还都是 PI 爬坡途中的转速（比指令低），
 * 于是回放的转角**系统性偏小**。
 *
 * 现在直接录指令本身：指令是阶跃的，没有"中间值"可采错，
 * 时刻精确到毫秒，数值就是原值。
 * 尤其是 ACT_LEFT/RIGHT —— 它的**转向时长烧在固件里**(p2=ms)，
 * 一条指令就是一次完整转向；原样重发等于让固件重跑同一段代码，
 * 转角自然对得上。而按转速录会把这条信息彻底碾碎。
 *
 * 指令从哪来：网关在 CAN 总线上回显 0x110（见 gateway_mqtt.c）。
 * 车有两个控制入口 —— Qt UI 走 MQTT，手机控制页在板子上直接写 CAN，
 * 总线是两者唯一的汇合点，在那里录才录得全。
 *
 * ── 这个方案的诚实边界 ──
 * 回放是**开环**的：不看反馈、不修偏差。同样的动作序列，
 * 电量掉了、地面换了、载重变了，走出来的就是另一条线，
 * 而且误差会一路累积没人拉回来。
 * 所以它适合"演示一段固定巡检路线"，不适合"精确回到某个点"。
 * 等 RTK 通了有真实位置，闭环跟踪（纯追踪）才有意义 —— 那时候位置是**测**
 * 出来的，不是**算**出来的，误差链才短得下来。
 *
 * 图上画两条线：蓝色是实时推算位置（仅供参考，会漂），
 * 灰色是把录下来的动作流按差速运动学积分出来的**标称路线**。
 */
class PathPage : public QWidget
{
    Q_OBJECT
public:
    explicit PathPage(QWidget *parent = nullptr);

    /* 0x102。位移用**有符号转速**算，不用里程——
     * 固件里程是取绝对值累加的（对"累计行驶里程"而言正确），
     * 拿它做位移会在**原地转向**时出大错：左轮倒转右轮正转，
     * 两边里程都在增加，平均下来 ds>0——每转一次弯轨迹就凭空往前窜一段。
     * 而有符号转速在原地转时 vL = -vR，平均正好是 0。
     * 里程参数仍然保留，只用来显示"累计行驶"。 */
    /* dispL/dispR 是固件累加的**有符号位移**(mm，协议 v2.6)。
       haveDisp=false 表示下位机是老固件、没有 0x103 帧，
       这时退回用有符号转速积分——精度差一些但不会不可用。 */
    void updateMotion(int rpmL, int rpmR, int odomL, int odomR,
                      int dispL, int dispR, bool haveDisp);
    /* 0x101：偏航角(度) */
    void updateYaw(double yawDeg);
    /* 命令回显（网关从 CAN 总线上抓的 0x110，见 gateway_mqtt.c）。
       ts 是网关侧的单调时钟毫秒。录制录的就是这个流。 */
    void onCmdEcho(qint64 ts, int dev, int act, int p1, int p2);

signals:
    /* 回放时下发的差速指令。走 MainWindow 统一的 publishCmd，
       不在这个页面里自己开 MQTT——一个进程里两个发布路径
       迟早会因为连接状态不一致而出怪事。 */
    void cmdRequested(int dev, int act, int p1, int p2);

private:
    /* 录下来的一条原始指令。回放时原样重发，
       连动作码都不翻译——翻译一次就多一次走样的机会。 */
    struct CmdSeg { qint64 ts; int act, p1, p2; };

    void updateStats();

    void startReplay();
    void stopReplay(const QString &why);
    void replayTick();
    /* 把动作流按差速运动学积分成一条标称路线，画在图上 */
    void rebuildNominal();
    void resetPose();

    TrackView *m_view;
    QPushButton *m_recBtn, *m_clearBtn, *m_playBtn;
    QLabel *m_stats, *m_hint;

    /* 实时推算状态（仅用于画蓝色参考线和显示，不参与录制/回放） */
    bool   m_havePrev = false;
    qint64 m_prevMs = 0;          /* 上一帧到达时刻，用来算实际 dt */
    bool   m_haveDisp = false;    /* 见 updateMotion */
    int    m_prevDispL = 0, m_prevDispR = 0;
    int    m_prevOdo = 0;         /* 上一次的左右轮平均里程(cm)，仅用于"累计行驶" */
    double m_yawRaw = 0;          /* IMU 原始偏航(弧度) */
    double m_yawRef = 0;          /* 零度基准，录制/回放开始时取当前朝向 */
    double m_yaw = 0;             /* 相对基准的偏航(弧度) */
    double m_x = 0, m_y = 0;      /* 当前位置(米)，起点为原点 */
    double m_dist = 0;            /* 累计行驶距离(米) */
    qint64 m_startMs = 0;

    /* 录制 */
    bool   m_recording = false;
    QVector<CmdSeg> m_segs;
    qint64 m_recT0 = 0;           /* 第一条指令的 ts，段偏移都相对它算 */

    /* 回放 */
    bool    m_replaying = false;
    QTimer *m_replayTimer = nullptr;
    qint64  m_replayT0 = 0;
    int     m_playIdx = 0;        /* 下一条待发的指令下标 */
    int     m_sentIdx = -1;       /* 已发出的最后一条，用于保活重发 */
};

#endif // PATHPAGE_H
