#!/usr/bin/env bash
# ota_test.sh - OTA 流程自动化验证（对应 docs/项目方案书.md 6.2 表格里的 ota_test.sh）
#
# 两种模式：
#   1. 自测模式（默认，不需要真机）：
#        ./ota_test.sh
#      用 vcan(虚拟 CAN) + scripts/mock_boot（复现 uds.c 的协议状态机，写内存
#      代替写 Flash）跑一遍完整 OTA，用一组"边界固件大小"回归测试之前修过的
#      两个坑：
#        - 固件大小不是 BLOCK_LEN(6) 整数倍时最后一块的收尾逻辑
#        - CRC 校验范围两端要对齐（按实际固件长度，不是整个 496KB App 分区）
#      每个 size 跑完后用 cmp 逐字节比较"mock_boot 收到并落盘的内容"和原始
#      测试固件，比 uds_tool 自己打印"SUCCESS"更可信——它验证的是数据本身
#      有没有一字不差地传过去、写进去。
#
#   2. 真机模式：
#        ./ota_test.sh --real can0 path/to/app.bin
#      直接用真实 CAN 口跑一次 OTA，成功后 candump 几秒确认能收到 0x100
#      状态帧，证明真的跳回 App 在正常工作（而不是停在 Boot 没跳转）。
#
# 依赖：gcc、SocketCAN（vcan 需要 root 权限加载 vcan 内核模块）。
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
GATEWAY_DIR="$PROJ_ROOT/gateway_imx6ull"
WORK_DIR="$(mktemp -d /tmp/ota_test.XXXXXX)"
UDS_TOOL="$WORK_DIR/uds_tool"
MOCK_BOOT="$WORK_DIR/mock_boot"

PASS=0
FAIL=0
FAILED_CASES=()

