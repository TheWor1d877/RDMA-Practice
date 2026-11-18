#!/usr/bin/env bash
# 编译并启动 server（接收端），stdout/stderr 全部落盘 results/。
# 本机冒烟和云端都可用。所有参数原样透传给 tcserver。
# 用法：./scripts/run_server.sh [-dev rxe0] [-listen :29999] [-slots 8] [-slotsize 1048576]
set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p results bin
make -s lib
(cd go && /usr/local/go/bin/go build -o ../bin/tcserver ./cmd/tcserver 2>/dev/null \
  || (command -v go >/dev/null && go build -o ../bin/tcserver ./cmd/tcserver))

LOG="results/server_$(date +%m%d_%H%M%S).log"
echo "== server 启动，日志: $LOG"
./bin/tcserver "$@" 2>&1 | tee "$LOG"
