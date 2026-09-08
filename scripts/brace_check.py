# 括号平衡快查。本机没有交叉编译器，这是编译前能做的最低限度的结构校验：
# 剥掉注释和字符串字面量之后数 {} 和 ()。不平衡一定有问题；平衡不代表一定对。
#
# 必须用状态机逐字符扫，不能用正则分步剥离：第一版先剥行注释再剥字符串，
# 结果 "https://..." 里的 // 被当成注释，把整行后半截连同括号一起吃掉了，
# 报了两个不存在的问题。注释和字符串是互相嵌套的语法，只能一遍扫过去。
import io, glob, sys, os

def count_brackets(src):
    i, n = 0, len(src)
    curly = paren = 0
    while i < n:
        c = src[i]
        nxt = src[i+1] if i + 1 < n else ''
        if c == '/' and nxt == '*':                 # 块注释
            j = src.find('*/', i + 2)
            i = n if j < 0 else j + 2
        elif c == '/' and nxt == '/':               # 行注释
            j = src.find('\n', i)
            i = n if j < 0 else j
        elif c == '"' or c == "'":                  # 字符串/字符字面量
            q, i = c, i + 1
            while i < n and src[i] != q:
                i += 2 if src[i] == '\\' else 1
            i += 1
        else:
            if c == '{': curly += 1
            elif c == '}': curly -= 1
            elif c == '(': paren += 1
            elif c == ')': paren -= 1
            i += 1
    return curly, paren

# 批量改代码时留下的可疑痕迹。这些字符串在正常的 C/C++ 源码里不该出现，
# 出现了基本可以断定是脚本改文件时拼错了（实际发生过两次：Python 的字符串
# 拼接表达式原样写进了源码）。编译器当然也会报错，但那要等到交叉编译那一步，
# 而本机没有交叉编译器——在这里拦下来能省一个来回。
SUSPICIOUS = [
    "' + B + '",      # Python 字符串拼接残留
    '" + B + "',
    'chr(92)',        # 同上，另一种写法
    'PY_MARKER',      # 占位符忘了替换
]


def scan_unterminated(src):
    """找“字符串字面量里混进了裸换行”。

    实际发生过：用 Bash 传 Python heredoc 改代码时，shell 把源码里的
    \r\n 当转义处理掉了，变成真正的换行，printf("...") 当场断成两行。
    括号依然平衡，可疑痕迹也没有，两项旧检查都拦不住，一直要等到
    Keil 报 missing closing quote 才暴露——白跑一个来回。

    必须用状态机扫，不能按 // 切注释："https://..." 这种串里就带 //，
    按字符串切会把后半个引号切掉，全变成误报（第一版就踩了这个）。
    """
    hits = []
    ln = 1
    st = 0          # 0=代码 1=行注释 2=块注释 3=字符串 4=字符
    start = 0
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ''
        if c == chr(10):
            ln += 1
            if st == 1:
                st = 0
            elif st in (3, 4):
                # 字符串/字符里碰到裸换行（行尾反斜杠续行已在下面吃掉）
                hits.append((start, src.split(chr(10))[start - 1].strip()[:70]))
                st = 0
            i += 1
            continue
        if st == 0:
            if c == '/' and nxt == '/': st = 1; i += 2; continue
            if c == '/' and nxt == '*': st = 2; i += 2; continue
            if c == '"':  st = 3; start = ln; i += 1; continue
            if c == "'":  st = 4; start = ln; i += 1; continue
        elif st == 2:
            if c == '*' and nxt == '/': st = 0; i += 2; continue
        elif st == 3:
            if c == chr(92): i += 2; continue
            if c == '"': st = 0; i += 1; continue
        elif st == 4:
            if c == chr(92): i += 2; continue
            if c == "'": st = 0; i += 1; continue
        i += 1
    return hits


def scan_suspicious(path, src):
    hits = []
    for i, line in enumerate(src.split(chr(10)), 1):
        for pat in SUSPICIOUS:
            if pat in line:
                hits.append((i, pat, line.strip()[:70]))
    return hits


root = sys.argv[1] if len(sys.argv) > 1 else '.' 
os.chdir(root)
pats = ['gateway_imx6ull/*.c', 'gateway_imx6ull/*.h',
        'firmware_stm32/*.c', 'firmware_stm32/*.h',
        'firmware_stm32/sensor/*.c', 'firmware_stm32/sensor/*.h',
        'firmware_stm32/bootloader/*.c', 'firmware_stm32/bootloader/*.h',
        'imx6ull_ui/*.cpp', 'imx6ull_ui/*.h']
files = [f for p in pats for f in glob.glob(p)]
bad = 0
susp = 0
for f in sorted(files):
    src = io.open(f, encoding='utf-8', errors='replace').read()
    curly, paren = count_brackets(src)
    if curly or paren:
        print('  [不平衡] %-46s {} %+d   () %+d' % (f, curly, paren))
        bad += 1
    for ln, pat, text in scan_suspicious(f, src):
        print('  [可疑]   %s:%d  含 %r' % (f, ln, pat))
        print('           %s' % text)
        susp += 1
    for ln, text in scan_unterminated(src):
        print('  [断串]   %s:%d  字符串引号没闭合（转义符被吃成真换行？）' % (f, ln))
        print('           %s' % text)
        susp += 1
print('检查 %d 个文件，%s；可疑痕迹 %d 处'
      % (len(files),
         '括号全部平衡' if bad == 0 else '括号不平衡 %d 个文件' % bad,
         susp))
