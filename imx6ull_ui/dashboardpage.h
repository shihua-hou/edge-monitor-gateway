#ifndef DASHBOARDPAGE_H
#define DASHBOARDPAGE_H

#include <QWidget>

class QLabel;
class AttitudeWidget;

/* DashboardPage - 中控首页：车载中控风格的"座舱概览"。
 * 大时钟 + 天气 + 传感器摘要 + 姿态水平仪，一眼看清当前设备状态和环境信息，
 * 跟 Web 大屏那个信息量拉满的仪表盘不是一回事——这是给现场操作者扫一眼用的。 */
class DashboardPage : public QWidget
{
    Q_OBJECT
public:
    explicit DashboardPage(QWidget *parent = nullptr);

    void updateStatus(double temp, double humi, int light, int dist, int stateBits);
    void updateImu(double pitch, double roll, double yaw);
    /* 传感器健康位（固件 app_task.h 的 SENS_FAULT_*，经 MQTT 的 hf 字段送达）。
       置 1 的那一路显示的是陈旧值，界面上必须让人看出来——否则传感器坏了，
       屏幕上照样是个稳定的数字，现场操作者不会有任何察觉 */
    void updateSensorHealth(int healthBits);
    /* city 非空时显示成 "杭州 · 阴"。定位是 IP 推算的，有可能偏，
       不把地名显示出来的话，偏了也没人能发现 */
    void updateWeather(const QString &tempC, const QString &text, const QString &city = QString());
    void setWeatherError(const QString &msg);

private slots:
    void tickClock();

private:
    QLabel *m_clock, *m_dateLabel;
    QLabel *m_weatherText, *m_weatherTemp;
    QLabel *m_vTemp, *m_vHumi, *m_vLight, *m_vDist;
    int m_health = 0;   // 见 updateSensorHealth
    /* 把一个数值标成正常/陈旧两种样式。抽出来是因为四个数值的处理
       完全一样，写四遍只会让以后改样式时漏掉其中一个 */
    void applyValue(QLabel *lab, const QString &text, int faultBit);
    AttitudeWidget *m_attitude;
};

#endif // DASHBOARDPAGE_H
