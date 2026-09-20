#ifndef PINYINIME_H
#define PINYINIME_H

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

/* PinyinIME - 一个够用就好的拼音候选引擎。
 *
 * ── 为什么不用 Qt 自带的 ──
 * 板子的 rootfs 里其实带了 Qt VirtualKeyboard 和它的拼音插件，试过了：
 * 它是 QtQuick 的独立窗口，在 600px 高的屏幕上几乎占满，正文完全被挡住；
 * 语言列表一长串，而这台设备只需要中/英两种。杀鸡用牛刀，还没杀成
 * （选了简体中文候选词也出不来）。
 *
 * ── 词库怎么来的 ──
 * scripts/gen_pinyin_dict.py 离线生成：jieba 的词频表提供**词频**，
 * pypinyin 提供读音。词频是关键——没有它，打 "ni" 第一个跳出来的
 * 可能是个生僻字，那这个输入法就没法用。
 * 成品 610KB，33488 个拼音键，每个键最多 6 个候选。
 *
 * ── 为什么不在内存里建哈希表 ──
 * 要做的是**按前缀查**：输入 ni 要能同时找到 ni / nihao / nihen…
 * 哈希表只能精确匹配，做前缀还得另建结构。
 * 而词库文件本身是按拼音排好序的，于是：整文件读进一个 QByteArray，
 * 再存一份每行起始偏移，二分一次就定位到前缀区间。
 * 代价是 610KB + 33488×4B 的偏移表 ≈ 745KB，
 * 而把它拆成 QString 建表要 3~5MB——这块板子总共 494MB。
 */
class PinyinIME
{
public:
    /* 加载词库。失败返回 false（文件不在、格式不对），
       上层据此把中文键置灰，而不是让人按了没反应。 */
    bool load(const QString &path);
    bool isLoaded() const { return !m_lineStart.isEmpty(); }

    /* 查候选：先精确匹配，再按前缀补。
       例如输入 "ni" → 你 尼 泥 呢 … 你好 你们 …
       max 之外的不返回——候选条一行放不下，返回再多也是浪费。 */
    QStringList candidates(const QString &pinyin, int max = 9) const;

private:
    QByteArray  keyAt(int line) const;
    int         lowerBound(const QByteArray &key) const;

    QByteArray     m_data;
    QVector<int>   m_lineStart;   /* 每行在 m_data 里的起始偏移 */
};

#endif // PINYINIME_H
