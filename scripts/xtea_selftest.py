#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
xtea_selftest.py - 校验 Boot 侧 uds.c 与上位机 uds_tool.c 的 XTEA 密钥算法自洽
用法：python3 xtea_selftest.py
输出：若干 seed -> key 测试向量，联调时可用来人工核对两端的计算一致
"""
import random

DELTA = 0x9E3779B9
MASK  = 0xFFFFFFFF

# 与 C 代码 XTEA_KEY0..3 完全一致
KEY = (0x4D474E54, 0x45444F4D, 0x41545741, 0x592B2B2B)

def xtea_encipher(v0, v1, key):
    """复刻 C 端 XTEA_Encipher：64-bit 块，32 轮，无符号 32 位回绕"""
    s = 0
    for _ in range(32):
        v0 = (v0 + ((((v1 << 4) ^ (v1 >> 5)) + v1) ^ (s + key[s & 3]))) & MASK
        s = (s + DELTA) & MASK
        v1 = (v1 + ((((v0 << 4) ^ (v0 >> 5)) + v0) ^ (s + key[(s >> 11) & 3]))) & MASK
    return v0, v1

def uds_calc_key(seed):
    """
    复刻 C 端 UDS_CalcKey：
      blk[0..3] = seed 大端字节，blk[4..7]=0
      (u32*)blk 在小端机器上 v[0]=seed, v[1]=0
      XTEA 加密后，取 v[0] 的字节（内存低地址到高 = 低字节到高字节），
      C 端 key = blk[0]<<24 | blk[1]<<16 | blk[2]<<8 | blk[3]
    """
    e0, _ = xtea_encipher(seed, 0, KEY)
    b0 = e0 & 0xFF
    b1 = (e0 >> 8) & 0xFF
    b2 = (e0 >> 16) & 0xFF
    b3 = (e0 >> 24) & 0xFF
    return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3

if __name__ == "__main__":
    print("XTEA self-test: Boot(uds.c) <-> Tool(uds_tool.c) 算法自洽")
    print("seed  = 复位计数 ^ 0xA5A5 ^ (FlashKB >> 2)")
    print("样例（直接用于联调核对）：\n")
    samples = [0x00000000, 0x12345678, 0xA5A55A5A]
    for _ in range(3):
        samples.append(random.getrandbits(32))
    for s in samples:
        print("  seed=0x%08X  ->  key=0x%08X" % (s, uds_calc_key(s)))
    print("\n两端（uds.c 的 UDS_CalcKey 与 uds_tool.c 的 uds_calc_key）")
    print("必须对同一 seed 算出同一 key，否则 0x27 安全访问将失败。")
