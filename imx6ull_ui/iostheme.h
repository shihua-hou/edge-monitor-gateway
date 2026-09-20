#ifndef IOSTHEME_H
#define IOSTHEME_H

#include <QColor>
#include <QFont>
#include <QPainterPath>
#include <QRectF>
#include <QEasingCurve>

class QPainter;

/* iostheme - iOS 风格设计系统的中心定义。
 *
 * 为什么要有这么一层：iOS 的观感不是"圆角 + 半透明"这么简单，它是一套
 * 互相咬合的规则（语义色、字阶、连续曲率圆角、弹簧动画）。把这些散在
 * 各个页面里各写一遍，结果一定是每个页面都"有点像但又不一样"。
 *
 * ── 这块屏幕的两个硬约束，直接决定了下面的取舍 ──
 *
 * 1. **16bpp / RGB565**（/sys/class/graphics/fb0/bits_per_pixel = 16）
 *    只有 32 级红、64 级绿、32 级蓝。iOS 那种大面积微妙渐变在这里会出现
 *    肉眼可见的色带。所以：渐变只用在很短的距离上（卡片顶部 1~2px 高光），
 *    大面积一律用纯色 + alpha。
 *
 * 2. **linuxfb 纯软件渲染，单核 Cortex-A7**
 *    没有 GPU 合成。QGraphicsBlurEffect 对 1024x600 逐帧做高斯模糊
 *    是不可能的（实测会直接卡成幻灯片）。所以真毛玻璃做不了，
 *    用"半透明 + 极短渐变 + 顶部高光"去**仿**那个材质感。
 *
 *    这是个诚实的折中，不是偷懒：在这块硬件上，真模糊的代价是整个界面
 *    不可用，而仿出来的材质在 7 寸屏上肉眼几乎分不出。
 */
namespace Ios {

/* ── 语义色（深色模式）──
 * 命名照抄 iOS 的语义体系而不是 "gray1/gray2"：语义色的好处是换主题时
 * 只改这一处，而调用方写的是"次级标签"这种意图，不是"某个灰"。 */
QColor bg();                 /* systemBackground        最底层 */
QColor bgElevated();         /* secondarySystemBackground 卡片 */
QColor bgGrouped();          /* 分组列表的行背景 */
QColor label();              /* 主文字 */
QColor labelSecondary();     /* 次级文字 */
QColor labelTertiary();      /* 三级文字/占位 */
QColor separator();          /* 分隔线 */
QColor fill();               /* 控件填充（未激活的分段控件等） */

QColor blue();               /* systemBlue   主强调色 */
QColor green();              /* systemGreen  正常/成功 */
QColor red();                /* systemRed    危险 */
QColor orange();             /* systemOrange 警告 */
QColor gray();               /* systemGray */

/* ── 字阶 ──
 * 板子上没有 SF Pro，用系统中文字体但套用 iOS 的字号/字重节奏。
 * iOS 的层级感很大程度来自"字号差得足够开 + 字重对比"，
 * 而不是靠很多种字号。所以这里只给 6 档，不再多。 */
QFont fontLargeTitle();      /* 34 Bold   大标题 */
QFont fontTitle();           /* 22 Semibold */
QFont fontHeadline();        /* 17 Semibold 卡片标题 */
QFont fontBody();            /* 17 Regular */
QFont fontFootnote();        /* 13 Regular 辅助说明 */
QFont fontCaption();         /* 11 Regular 最小标注 */
/* 数值专用：等宽，避免数字跳动时整行宽度变化 */
QFont fontNumber(int px, bool bold = true);

/* ── 连续曲率圆角（squircle）──
 *
 * iOS 的圆角**不是正圆弧**。从 iOS 7 起用的是连续曲率（continuous corner），
 * 数学上接近超椭圆——曲率从直边到圆角是渐变的，而正圆弧在切点处曲率
 * 突然从 0 跳到 1/r。这个差别单看说不清，但并排一放，正圆弧会显得"鼓"、
 * 有点廉价，而 squircle 更"绷"。这是 iOS 观感里最容易被忽略、
 * 又最能拉开差距的一条。
 *
 * Qt 的 drawRoundedRect 只有正圆弧，所以这里用三次贝塞尔手工逼近。
 * 路径只在尺寸变化时算一次并缓存，不进逐帧路径。 */
QPainterPath squircle(const QRectF &r, qreal radius);

/* ── 动画 ──
 * iOS 的动效核心是**弹簧**，不是线性也不是普通缓动：位移接近目标时
 * 会有一点点过冲再回落。用 OutBack 逼近，overshoot 调小——
 * 车机上过冲太大会显得轻浮。 */
QEasingCurve spring();
int durFast();               /* 180ms 小控件 */
int durNormal();             /* 280ms 页面元素 */

/* ── 材质绘制 ──
 * 统一在这里画，页面只管调用。改材质时不用翻遍每个 paintEvent。 */
void paintCard(QPainter *p, const QRectF &r, qreal radius, bool pressed = false);
/* 分组列表的整块背景（iOS 设置页那种） */
void paintGroup(QPainter *p, const QRectF &r, qreal radius);

} // namespace Ios

#endif // IOSTHEME_H
