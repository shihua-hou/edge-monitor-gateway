#!/bin/bash
# run.sh - i.MX6ULL 网关一键配置+编译+运行
# 用法: ./run.sh [can0]
# 依赖: 需要 root 权限配置 can0

IFACE="${1:-can0}"

echo "==> 1. 配置 CAN 接口 $IFACE (500K)"
sudo ip link set "$IFACE" type can bitrate 500000
sudo ip link set "$IFACE" up
ip link show "$IFACE"

echo "==> 2. 编译网关程序"
gcc gateway_can.c -o gateway_can || { echo "编译失败(若无gcc请用交叉编译)"; exit 1; }

echo "==> 3. 运行网关(监听 $IFACE)"
./gateway_can "$IFACE"
