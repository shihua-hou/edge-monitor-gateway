#include <QApplication>
#include <QSplashScreen>
#include <QPixmap>
#include <QPainter>
#include <QLinearGradient>
#include <QTimer>
#include "mainwindow.h"

/* 开机动画：车机中控开机大多有个几百毫秒到一两秒的品牌启动画面，一是遮住
   界面初始化那一瞬间可能的白屏/控件逐个出现的过程，二是这种终端设备该有的
   "仪式感"。用 QSplashScreen（Qt 自带、专门干这个的类）实现，没有自己另外
   拼一个顶层窗口 + 定时器 + 手动关闭的必要。 */
static QPixmap buildSplashPixmap()
{
    QPixmap pm(480, 300);
    pm.fill(QColor("#0a0e15"));
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    QLinearGradient g(190, 90, 290, 190);
    g.setColorAt(0, QColor("#3ddc97"));
    g.setColorAt(1, QColor("#5aa9ff"));
    p.setBrush(g);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(190, 80, 100, 100, 26, 26);

    p.setPen(QColor("#04140d"));
    QFont f = p.font();
    f.setPointSize(30); f.setBold(true);
    p.setFont(f);
    p.drawText(QRect(190, 80, 100, 100), Qt::AlignCenter, "EM");

    p.setPen(QColor("#e6e6e6"));
    f.setPointSize(16); f.setBold(true);
    p.setFont(f);
    p.drawText(QRect(0, 200, 480, 36), Qt::AlignCenter, "EdgeMonitor 车载中控终端");

    p.setPen(QColor("#8a94a6"));
    f.setPointSize(10); f.setBold(false);
    p.setFont(f);
    p.drawText(QRect(0, 240, 480, 26), Qt::AlignCenter, "正在启动…");

    return pm;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QSplashScreen splash(buildSplashPixmap());
    splash.show();
    app.processEvents();

    MainWindow w;   // 构造过程里会去连 MQTT/加载配置，正好在开机动画挡着的时候做

    QTimer::singleShot(1100, &splash, [&splash, &w]() {
        /* 全屏跑，不带窗口装饰——这是装在巡检设备上的固定操作台，不是桌面应用，
           不需要标题栏/最小化/关闭按钮，也不用关心面板实际分辨率跟设计时假设的
           800x480 是否完全一致（布局用的是 layout 自适应，不是绝对像素定位） */
        w.showFullScreen();
        splash.finish(&w);
    });

    return app.exec();
}