cleanup() {
    [ -n "${MOCK_PID:-}" ] && kill "$MOCK_PID" >/dev/null 2>&1
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

log()  { printf '%s\n' "$*"; }
ok()   { printf '  \033[32m[PASS]\033[0m %s\n' "$*"; PASS=$((PASS+1)); }
bad()  { printf '  \033[31m[FAIL]\033[0m %s\n' "$*"; FAIL=$((FAIL+1)); FAILED_CASES+=("$*"); }

build() {
    log "== 编译 uds_tool / mock_boot =="
    gcc -O2 -Wall -o "$UDS_TOOL" "$GATEWAY_DIR/uds_tool.c" || { log "uds_tool 编译失败"; exit 1; }
    gcc -O2 -Wall -o "$MOCK_BOOT" "$SCRIPT_DIR/mock_boot.c" || { log "mock_boot 编译失败"; exit 1; }
}

# ============ 真机模式 ============
run_real() {
    local ifname="$1" fw="$2"
    [ -f "$fw" ] || { log "固件文件不存在: $fw"; exit 1; }
    log "== 真机 OTA：$fw -> $ifname =="
    "$UDS_TOOL" "$fw" "$ifname"
    local rc=$?
    if [ $rc -ne 0 ]; then
        bad "uds_tool 升级流程返回非 0 ($rc)"
        exit 1
    fi
    ok "uds_tool 报告升级成功"

    log "-- candump 5 秒，确认 App 已跳转并在正常上报 0x100 --"
    if ! command -v candump >/dev/null 2>&1; then
        log "  (未安装 can-utils，跳过 candump 验证，建议: apt install can-utils)"
        return
    fi
    local dump="$WORK_DIR/candump.log"
    timeout 5 candump "$ifname",100:7FF -n 3 > "$dump" 2>/dev/null
    if [ -s "$dump" ]; then
        ok "跳转后收到 0x100 状态帧，App 在正常运行"
        cat "$dump"
    else
        bad "5 秒内没收到 0x100，App 可能没有成功跳转（或仍停在升级模式）"
    fi
}

# ============ 自测模式：vcan + mock_boot ============
ensure_vcan() {
    local ifn="$1"
    if ip link show "$ifn" >/dev/null 2>&1; then
        return 0
    fi
    log "== 创建虚拟 CAN 接口 $ifn（需要 root）=="
    sudo modprobe vcan 2>/dev/null
    sudo ip link add dev "$ifn" type vcan 2>/dev/null
    sudo ip link set up "$ifn" 2>/dev/null
    if ! ip link show "$ifn" >/dev/null 2>&1; then
        log "创建 $ifn 失败——本机可能没有 vcan 内核模块或没有 sudo 权限。"
        log "可以手动执行："
        log "  sudo modprobe vcan && sudo ip link add dev $ifn type vcan && sudo ip link set up $ifn"
        exit 1
    fi
}

run_case() {
    local ifn="$1" size="$2" expect="$3"   # expect: ok | reject
    local fw="$WORK_DIR/fw_$size.bin"
    local out="$WORK_DIR/out_$size.bin"
    local mocklog="$WORK_DIR/mock_$size.log"

    if [ "$size" -gt 0 ]; then
        head -c "$size" /dev/urandom > "$fw"
    else
        : > "$fw"   # 0 字节，专门测"应该被拒绝"这条路径
    fi

    if [ "$expect" = "reject" ]; then
        # uds_tool 自己在打开 CAN 之前就该按大小拒绝，不需要 mock_boot 陪跑
        "$UDS_TOOL" "$fw" "$ifn" >/dev/null 2>&1
        if [ $? -ne 0 ]; then
            ok "size=$size：超出范围被 uds_tool 正确拒绝"
        else
            bad "size=$size：本应被拒绝，但 uds_tool 报告成功了"
        fi
        return
    fi

    "$MOCK_BOOT" "$ifn" "$out" > "$mocklog" 2>&1 &
    MOCK_PID=$!
    sleep 0.3   # 等 mock_boot 完成 socket bind

    "$UDS_TOOL" "$fw" "$ifn" > "$WORK_DIR/uds_$size.log" 2>&1
    local rc=$?

    # 等 mock_boot 收到 0x11 复位后自行退出（最多等 2 秒）
    local waited=0
    while kill -0 "$MOCK_PID" >/dev/null 2>&1 && [ $waited -lt 20 ]; do
        sleep 0.1; waited=$((waited+1))
    done
    kill "$MOCK_PID" >/dev/null 2>&1
    MOCK_PID=""

    if [ $rc -ne 0 ]; then
        bad "size=$size：uds_tool 返回非 0，日志见 $WORK_DIR/uds_$size.log"
        return
    fi
    if [ ! -s "$out" ] && [ "$size" -gt 0 ]; then
        bad "size=$size：mock_boot 没收到任何数据（可能没触发 0x11 复位）"
        return
    fi
    if cmp -s "$fw" "$out"; then
        ok "size=$size：收发字节完全一致"
    else
        bad "size=$size：mock_boot 落盘内容和原始固件不一致（diff: $(cmp "$fw" "$out" 2>&1)）"
    fi
}

run_selftest() {
    local ifn="vcan0"
    ensure_vcan "$ifn"
    build

    log ""
    log "== 边界固件大小回归测试（BLOCK_LEN=6, MAX_FIRMWARE=65536）=="
    # 6 的整数倍 / 非整数倍都要覆盖，1 和 5 是最容易踩坑的极小值
    for size in 1 5 6 7 11 12 18 61 20000 65536; do
        run_case "$ifn" "$size" ok
    done
    # 0 字节 / 超过 MAX_FIRMWARE，都应该在客户端就被拒绝，不需要真的传输
    run_case "$ifn" 0 reject
    run_case "$ifn" 65537 reject

    log ""
    log "== 结果：通过 $PASS / 失败 $FAIL =="
    if [ $FAIL -gt 0 ]; then
        log "失败用例："
        for c in "${FAILED_CASES[@]}"; do log "  - $c"; done
        exit 1
    fi
    exit 0
}

# ============ 入口 ============
if [ "${1:-}" = "--real" ]; then
    build
    run_real "${2:?用法: ota_test.sh --real <can口> <固件.bin>}" "${3:?用法: ota_test.sh --real <can口> <固件.bin>}"
    [ $FAIL -eq 0 ]
    exit $?
else
    run_selftest
fi
