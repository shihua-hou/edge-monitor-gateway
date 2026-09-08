#ifndef ATTITUDEWIDGET_H
#define ATTITUDEWIDGET_H

#include <QWidget>

/* AttitudeWidget - 姿态水平仪（人工地平仪），自绘控件。
 *
 * Web 端那版是用 CSS transform 做的，Qt Widgets 没有等价的声明式变换，
 * 这里用 QPainter 手画：地平线按 roll 旋转、按 pitch 上下平移，中心固定一个
 * 十字准心代表机体本身，右上角一个小罗盘按 yaw 转指针——是航空仪表那个经典
 * "球在动、飞机不动"的画法，不是我发明的，是这类姿态仪表的标准呈现方式。 */
class AttitudeWidget : public QWidget
{
    Q_OBJECT
public:
    explicit AttitudeWidget(QWidget *parent = nullptr);
    void setAttitude(double pitchDeg, double rollDeg, double yawDeg);
    /* IMU 失联时置 true：画面整体压暗并盖一条提示。
       姿态仪表尤其需要这个——地平线停在某个角度不动，看起来跟"车停着没动"
       完全一样，是最容易被误读成正常的一种故障 */
    void setStale(bool stale);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    double m_pitch = 0, m_roll = 0, m_yaw = 0;
    bool m_stale = false;
};

#endif // ATTITUDEWIDGET_H
