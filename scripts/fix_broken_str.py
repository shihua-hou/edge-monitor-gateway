# 修复"字符串字面量里的 \n 变成了真换行"这种破损。
#
# 症状：一行以未闭合的双引号结尾，下一行以 ");  或 ", 之类开头。
# C/C++ 里字符串字面量不能跨行，这种代码编译必然失败（missing terminating
# " character），所以只要检出就一定是破损，不会误伤正常代码。
import io, glob, os, sys, re

BS = chr(92)

def unterminated(line):
    """这一行里的双引号是否处于未闭合状态（忽略转义引号和注释里的引号）"""
    i, n, inq = 0, len(line), False
    while i < n:
        c = line[i]
        if inq:
            if c == BS: i += 2; continue
            if c == '"': inq = False
            i += 1
        else:
            if c == '"': inq = True; i += 1
            elif c == '/' and i + 1 < n and line[i+1] == '/': return False
            else: i += 1
    return inq

root = sys.argv[1]
os.chdir(root)
pats = ['gateway_imx6ull/*.c', 'firmware_stm32/*.c', 'firmware_stm32/sensor/*.c',
        'firmware_stm32/bootloader/*.c', 'imx6ull_ui/*.cpp', 'imx6ull_ui/*.h',
        'EdgeMonitor_App/User/*.c', 'EdgeMonitor_App/User/sensor/*.c']
total = 0
for f in sorted({x for p in pats for x in glob.glob(p)}):
    lines = io.open(f, encoding='utf-8', errors='replace').read().split('\n')
    out, i, fixed = [], 0, 0
    while i < len(lines):
        cur = lines[i]
        if i + 1 < len(lines) and unterminated(cur):
            nxt = lines[i + 1]
            # 下一行去掉前导空白后，应当以引号开头（说明字面量在这里接上了）
            if nxt.lstrip().startswith('"'):
                out.append(cur + BS + 'n' + nxt.lstrip())
                i += 2
                fixed += 1
                continue
        out.append(cur)
        i += 1
    if fixed:
        io.open(f, 'w', encoding='utf-8', newline='').write('\n'.join(out))
        print('  [修复] %-45s %d 处' % (f, fixed))
        total += fixed
print('共修复 %d 处断裂的字符串字面量' % total)
