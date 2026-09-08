#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
固件源码同步检查 / 同步工具

背景：STM32 这边的源码在仓库里存了两份——

  firmware_stm32/           给人看的干净源码树，只有业务代码，
                            面试/评审时不用在几百个 HAL 库文件里翻
  EdgeMonitor_App/User/     Keil 实际编译的工程目录（骨架是正点原子官方例程，
  EdgeMonitor_Boot/User/    业务文件混在 BSP/HAL 中间）

两份都要在，但"靠记得手动复制"是不可靠的：改完传感器只同步了一边，
编译出来的还是旧代码，现象却是"改了没生效"，很难往这个方向怀疑。
（DHT11 那轮调试每改一次都要同步两处，OTA 刷进旧固件也是同一类问题。）

用法：
    python scripts/sync_firmware.py            # 只检查，有差异时退出码为 1
    python scripts/sync_firmware.py --push     # 以 firmware_stm32 为准，覆盖 Keil 工程
    python scripts/sync_firmware.py --pull     # 以 Keil 工程为准，回写 firmware_stm32
    python scripts/sync_firmware.py --diff     # 检查并打印具体差异行

编译前跑一次 check，就能挡掉"改了一边"这类问题。
"""

import argparse
import difflib
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# (阅读用源码树, Keil 工程里的对应文件)
PAIRS = [
    ("firmware_stm32/app_task.c",            "EdgeMonitor_App/User/app_task.c"),
    ("firmware_stm32/app_task.h",            "EdgeMonitor_App/User/app_task.h"),
    ("firmware_stm32/can.c",                 "EdgeMonitor_App/User/can.c"),
    ("firmware_stm32/motor.c",               "EdgeMonitor_App/User/motor.c"),
    ("firmware_stm32/motor.h",               "EdgeMonitor_App/User/motor.h"),
    ("firmware_stm32/can.h",                 "EdgeMonitor_App/User/can.h"),
    ("firmware_stm32/types.h",               "EdgeMonitor_App/User/types.h"),
    ("firmware_stm32/fw_meta.h",             "EdgeMonitor_App/User/fw_meta.h"),
    ("firmware_stm32/fw_meta.h",             "EdgeMonitor_Boot/User/fw_meta.h"),
    ("firmware_stm32/sensor/dht11.c",        "EdgeMonitor_App/User/sensor/dht11.c"),
    ("firmware_stm32/sensor/dht11.h",        "EdgeMonitor_App/User/sensor/dht11.h"),
    ("firmware_stm32/sensor/adc_light.c",    "EdgeMonitor_App/User/sensor/adc_light.c"),
    ("firmware_stm32/sensor/adc_light.h",    "EdgeMonitor_App/User/sensor/adc_light.h"),
    ("firmware_stm32/sensor/hcsr04.c",       "EdgeMonitor_App/User/sensor/hcsr04.c"),
    ("firmware_stm32/sensor/hcsr04.h",       "EdgeMonitor_App/User/sensor/hcsr04.h"),
    ("firmware_stm32/sensor/mpu6050.c",      "EdgeMonitor_App/User/sensor/mpu6050.c"),
    ("firmware_stm32/sensor/mpu6050.h",      "EdgeMonitor_App/User/sensor/mpu6050.h"),
    ("firmware_stm32/bootloader/uds.c",      "EdgeMonitor_Boot/User/uds.c"),
    ("firmware_stm32/bootloader/boot_uds.h", "EdgeMonitor_Boot/User/boot_uds.h"),
    ("firmware_stm32/types.h",               "EdgeMonitor_Boot/User/types.h"),
]

# 这两对是"有意不同"的，不参与同步：入口文件的头部注释和 include 路径
# 各自针对所在工程写（Keil 工程要用 ./SYSTEM/xxx 这种相对路径），
# 但函数体应该保持一致，所以仍然检查，只是差异不算错误
EXEMPT = [
    ("firmware_stm32/main.c",                "EdgeMonitor_App/User/main.c",
     "App 入口：firmware_stm32 版是存档参考，两份只有注释措辞不同"),
    ("firmware_stm32/bootloader/boot_main.c", "EdgeMonitor_Boot/User/main.c",
     "Boot 入口：仅头部注释和 include 路径不同，函数体应一致"),
]


def read(p: Path):
    """按字节读，避免换行符差异被平台悄悄改写造成假差异。"""
    return p.read_bytes() if p.exists() else None


def code_only(p: Path):
    """剥掉注释、include 和空行，只留实质代码，用于比对 EXEMPT 的两份入口文件。

    不是完整的 C 解析器，只处理本项目实际用到的几种形式：整行 //、整行块注释、
    跨行块注释、#include、空行。行内尾部注释也一并去掉。"""
    out, in_block = [], False
    for raw in p.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if in_block:
            if "*/" in line:
                line = line.split("*/", 1)[1].strip()
                in_block = False
            else:
                continue
        while "/*" in line:
            head, rest = line.split("/*", 1)
            if "*/" in rest:
                line = (head + " " + rest.split("*/", 1)[1]).strip()
            else:
                line = head.strip()
                in_block = True
                break
        if "//" in line:
            line = line.split("//", 1)[0].strip()
        if not line or line.startswith("#include"):
            continue
        out.append(line)
    return out


