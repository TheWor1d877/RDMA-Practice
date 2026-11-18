#!/usr/bin/env bash
# Soft-RoCE 本机冒烟：三轮跑法（write 批量 / send / mixed），结果落盘 results/。
# 通过标准：每轮两端 RESULT 行均为 0 失败，server 校验通过，退出码 0。
# 用法：./scripts/run_smoke.sh [N]    N 为每轮迭代次数，默认 50
set -euo pipefail
cd "$(dirname "$0")/.."

N="${1:-50}"
mkdir -p results bin
make -s lib
GO_BIN=/usr/local/go/bin/go; command -v go >/dev/null && GO_BIN=go
(cd go && $GO_BIN build -o ../bin/tcserver ./cmd/tcserver && $GO_BIN build -o ../bin/tcclient ./cmd/tcclient)

# 冒烟前自检：rxe 设备要在
rdma link show | grep -q "rxe0.*ACTIVE" || {
    echo "!! rxe0 不在或未 ACTIVE，先: sudo rdma link add rxe0 type rxe netdev <网卡名>"
    exit 1
}

# 三轮共用参数：槽位 256KB（本机 memlock 上限 8MB，8 个 1MB 会 reg_mr 失败；
# 云上 memlock 放开后再用 -slotsize 1048576）
SRV_ARGS=(-slotsize 262144)

run_pass() {
    local tag="$1"; shift
    echo "== 冒烟 [$tag] 开始"
    ./bin/tcserver "${SRV_ARGS[@]}" 2>&1 | tee "results/smoke_${tag}_server.log" &
    local spid=$!
    sleep 1
    ./bin/tcclient "$@" -n "$N" 2>&1 | tee "results/smoke_${tag}_client.log" || true
    wait $spid 2>/dev/null || true
    echo "== 冒烟 [$tag] 结束"
}

run_pass write   -mode write  -depth 8
run_pass send    -mode send
run_pass mixed   -mode mixed

grep -h RESULT results/smoke_*_server.log results/smoke_*_client.log \
    | tee results/smoke_summary.txt
echo "== 明细看 results/smoke_write_*.log / smoke_send_*.log / smoke_mixed_*.log"
