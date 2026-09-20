#ifndef VEHICLEPAGE_H
#define VEHICLEPAGE_H

#include <QWidget>

class QLabel;
class SpeedGauge;

/* VehiclePage - 车辆状态仪表盘。
 *
 * 数据全部来自已有的 CAN 上报，这一页不新增任何采集：
 *   车速  ← 0x102 的左右轮转速换算（编码器实测，不是给定值）
 *   里程  ← 0x102 的左右轮累计里程
 *   电量  ← 0x101 buf[7] 的电池电压
 *
 * ── 关于续航估算的诚实说明 ──
 * 这块车上**没有电流传感器**，只有电压。所以续航只能"按电压查表估剩余
 * 电量百分比，再乘一个标称续航"——它无法反映实际负载：空载和爬坡的
 * 耗电速度差很远，而电压法看不见这个差别。
 *
 * 因此界面上写的是"约"，并且把电压原值一起显示出来。
 * **一个估算值如果不标明它是估算，用户就会当成实测值用**——
 * 续航这种数字尤其危险，看着还剩 30 分钟结果 10 分钟就趴窝，
 * 比压根不显示更糟。
 */
class VehiclePage : public QWidget
{
    Q_OBJECT
public:
    explicit VehiclePage(QWidget *parent = nullptr);

    /* 0x102：左右轮转速(RPM，正=前进) + 累计里程(cm) */
    void updateMotor(int rpmL, int rpmR, int odomL, int odomR);
    /* 0x101 的 bat 字段，单位 V。**0 表示 ADC 线未接**，不是 0V */
    void updateBattery(double volt);
    /* 传感器健康位，用来显示电机驱动异常/飞车保护已触发 */
    void updateHealth(int hf);

    /* RTK/GNSS 状态。fix: 0无 1单点 2差分 4RTK固定 5RTK浮动。
       放在车辆页而不是首页：定位质量是个**车辆能力**指标——
       RTK 固定解时才能跑厉害的路径跟踪，单点解只能粗略导航。 */
    void updateGps(int fix, const QString &fixText, int sat, double hdop, double speedKmh);

private:
    SpeedGauge *m_gauge;
    QLabel *m_odom, *m_rpmL, *m_rpmR;
    QLabel *m_battPct, *m_battVolt, *m_range, *m_battBarHint;
    QLabel *m_motorState;
    QLabel *m_fixText, *m_satHdop, *m_gpsSpeed;
};

#endif // VEHICLEPAGE_H
