# -*- coding: utf-8 -*-
"""原子写入辅助 —— 改文件一律走这里，不要再用 open(p, 'w')。

为什么需要它（真实事故，2026-09-08）：
    io.open(p, 'w') 在**打开的那一刻就把文件截断成 0 字节**，之后才写入。
    如果写入过程中抛异常（那次是字符串里混了一对 surrogate，
    UnicodeEncodeError），文件就永久停在 0 字节——原内容没有任何副本，
    而这个项目不是 git 仓库，没有任何东西能把它找回来。
    docs/系统搭建与启动手册.md 就是这么没的。

    危险的地方在于：**这个失败模式和"脚本没跑成功"看起来一模一样**。
    终端上只有一个 Traceback，很容易以为"报错了所以没改成"，
    实际上文件已经被清空了。

做法：写临时文件 -> 校验 -> os.replace 原子替换。
    os.replace 在同一文件系统上是原子的：要么完全是新内容，
    要么完全是旧内容，不存在中间态。
"""
import io
import os
import sys


def safe_write(path, text, encoding='utf-8'):
    """把 text 原子地写进 path。任何异常都不会损坏原文件。"""
    # 先在内存里试编码。编码错误是最常见的失败原因，
    # 在碰文件之前就把它挡掉。
    data = text.encode(encoding)

    tmp = path + '.tmp_safe_write'
    with io.open(tmp, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())   # 确保落盘，防止断电时留下空文件
    os.replace(tmp, path)      # 原子替换
    return len(data)


def safe_sub(path, old, new, count=1, encoding='utf-8'):
    """读 -> 断言 old 出现 count 次 -> 替换 -> 原子写回。

    断言放在写之前：改错了就什么都不做，而不是写进去一半。
    """
    with io.open(path, encoding=encoding) as f:
        s = f.read()
    n = s.count(old)
    if n != count:
        raise AssertionError(
            '%s: 期望 %d 处匹配，实际 %d 处，未做任何修改' % (path, count, n))
    return safe_write(path, s.replace(old, new), encoding)


if __name__ == '__main__':
    # 自检：故意让写入失败，确认原文件不受影响
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), '_safe_write_selftest.txt')
    safe_write(p, u'原始内容')
    try:
        safe_write(p, u'\ud83d')      # 孤立代理项，必定编码失败
    except Exception as e:
        print('预期的失败:', type(e).__name__)
    with io.open(p, encoding='utf-8') as f:
        got = f.read()
    os.remove(p)
    print('原文件是否完好:', 'OK' if got == u'原始内容' else '损坏了！')
    sys.exit(0 if got == u'原始内容' else 1)