def show_diff(a: Path, b: Path):
    ta = a.read_text(encoding="utf-8", errors="replace").splitlines()
    tb = b.read_text(encoding="utf-8", errors="replace").splitlines()
    for line in difflib.unified_diff(ta, tb, str(a), str(b), lineterm="", n=2):
        print("    " + line)


def main():
    ap = argparse.ArgumentParser(description="检查/同步两份 STM32 源码副本")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--push", action="store_true", help="以 firmware_stm32 为准覆盖 Keil 工程")
    g.add_argument("--pull", action="store_true", help="以 Keil 工程为准回写 firmware_stm32")
    ap.add_argument("--diff", action="store_true", help="打印具体差异行")
    args = ap.parse_args()

    missing, differing, synced = [], [], 0

    for src_rel, dst_rel in PAIRS:
        src, dst = ROOT / src_rel, ROOT / dst_rel
        a, b = read(src), read(dst)
        if a is None or b is None:
            # 新增文件时目标侧还不存在，push/pull 应该直接建出来，
            # 而不是只报一句"缺失"让人手动复制
            if args.push and a is not None:
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
                print(f"[新建] {src_rel} -> {dst_rel}")
                synced += 1
                continue
            if args.pull and b is not None:
                src.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(dst, src)
                print(f"[新建] {dst_rel} -> {src_rel}")
                synced += 1
                continue
            missing.append((src_rel, dst_rel, "源缺失" if a is None else "目标缺失"))
            continue
        if a == b:
            synced += 1
            continue

        if args.push or args.pull:
            frm, to = (src, dst) if args.push else (dst, src)
            to.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(frm, to)
            print(f"[同步] {frm.relative_to(ROOT)} -> {to.relative_to(ROOT)}")
            synced += 1
        else:
            differing.append((src_rel, dst_rel))
            if args.diff:
                print(f"\n[不一致] {src_rel}  <->  {dst_rel}")
                show_diff(src, dst)

    print()
    print(f"一致 {synced} 对 / 共 {len(PAIRS)} 对")

    for src_rel, dst_rel, why in missing:
        print(f"[缺失] {why}: {src_rel} <-> {dst_rel}")

    if differing:
        print(f"\n以下 {len(differing)} 对文件不一致：")
        for src_rel, dst_rel in differing:
            print(f"  - {src_rel}\n    {dst_rel}")
        print("\n改完源码后请跑一次同步，否则 Keil 编译的还是旧代码：")
        print("  python scripts/sync_firmware.py --push    (改的是 firmware_stm32)")
        print("  python scripts/sync_firmware.py --pull    (改的是 Keil 工程)")
        if args.diff:
            pass
        else:
            print("  加 --diff 可以看具体差了哪几行")

    # EXEMPT 的两份文件整体不同是正常的（注释、include 路径各写各的），
    # 但函数体必须一致。之前就漏过一次：改了 firmware_stm32 那份的 A/B 逻辑，
    # Keil 工程那份还是旧代码，脚本只提示了个"≠"，直到编译报错才发现。
    # 这里剥掉注释/include/空行之后再比一次，实质代码不同就算错误。
    print("\n有意不同的入口文件（比对实质代码，忽略注释和 include）：")
    exempt_bad = False
    for src_rel, dst_rel, why in EXEMPT:
        src, dst = ROOT / src_rel, ROOT / dst_rel
        if not (src.exists() and dst.exists()):
            print(f"  [?] {src_rel} <-> {dst_rel}  (文件缺失)")
            exempt_bad = True
            continue
        a, b = code_only(src), code_only(dst)
        if a == b:
            print(f"  [=] {src_rel}\n      代码一致（{why}）")
        else:
            exempt_bad = True
            print(f"  [!] {src_rel} <-> {dst_rel}")
            print(f"      代码不一致！这两份的函数体应该保持同步（{why}）")
            if args.diff:
                for line in difflib.unified_diff(a, b, src_rel, dst_rel, lineterm="", n=1):
                    print("      " + line)
            else:
                print("      加 --diff 看具体差异")

    return 1 if (differing or missing or exempt_bad) else 0


if __name__ == "__main__":
    sys.exit(main())
