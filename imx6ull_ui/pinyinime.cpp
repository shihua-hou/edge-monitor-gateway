#include "pinyinime.h"

#include <QFile>
#include <QDebug>

bool PinyinIME::load(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning("[pinyin] 打不开词库 %s：%s",
                 qPrintable(path), qPrintable(f.errorString()));
        return false;
    }
    m_data = f.readAll();
    f.close();

    m_lineStart.clear();
    m_lineStart.reserve(40000);
    int i = 0;
    const int n = m_data.size();
    while (i < n) {
        m_lineStart.append(i);
        int nl = m_data.indexOf('\n', i);
        if (nl < 0) break;
        i = nl + 1;
    }

    /* 词库必须是**按拼音升序**的，整个查找靠这个前提。
       生成脚本会排序，但文件是可以被手改的——这里抽查一下，
       乱序的话二分会静默返回错误结果，比直接报错难查得多。 */
    for (int k = 1; k < m_lineStart.size(); k += 997) {
        if (keyAt(k - 1) > keyAt(k)) {
            qWarning("[pinyin] 词库未按拼音排序（第 %d 行附近），查找会出错", k);
            m_lineStart.clear();
            m_data.clear();
            return false;
        }
    }

    qWarning("[pinyin] 词库已加载：%d 行，%d KB",
             m_lineStart.size(), m_data.size() / 1024);
    return true;
}

QByteArray PinyinIME::keyAt(int line) const
{
    const int s = m_lineStart.at(line);
    int e = m_data.indexOf(' ', s);
    if (e < 0) e = m_data.indexOf('\n', s);
    if (e < 0) e = m_data.size();
    return m_data.mid(s, e - s);
}

int PinyinIME::lowerBound(const QByteArray &key) const
{
    int lo = 0, hi = m_lineStart.size();
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (keyAt(mid) < key) lo = mid + 1;
        else                  hi = mid;
    }
    return lo;
}

QStringList PinyinIME::candidates(const QString &pinyin, int max) const
{
    QStringList out;
    if (pinyin.isEmpty() || m_lineStart.isEmpty()) return out;

    const QByteArray key = pinyin.toLatin1();
    int i = lowerBound(key);

    /* 从前缀区间的起点往后扫。因为文件按拼音排序，所有以 key 开头的行
       必然是连续的一段，扫到第一个不匹配就可以停。
       顺序上"精确匹配"天然排在最前（"ni" < "nia"），不用额外处理。 */
    while (i < m_lineStart.size() && out.size() < max) {
        const QByteArray k = keyAt(i);
        if (!k.startsWith(key)) break;

        const int s = m_lineStart.at(i);
        int e = m_data.indexOf('\n', s);
        if (e < 0) e = m_data.size();
        const QString line = QString::fromUtf8(m_data.constData() + s, e - s);

        const QStringList parts = line.split(' ', QString::SkipEmptyParts);
        for (int j = 1; j < parts.size() && out.size() < max; j++) {
            /* 同一个词可能从不同长度的键里被找到（比如 "ni" 的前缀扫描
               会同时命中 "ni" 和 "nihao" 两行），去重一下，
               否则候选条里会出现重复项。 */
            if (!out.contains(parts[j])) out.append(parts[j]);
        }
        i++;
    }
    return out;
}
