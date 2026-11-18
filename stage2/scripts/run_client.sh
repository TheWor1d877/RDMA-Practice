#!/usr/bin/env bash
# 编译并运行 client（发送端），stdout/stderr 全部落盘 results/。
# 本机冒烟和云端都可用。
# 用法：./scripts/run_client.sh <server_ip> [-n 1000] [-msgsize 65536] [-dev rxe0]
set -euo pipefail
cd "$(dirname "$0")/.."

[ $# -lt 1 ] && { echo "用法: $0 <server_ip> [tcclient 参数...]"; exit 1; }
SERVER_IP="$1"; shift

mkdir -p results bin
make -s lib
(cd go && /usr/local/go/bin/go build -o ../bin/tcclient ./cmd/tcclient 2>/dev/null \
  || (command -v go >/dev/null && go build -o ../bin/tcclient ./cmd/tcclient))

LOG="results/client_$(date +%m%d_%H%M%S).log"
echo "== client 启动，目标 $SERVER_IP:29999，日志: $LOG"
./bin/tcclient -server "$SERVER_IP:29999" "$@" 2>&1 | tee "$LOG"
