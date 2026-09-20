# -*- coding: utf-8 -*-
"""
生成板子用的拼音候选词库。

输入：
  jieba 的 dict.txt —— 35 万词条，**带词频**。词频是关键：
  没有它的话，打 "ni" 第一个跳出来的可能是个生僻字，那这个输入法就没法用。
  pypinyin —— 汉字/词组 -> 拼音。它自带词组词典，所以多音字在词里的读音
  基本是对的（"重庆" 会读 chongqing 而不是 zhongqing）。

输出：一个按拼音排序的纯文本文件，每行：
  <无声调全拼> <候选1> <候选2> ...

为什么用排好序的纯文本而不是二进制/JSON：
  板子上要做的是"按前缀查"——输入 ni 要能同时找到 ni / nihao / nihen…
  排好序之后二分一次就能定位前缀区间，不用把整个表建成哈希表塞进内存。
  纯文本还便于出问题时直接 grep 看一眼。
"""
import io, os, sys
import jieba
from pypinyin import lazy_pinyin, Style

DICT = os.path.join(os.path.dirname(jieba.__file__), 'dict.txt')
OUT = sys.argv[1] if len(sys.argv) > 1 else 'pinyin.dict'

MAX_WORD_LEN = 4        # 超过四个字的词，输入法里几乎用不到
MAX_CAND = 6            # 每个拼音最多留几个候选
MIN_FREQ_MULTI = 60     # 多字词的词频门槛，滤掉长尾
MIN_FREQ_SINGLE = 1     # 单字一律保留：单字是兜底，缺了就打不出字

def is_cjk(s):
    return all('一' <= ch <= '鿿' for ch in s)

buckets = {}
kept = 0
with io.open(DICT, encoding='utf-8') as f:
    for line in f:
        parts = line.split()
        if len(parts) < 2:
            continue
        word, freq = parts[0], int(parts[1])
        if not is_cjk(word) or len(word) > MAX_WORD_LEN:
            continue
        if len(word) == 1:
            if freq < MIN_FREQ_SINGLE:
                continue
        elif freq < MIN_FREQ_MULTI:
            continue
        try:
            py = ''.join(lazy_pinyin(word, style=Style.NORMAL))
        except Exception:
            continue
        if not py.isalpha():
            continue
        buckets.setdefault(py, []).append((freq, word))
        kept += 1

lines = []
for py in sorted(buckets):
    cands = sorted(buckets[py], key=lambda t: -t[0])[:MAX_CAND]
    seen, words = set(), []
    for _, w in cands:
        if w not in seen:
            seen.add(w)
            words.append(w)
    lines.append(py + ' ' + ' '.join(words))

with io.open(OUT, 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n'.join(lines) + '\n')

print('收录词条 %d，拼音键 %d，文件 %d KB'
      % (kept, len(lines), os.path.getsize(OUT) // 1024))
for probe in ('ni', 'nihao', 'jiayouzhan', 'nanhang', 'zhongguo'):
    idx = [l for l in lines if l.split(' ', 1)[0] == probe]
    print('  %-12s %s' % (probe, idx[0] if idx else '(无)'))
