#ifndef NOTIFICATIONBANNER_H
#define NOTIFICATIONBANNER_H

#include <QWidget>
#include <QQueue>

class QLabel;
class QPropertyAnimation;
class QTimer;
class StatusIcon;

/* NotificationBanner - 顶部通知横幅，车机中控那种"从顶部滑下一条提示，
 * 停几秒再收起"的交互。跟 Web 端右上角堆叠的 Toast 不是同一套实现（Widgets
 * 没有等价的浮层堆叠机制，这里改成横幅内嵌在布局里、用 maximumHeight 动画
 * 展开收起），但产品语义一样：都是"临时的、不打断当前操作的提示"。
 *
 * 多条通知排队显示，不是后一条直接覆盖前一条——现场巡检时一次可能触发
 * 好几条阈值告警（比如温度和距离同时超限），全都要让操作者看到，不能因为
 * 顶掉了前一条而漏看。 */
class NotificationBanner : public QWidget
{
    Q_OBJECT
public:
    explicit NotificationBanner(QWidget *parent = nullptr);

    void show(const QString &title, const QString &msg, const QString &level);
    // level: "info" | "warn" | "danger"

private:
    void showNext();
    void dismissCurrent();

    StatusIcon *m_icon;
    QLabel *m_title, *m_msg;
    QPropertyAnimation *m_anim;
    QTimer *m_dismissTimer;
    QQueue<QStringList> m_queue;   // 每项 {title, msg, level}
    bool m_showing = false;
};

#endif // NOTIFICATIONBANNER_H
